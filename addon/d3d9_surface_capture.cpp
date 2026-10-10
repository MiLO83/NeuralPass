#include "d3d9_surface_capture.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

namespace neuralpass::d3d9_capture {
namespace {

template <typename T> void release(T *&object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}

struct ShaderKey {
    std::string semantic;
    std::uint32_t semantic_index = 0;
    std::uint64_t material = 0;
    int source_stage = -1;
    bool operator==(const ShaderKey &) const = default;
};

struct ShaderKeyHash {
    std::size_t operator()(const ShaderKey &key) const noexcept {
        std::size_t result = std::hash<std::string> {}(key.semantic);
        const auto mix = [&result](auto value) {
            result ^= std::hash<decltype(value)> {}(value) + 0x9e3779b9u +
                (result << 6) + (result >> 2);
        };
        mix(key.semantic_index); mix(key.material); mix(key.source_stage);
        return result;
    }
};

struct ReadbackSlot {
    std::array<IDirect3DSurface9 *, 4> cpu {};
    bool in_flight = false;
    std::uint64_t frame_index = 0;
};

struct ReplacementTexture {
    IDirect3DBaseTexture9 *source = nullptr;
    IDirect3DBaseTexture9 *rejected_source = nullptr;
    IDirect3DTexture9 *texture = nullptr;
    std::vector<ReplacementMip> mips;
    bool dirty = true;
};

struct SourceBinding {
    int stage = -1;
    IDirect3DBaseTexture9 *texture = nullptr;
    IDirect3DTexture9 *texture_2d = nullptr;
    D3DSURFACE_DESC desc {};
    [[nodiscard]] bool valid() const noexcept {
        return stage >= 0 && texture != nullptr && texture_2d != nullptr;
    }
};

void release_source(SourceBinding &binding) {
    release(binding.texture_2d);
    release(binding.texture);
    binding = {};
}

bool source_format(D3DFORMAT format) {
    return format == D3DFMT_A8R8G8B8 || format == D3DFMT_X8R8G8B8 ||
        format == D3DFMT_A8B8G8R8;
}

SourceBinding select_source(IDirect3DDevice9 *device, int override_stage) {
    SourceBinding best;
    std::uint64_t best_score = 0;
    for (DWORD stage = 0; stage < 16; ++stage) {
        IDirect3DBaseTexture9 *base = nullptr;
        if (FAILED(device->GetTexture(stage, &base)) || base == nullptr) continue;
        IDirect3DTexture9 *texture = nullptr;
        base->QueryInterface(__uuidof(IDirect3DTexture9),
                             reinterpret_cast<void **>(&texture));
        if (texture == nullptr) { release(base); continue; }
        D3DSURFACE_DESC desc {};
        if (FAILED(texture->GetLevelDesc(0, &desc)) || desc.MultiSampleType != D3DMULTISAMPLE_NONE ||
            desc.Width < 4 || desc.Height < 4) {
            release(texture); release(base); continue;
        }
        const auto score = static_cast<std::uint64_t>(desc.Width) * desc.Height +
            (static_cast<std::uint64_t>(texture->GetLevelCount()) << 24) +
            ((desc.Usage & D3DUSAGE_RENDERTARGET) == 0 ? (std::uint64_t {1} << 56) : 0);
        const bool selected = override_stage >= 0 &&
            static_cast<int>(stage) == override_stage;
        if (selected || (override_stage < 0 && score > best_score)) {
            release_source(best);
            best = {static_cast<int>(stage), base, texture, desc};
            best_score = score;
            if (selected) break;
        } else {
            release(texture); release(base);
        }
    }
    return best;
}

std::optional<D3DPRIMITIVETYPE> primitive_type(std::uint32_t topology) {
    if (topology < static_cast<std::uint32_t>(D3DPT_POINTLIST) ||
        topology > static_cast<std::uint32_t>(D3DPT_TRIANGLEFAN)) return std::nullopt;
    return static_cast<D3DPRIMITIVETYPE>(topology);
}

std::optional<UINT> primitive_count(D3DPRIMITIVETYPE type, std::uint32_t count) {
    switch (type) {
    case D3DPT_POINTLIST: return count;
    case D3DPT_LINELIST: return count / 2;
    case D3DPT_LINESTRIP: return count >= 2 ? count - 1 : 0;
    case D3DPT_TRIANGLELIST: return count / 3;
    case D3DPT_TRIANGLESTRIP:
    case D3DPT_TRIANGLEFAN: return count >= 3 ? count - 2 : 0;
    default: return std::nullopt;
    }
}

} // namespace

UvSemantic inspect_uv_output(const void *bytecode, std::size_t size) {
    if (bytecode == nullptr || size < sizeof(DWORD) * 2 || size % sizeof(DWORD) != 0)
        return {};
    const auto *tokens = static_cast<const DWORD *>(bytecode);
    const auto count = size / sizeof(DWORD);
    if ((tokens[0] & 0xffff0000u) != 0xfffe0000u ||
        D3DSHADER_VERSION_MAJOR(tokens[0]) < 3) return {};
    std::size_t offset = 1;
    while (offset < count) {
        const DWORD instruction = tokens[offset];
        const auto opcode = instruction & D3DSI_OPCODE_MASK;
        if (opcode == D3DSIO_END) break;
        if (opcode == D3DSIO_COMMENT) {
            const auto words = (instruction & D3DSI_COMMENTSIZE_MASK) >>
                D3DSI_COMMENTSIZE_SHIFT;
            if (words > count - offset - 1) return {};
            offset += 1 + words;
            continue;
        }
        const auto length = (instruction & D3DSI_INSTLENGTH_MASK) >>
            D3DSI_INSTLENGTH_SHIFT;
        if (length > count - offset - 1) return {};
        if (opcode == D3DSIO_DCL && length >= 2) {
            const DWORD usage_token = tokens[offset + 1];
            const DWORD destination = tokens[offset + 2];
            const auto usage = (usage_token & D3DSP_DCL_USAGE_MASK) >>
                D3DSP_DCL_USAGE_SHIFT;
            const auto usage_index = (usage_token & D3DSP_DCL_USAGEINDEX_MASK) >>
                D3DSP_DCL_USAGEINDEX_SHIFT;
            const auto register_type =
                ((destination & D3DSP_REGTYPE_MASK) >> D3DSP_REGTYPE_SHIFT) |
                ((destination & D3DSP_REGTYPE_MASK2) >> D3DSP_REGTYPE_SHIFT2);
            if (usage == D3DDECLUSAGE_TEXCOORD && register_type == D3DSPR_OUTPUT &&
                (destination & D3DSP_WRITEMASK_0) != 0 &&
                (destination & D3DSP_WRITEMASK_1) != 0)
                return {"TEXCOORD", usage_index,
                        destination & D3DSP_REGNUM_MASK};
        }
        offset += 1 + length;
    }
    return {};
}

struct SurfaceCapture::Impl {
    IDirect3DDevice9 *device = nullptr;
    std::array<IDirect3DTexture9 *, 4> targets {};
    std::array<IDirect3DSurface9 *, 4> target_surfaces {};
    std::array<ReadbackSlot, 3> readback;
    std::unordered_map<ShaderKey, IDirect3DPixelShader9 *, ShaderKeyHash> shaders;
    std::unordered_map<std::uint64_t, ReplacementTexture> replacements;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t next_readback = 0;
    std::uint64_t next_frame_index = 1;
    capture::CaptureStatistics stats;
    bool cleared = false;

    ~Impl() {
        for (auto &[key, shader] : shaders) { (void)key; release(shader); }
        release_replacements();
        for (auto &slot : readback) {
            for (auto *&surface : slot.cpu) release(surface);
        }
        for (auto *&surface : target_surfaces) release(surface);
        for (auto *&texture : targets) release(texture);
        release(device);
    }

    void release_replacements() {
        for (auto &[material, entry] : replacements) {
            (void)material;
            release(entry.texture); release(entry.source); release(entry.rejected_source);
        }
        replacements.clear();
    }

    bool clear_targets() {
        std::array<IDirect3DSurface9 *, 4> originals {};
        for (DWORD index = 0; index < originals.size(); ++index)
            device->GetRenderTarget(index, &originals[index]);
        bool success = true;
        for (DWORD index = 0; index < target_surfaces.size(); ++index)
            if (FAILED(device->SetRenderTarget(index, target_surfaces[index]))) success = false;
        if (success && FAILED(device->Clear(0, nullptr, D3DCLEAR_TARGET, 0, 1.0f, 0)))
            success = false;
        for (DWORD index = 0; index < originals.size(); ++index) {
            device->SetRenderTarget(index, originals[index]);
            release(originals[index]);
        }
        if (success) cleared = true;
        return success;
    }

    IDirect3DPixelShader9 *shader(const UvSemantic &uv, std::uint64_t material,
                                  int source_stage) {
        const ShaderKey key {uv.name, uv.index, material, source_stage};
        if (const auto found = shaders.find(key); found != shaders.end()) return found->second;
        if (_stricmp(uv.name.c_str(), "TEXCOORD") != 0) return nullptr;
        std::ostringstream text;
        if (source_stage >= 0)
            text << "sampler2D source_texture : register(s" << source_stage << ");\n";
        text << "struct Input { float2 uv:TEXCOORD" << uv.index << "; };\n"
             << "struct Output { float4 identity:COLOR0; float4 surface:COLOR1; "
                "float4 gradients:COLOR2; float4 source:COLOR3; };\n"
             << "Output main(Input i) { Output o; o.identity=float4("
             << static_cast<std::uint16_t>(material) << ".0,"
             << static_cast<std::uint16_t>(material >> 16) << ".0,"
             << static_cast<std::uint16_t>(material >> 32) << ".0,"
             << static_cast<std::uint16_t>(material >> 48) << ".0);"
                "o.surface=float4(i.uv,-1.0,1.0);"
                "float2 dx=ddx(i.uv); float2 dy=ddy(i.uv);"
                "o.gradients=float4(dx.x,dy.x,dx.y,dy.y);";
        if (source_stage >= 0)
            text << "o.source=tex2D(source_texture,i.uv);"
                    "clip(o.source.a-(0.5/255.0));";
        else
            text << "o.source=float4(0.0,0.0,0.0,-1.0);";
        text << "return o;}";
        const auto source = text.str();
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const auto compiled = D3DCompile(source.data(), source.size(),
            "NeuralPassD3D9Capture", nullptr, nullptr, "main", "ps_3_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
        release(errors);
        if (FAILED(compiled) || bytecode == nullptr) { release(bytecode); return nullptr; }
        IDirect3DPixelShader9 *result = nullptr;
        const auto created = device->CreatePixelShader(
            static_cast<const DWORD *>(bytecode->GetBufferPointer()), &result);
        release(bytecode);
        if (FAILED(created)) return nullptr;
        shaders.emplace(key, result);
        return result;
    }

    IDirect3DTexture9 *replacement(std::uint64_t material, const SourceBinding &binding) {
        const auto found = replacements.find(material);
        if (found == replacements.end() || found->second.mips.empty() || !binding.valid())
            return nullptr;
        auto &entry = found->second;
        if (entry.rejected_source == binding.texture) return nullptr;
        auto reject = [&]() -> IDirect3DTexture9 * {
            release(entry.rejected_source);
            entry.rejected_source = binding.texture;
            entry.rejected_source->AddRef();
            ++stats.rejected_replacements;
            return nullptr;
        };
        if (!source_format(binding.desc.Format) || binding.desc.MultiSampleType != D3DMULTISAMPLE_NONE)
            return reject();
        if (entry.source != binding.texture) {
            release(entry.texture); release(entry.source); release(entry.rejected_source);
            const auto levels = binding.texture_2d->GetLevelCount();
            if (FAILED(device->CreateTexture(binding.desc.Width, binding.desc.Height, levels,
                    0, binding.desc.Format, D3DPOOL_DEFAULT, &entry.texture, nullptr)))
                return reject();
            entry.source = binding.texture;
            entry.source->AddRef();
            entry.dirty = true;
        }
        if (!entry.dirty) return entry.texture;

        IDirect3DTexture9 *upload = nullptr;
        const auto levels = binding.texture_2d->GetLevelCount();
        if (FAILED(device->CreateTexture(binding.desc.Width, binding.desc.Height, levels,
                0, binding.desc.Format, D3DPOOL_SYSTEMMEM, &upload, nullptr)))
            return reject();
        bool copied = true;
        for (UINT level = 0; level < levels; ++level) {
            D3DSURFACE_DESC level_desc {};
            D3DLOCKED_RECT source_lock {}, upload_lock {};
            if (FAILED(binding.texture_2d->GetLevelDesc(level, &level_desc)) ||
                FAILED(binding.texture_2d->LockRect(level, &source_lock, nullptr,
                                                    D3DLOCK_READONLY)) ||
                FAILED(upload->LockRect(level, &upload_lock, nullptr, 0))) {
                if (source_lock.pBits != nullptr) binding.texture_2d->UnlockRect(level);
                copied = false;
                break;
            }
            for (UINT y = 0; y < level_desc.Height; ++y)
                std::memcpy(static_cast<std::uint8_t *>(upload_lock.pBits) +
                                static_cast<std::size_t>(y) * upload_lock.Pitch,
                            static_cast<const std::uint8_t *>(source_lock.pBits) +
                                static_cast<std::size_t>(y) * source_lock.Pitch,
                            static_cast<std::size_t>(level_desc.Width) * 4);
            if (level < entry.mips.size()) {
                const auto &mip = entry.mips[level];
                if (mip.width != 0 && mip.height != 0 &&
                    mip.rgba.size() == static_cast<std::size_t>(mip.width) * mip.height * 4 &&
                    mip.coverage.size() == static_cast<std::size_t>(mip.width) * mip.height) {
                    const bool rgba_memory = binding.desc.Format == D3DFMT_A8B8G8R8;
                    const bool opaque = binding.desc.Format == D3DFMT_X8R8G8B8;
                    for (UINT y = 0; y < level_desc.Height; ++y) {
                        const auto ay = std::min(mip.height - 1,
                            y * mip.height / level_desc.Height);
                        auto *row = static_cast<std::uint8_t *>(upload_lock.pBits) +
                            static_cast<std::size_t>(y) * upload_lock.Pitch;
                        for (UINT x = 0; x < level_desc.Width; ++x) {
                            const auto ax = std::min(mip.width - 1,
                                x * mip.width / level_desc.Width);
                            const auto sample = static_cast<std::size_t>(ay) * mip.width + ax;
                            if (mip.coverage[sample] == 0) continue;
                            const auto *rgba = mip.rgba.data() + sample * 4;
                            auto *out = row + x * 4;
                            out[0] = rgba[rgba_memory ? 0 : 2];
                            out[1] = rgba[1];
                            out[2] = rgba[rgba_memory ? 2 : 0];
                            out[3] = opaque ? 255 : rgba[3];
                        }
                    }
                }
            }
            upload->UnlockRect(level);
            binding.texture_2d->UnlockRect(level);
        }
        if (copied && FAILED(device->UpdateTexture(upload, entry.texture))) copied = false;
        release(upload);
        if (!copied) return reject();
        entry.dirty = false;
        return entry.texture;
    }

    bool target_matches(IDirect3DSurface9 *surface) const {
        if (surface == nullptr) return false;
        D3DSURFACE_DESC desc {};
        return SUCCEEDED(surface->GetDesc(&desc)) && desc.Width == width &&
            desc.Height == height && desc.MultiSampleType == D3DMULTISAMPLE_NONE;
    }

    template <typename Draw>
    bool replay(const UvSemantic &uv, std::uint64_t material, int source_override,
                Draw &&draw) {
        if (device == nullptr || !uv.valid() || material == 0) return false;
        std::array<IDirect3DSurface9 *, 4> original_targets {};
        for (DWORD index = 0; index < original_targets.size(); ++index)
            device->GetRenderTarget(index, &original_targets[index]);
        IDirect3DSurface9 *original_depth = nullptr;
        device->GetDepthStencilSurface(&original_depth);
        if (!target_matches(original_targets[0])) {
            for (auto *&surface : original_targets) release(surface);
            release(original_depth);
            return false;
        }
        IDirect3DStateBlock9 *state = nullptr;
        if (FAILED(device->CreateStateBlock(D3DSBT_ALL, &state)) ||
            FAILED(state->Capture())) {
            release(state);
            for (auto *&surface : original_targets) release(surface);
            release(original_depth);
            return false;
        }
        if (!cleared && !clear_targets()) {
            state->Apply(); release(state);
            for (auto *&surface : original_targets) release(surface);
            release(original_depth);
            return false;
        }

        auto source = select_source(device, source_override);
        auto *replacement_texture = replacement(material, source);
        if (replacement_texture != nullptr) {
            device->SetTexture(static_cast<DWORD>(source.stage), replacement_texture);
            ++stats.replacement_draws;
        }
        const auto application_result = draw();
        if (replacement_texture != nullptr)
            device->SetTexture(static_cast<DWORD>(source.stage), source.texture);
        if (FAILED(application_result)) {
            state->Apply(); release(state); release_source(source);
            for (auto *&surface : original_targets) release(surface);
            release(original_depth);
            return false;
        }

        auto *capture_shader = shader(uv, material, source.valid() ? source.stage : -1);
        bool captured = capture_shader != nullptr;
        for (DWORD index = 0; captured && index < target_surfaces.size(); ++index)
            captured = SUCCEEDED(device->SetRenderTarget(index, target_surfaces[index]));
        if (captured) {
            device->SetPixelShader(capture_shader);
            device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_SEPARATEALPHABLENDENABLE, FALSE);
            device->SetRenderState(D3DRS_COLORWRITEENABLE, 0xf);
            device->SetRenderState(D3DRS_COLORWRITEENABLE1, 0xf);
            device->SetRenderState(D3DRS_COLORWRITEENABLE2, 0xf);
            device->SetRenderState(D3DRS_COLORWRITEENABLE3, 0xf);
            device->SetRenderState(D3DRS_ZWRITEENABLE, FALSE);
            if (original_depth != nullptr) device->SetRenderState(D3DRS_ZFUNC, D3DCMP_EQUAL);
            else device->SetRenderState(D3DRS_ZENABLE, FALSE);
            device->SetRenderState(D3DRS_STENCILWRITEMASK, 0);
            captured = SUCCEEDED(draw());
            if (captured) ++stats.replayed_draws;
        }
        state->Apply();
        for (DWORD index = 0; index < original_targets.size(); ++index)
            device->SetRenderTarget(index, original_targets[index]);
        device->SetDepthStencilSurface(original_depth);
        release(state); release_source(source);
        for (auto *&surface : original_targets) release(surface);
        release(original_depth);
        // The application draw was executed once, so suppress the wrapper draw
        // even if this particular geometry could not enter the capture planes.
        return true;
    }
};

SurfaceCapture::~SurfaceCapture() { reset(); }
capture::GraphicsBackend SurfaceCapture::backend() const noexcept {
    return capture::GraphicsBackend::d3d9;
}
capture::CaptureCapabilities SurfaceCapture::capabilities() const noexcept {
    return {.direct_draws=true, .indexed_draws=true, .indirect_draws=false,
            .replacement_textures=true, .asynchronous_readback=false,
            .shader_coverage_preserved=false};
}
bool SurfaceCapture::initialize(void *native_device, std::uint32_t width,
                                std::uint32_t height) {
    return initialize(static_cast<IDirect3DDevice9 *>(native_device), width, height);
}

bool SurfaceCapture::initialize(IDirect3DDevice9 *device, std::uint32_t width,
                                std::uint32_t height) {
    reset();
    if (device == nullptr || width == 0 || height == 0) return false;
    D3DCAPS9 caps {};
    if (FAILED(device->GetDeviceCaps(&caps)) || caps.NumSimultaneousRTs < 4 ||
        caps.PixelShaderVersion < D3DPS_VERSION(3, 0) ||
        caps.VertexShaderVersion < D3DVS_VERSION(3, 0)) return false;
    auto *implementation = new Impl;
    implementation->device = device;
    device->AddRef();
    implementation->width = width;
    implementation->height = height;
    for (std::size_t index = 0; index < implementation->targets.size(); ++index) {
        if (FAILED(device->CreateTexture(width, height, 1, D3DUSAGE_RENDERTARGET,
                D3DFMT_A32B32G32R32F, D3DPOOL_DEFAULT,
                &implementation->targets[index], nullptr)) ||
            FAILED(implementation->targets[index]->GetSurfaceLevel(
                0, &implementation->target_surfaces[index]))) {
            delete implementation; return false;
        }
    }
    for (auto &slot : implementation->readback) {
        for (std::size_t index = 0; index < slot.cpu.size(); ++index) {
            if (FAILED(device->CreateOffscreenPlainSurface(width, height,
                    D3DFMT_A32B32G32R32F, D3DPOOL_SYSTEMMEM,
                    &slot.cpu[index], nullptr))) {
                delete implementation; return false;
            }
        }
    }
    impl_ = implementation;
    return true;
}

void SurfaceCapture::reset() { delete impl_; impl_ = nullptr; }

bool SurfaceCapture::replay(void *, const UvSemantic &uv, std::uint64_t material,
                            const capture::DrawCommand &draw, int source_override) {
    if (impl_ == nullptr || draw.instance_count == 0) return false;
    const auto topology = primitive_type(draw.primitive_topology);
    if (!topology) return false;
    const auto primitives = primitive_count(*topology, draw.vertex_or_index_count);
    if (!primitives || *primitives == 0) return false;
    switch (draw.kind) {
    case capture::DrawKind::direct:
        return impl_->replay(uv, material, source_override, [&] {
            return impl_->device->DrawPrimitive(*topology,
                draw.first_vertex_or_index, *primitives);
        });
    case capture::DrawKind::indexed: {
        IDirect3DVertexBuffer9 *vertices = nullptr;
        UINT stream_offset = 0, stride = 0;
        if (FAILED(impl_->device->GetStreamSource(
                0, &vertices, &stream_offset, &stride)) || vertices == nullptr || stride == 0) {
            release(vertices); return false;
        }
        D3DVERTEXBUFFER_DESC desc {};
        const bool valid = SUCCEEDED(vertices->GetDesc(&desc));
        release(vertices);
        if (!valid || stream_offset >= desc.Size) return false;
        const auto total_vertices = (desc.Size - stream_offset) / stride;
        const auto minimum = draw.vertex_offset < 0
            ? static_cast<UINT>(-static_cast<std::int64_t>(draw.vertex_offset)) : 0u;
        const auto absolute_start = static_cast<std::int64_t>(draw.vertex_offset) + minimum;
        if (absolute_start < 0 || static_cast<std::uint64_t>(absolute_start) >= total_vertices)
            return false;
        const auto vertex_count = total_vertices - static_cast<UINT>(absolute_start);
        return impl_->replay(uv, material, source_override, [&] {
            return impl_->device->DrawIndexedPrimitive(*topology, draw.vertex_offset,
                minimum, vertex_count, draw.first_vertex_or_index, *primitives);
        });
    }
    default:
        return false;
    }
}

std::optional<SurfaceCaptureFrame> SurfaceCapture::finish_frame(void *, bool schedule_next) {
    return finish_frame(schedule_next);
}

std::optional<SurfaceCaptureFrame> SurfaceCapture::finish_frame(bool schedule_next) {
    if (impl_ == nullptr) return std::nullopt;
    std::optional<SurfaceCaptureFrame> result;
    for (auto &slot : impl_->readback) {
        if (!slot.in_flight) continue;
        bool copied = true;
        std::array<D3DLOCKED_RECT, 4> mapped {};
        std::array<bool, 4> locked {};
        for (std::size_t index = 0; copied && index < slot.cpu.size(); ++index) {
            locked[index] = SUCCEEDED(slot.cpu[index]->LockRect(
                &mapped[index], nullptr, D3DLOCK_READONLY));
            copied = locked[index];
        }
        if (copied) {
            SurfaceCaptureFrame frame(impl_->width, impl_->height, slot.frame_index);
            const auto nan = std::numeric_limits<float>::quiet_NaN();
            for (std::uint32_t y = 0; y < impl_->height; ++y) {
                std::array<const std::uint8_t *, 4> rows {};
                for (std::size_t index = 0; index < rows.size(); ++index)
                    rows[index] = static_cast<const std::uint8_t *>(mapped[index].pBits) +
                        static_cast<std::size_t>(y) * mapped[index].Pitch;
                for (std::uint32_t x = 0; x < impl_->width; ++x) {
                    const auto *identity = reinterpret_cast<const float *>(rows[0]) + x * 4;
                    const auto *surface = reinterpret_cast<const float *>(rows[1]) + x * 4;
                    const auto *gradient = reinterpret_cast<const float *>(rows[2]) + x * 4;
                    const auto *source = reinterpret_cast<const float *>(rows[3]) + x * 4;
                    auto &pixel = frame.pixels().at(x, y);
                    const auto chunk = [](float value) {
                        return static_cast<std::uint64_t>(std::clamp(
                            std::lround(value), 0l, 65535l));
                    };
                    pixel.material_id = chunk(identity[0]) | (chunk(identity[1]) << 16) |
                        (chunk(identity[2]) << 32) | (chunk(identity[3]) << 48);
                    pixel.u = surface[0]; pixel.v = surface[1];
                    pixel.confidence = pixel.material_id == 0 ? 0.0f : surface[3];
                    pixel.du_dx = gradient[0]; pixel.du_dy = gradient[1];
                    pixel.dv_dx = gradient[2]; pixel.dv_dy = gradient[3];
                    if (source[3] < 0.0f) {
                        pixel.source_r = pixel.source_g = pixel.source_b = pixel.source_a = nan;
                    } else {
                        pixel.source_r = source[0]; pixel.source_g = source[1];
                        pixel.source_b = source[2]; pixel.source_a = source[3];
                    }
                    pixel.framebuffer_depth = pixel.hit_depth = nan;
                }
            }
            result = std::move(frame);
        }
        for (std::size_t index = 0; index < slot.cpu.size(); ++index)
            if (locked[index]) slot.cpu[index]->UnlockRect();
        slot.in_flight = false;
    }
    if (!schedule_next) return result;
    auto &next = impl_->readback[impl_->next_readback];
    if (!next.in_flight) {
        bool queued = impl_->cleared;
        for (std::size_t index = 0; queued && index < next.cpu.size(); ++index)
            queued = SUCCEEDED(impl_->device->GetRenderTargetData(
                impl_->target_surfaces[index], next.cpu[index]));
        if (queued) {
            next.in_flight = true;
            next.frame_index = impl_->next_frame_index++;
            impl_->next_readback = (impl_->next_readback + 1) % impl_->readback.size();
        }
    } else {
        ++impl_->stats.dropped_frames;
    }
    if (impl_->cleared) {
        impl_->cleared = false;
        impl_->clear_targets();
    }
    return result;
}

capture::CaptureStatistics SurfaceCapture::statistics() const noexcept {
    return impl_ != nullptr ? impl_->stats : capture::CaptureStatistics {};
}
std::uint32_t SurfaceCapture::width() const noexcept { return impl_ != nullptr ? impl_->width : 0; }
std::uint32_t SurfaceCapture::height() const noexcept { return impl_ != nullptr ? impl_->height : 0; }
void SurfaceCapture::queue_replacement(std::uint64_t material,
                                       std::vector<ReplacementMip> mips) {
    if (impl_ == nullptr || material == 0 || mips.empty()) return;
    auto &entry = impl_->replacements[material];
    release(entry.rejected_source);
    entry.mips = std::move(mips);
    entry.dirty = true;
}
void SurfaceCapture::clear_replacements() {
    if (impl_ != nullptr) impl_->release_replacements();
}

} // namespace neuralpass::d3d9_capture
