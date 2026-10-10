#include "d3d10_surface_capture.hpp"

#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <string>
#include <unordered_map>

namespace neuralpass::d3d10_capture {
namespace {

template <typename T> void release(T *&object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}

struct ShaderKey {
    std::string semantic;
    std::uint32_t semantic_index = 0;
    std::uint32_t register_index = 0;
    std::uint64_t material_id = 0;
    int source_slot = -1;
    int sampler_slot = -1;
    bool operator==(const ShaderKey &) const = default;
};

struct ShaderKeyHash {
    std::size_t operator()(const ShaderKey &key) const noexcept {
        std::size_t result = std::hash<std::string> {}(key.semantic);
        const auto mix = [&result](auto value) {
            result ^= std::hash<decltype(value)> {}(value) + 0x9e3779b9u +
                (result << 6) + (result >> 2);
        };
        mix(key.semantic_index); mix(key.register_index); mix(key.material_id);
        mix(key.source_slot); mix(key.sampler_slot);
        return result;
    }
};

struct DepthVariant {
    ID3D10DepthStencilState *source = nullptr;
    ID3D10DepthStencilState *capture = nullptr;
};

struct ReadbackSlot {
    ID3D10Texture2D *identity = nullptr;
    ID3D10Texture2D *gradients = nullptr;
    ID3D10Texture2D *source = nullptr;
    ID3D10Texture2D *depth = nullptr;
    ID3D10Query *query = nullptr;
    bool in_flight = false;
    std::uint64_t frame_index = 0;
};

struct ReplacementTexture {
    ID3D10Resource *source = nullptr;
    ID3D10Resource *rejected_source = nullptr;
    ID3D10Texture2D *texture = nullptr;
    ID3D10ShaderResourceView *view = nullptr;
    std::vector<ReplacementMip> mips;
    bool dirty = true;
};

struct SourceBinding {
    int texture_slot = -1;
    int sampler_slot = -1;
    [[nodiscard]] bool valid() const noexcept {
        return texture_slot >= 0 && sampler_slot >= 0;
    }
};

constexpr std::array<float, 4> k_clear {};
const std::array<float, 4> k_missing {
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN()};

bool color_sample_format(DXGI_FORMAT format) {
    switch (format) {
    case DXGI_FORMAT_R8G8B8A8_UNORM:
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8A8_UNORM:
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB:
    case DXGI_FORMAT_B8G8R8X8_UNORM:
    case DXGI_FORMAT_B8G8R8X8_UNORM_SRGB:
    case DXGI_FORMAT_BC1_UNORM:
    case DXGI_FORMAT_BC1_UNORM_SRGB:
    case DXGI_FORMAT_BC2_UNORM:
    case DXGI_FORMAT_BC2_UNORM_SRGB:
    case DXGI_FORMAT_BC3_UNORM:
    case DXGI_FORMAT_BC3_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

SourceBinding select_source_binding(ID3D10Device *device, int override_slot) {
    std::array<ID3D10ShaderResourceView *, D3D10_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views {};
    std::array<ID3D10SamplerState *, D3D10_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers {};
    device->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    device->PSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    int best_slot = -1;
    std::uint64_t best_score = 0;
    for (std::size_t slot = 0; slot < views.size(); ++slot) {
        auto *view = views[slot];
        if (view == nullptr) continue;
        D3D10_SHADER_RESOURCE_VIEW_DESC view_desc {};
        view->GetDesc(&view_desc);
        if (view_desc.ViewDimension != D3D10_SRV_DIMENSION_TEXTURE2D ||
            !color_sample_format(view_desc.Format)) continue;
        ID3D10Resource *resource = nullptr;
        ID3D10Texture2D *texture = nullptr;
        view->GetResource(&resource);
        if (resource != nullptr)
            resource->QueryInterface(__uuidof(ID3D10Texture2D),
                                     reinterpret_cast<void **>(&texture));
        release(resource);
        if (texture == nullptr) continue;
        D3D10_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        release(texture);
        if (desc.SampleDesc.Count != 1 || desc.Width < 4 || desc.Height < 4) continue;
        const auto score = static_cast<std::uint64_t>(desc.Width) * desc.Height +
            (static_cast<std::uint64_t>(desc.MipLevels) << 24) +
            (((desc.BindFlags & (D3D10_BIND_RENDER_TARGET | D3D10_BIND_DEPTH_STENCIL)) == 0)
                ? (std::uint64_t {1} << 56) : 0);
        if (override_slot >= 0) {
            if (static_cast<int>(slot) == override_slot) best_slot = override_slot;
        } else if (score > best_score) {
            best_score = score;
            best_slot = static_cast<int>(slot);
        }
    }
    int sampler_slot = -1;
    if (best_slot >= 0 && best_slot < static_cast<int>(samplers.size()) &&
        samplers[static_cast<std::size_t>(best_slot)] != nullptr)
        sampler_slot = best_slot;
    if (sampler_slot < 0)
        for (std::size_t slot = 0; slot < samplers.size(); ++slot)
            if (samplers[slot] != nullptr) { sampler_slot = static_cast<int>(slot); break; }
    for (auto *&view : views) release(view);
    for (auto *&sampler : samplers) release(sampler);
    return {best_slot, sampler_slot};
}

} // namespace

struct SurfaceCapture::Impl {
    ID3D10Device *device = nullptr;
    ID3D10Texture2D *identity = nullptr;
    ID3D10RenderTargetView *identity_view = nullptr;
    ID3D10Texture2D *gradients = nullptr;
    ID3D10RenderTargetView *gradients_view = nullptr;
    ID3D10Texture2D *source = nullptr;
    ID3D10RenderTargetView *source_view = nullptr;
    ID3D10Texture2D *depth = nullptr;
    ID3D10RenderTargetView *depth_view = nullptr;
    std::array<ReadbackSlot, 3> readback;
    std::unordered_map<ShaderKey, ID3D10PixelShader *, ShaderKeyHash> shaders;
    std::unordered_map<ID3D10DepthStencilState *, DepthVariant> depth_variants;
    std::unordered_map<std::uint64_t, ReplacementTexture> replacements;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t next_readback = 0;
    std::uint64_t next_frame_index = 1;
    capture::CaptureStatistics stats;
    bool cleared = false;

    ~Impl() {
        for (auto &[key, shader] : shaders) { (void)key; release(shader); }
        for (auto &[key, variant] : depth_variants) {
            (void)key; release(variant.source); release(variant.capture);
        }
        release_replacements();
        for (auto &slot : readback) {
            release(slot.identity); release(slot.gradients); release(slot.source);
            release(slot.depth); release(slot.query);
        }
        release(identity_view); release(identity);
        release(gradients_view); release(gradients);
        release(source_view); release(source);
        release(depth_view); release(depth);
        release(device);
    }

    void release_replacements() {
        for (auto &[material, entry] : replacements) {
            (void)material;
            release(entry.view); release(entry.texture); release(entry.source);
            release(entry.rejected_source);
        }
        replacements.clear();
    }

    ID3D10PixelShader *shader(const UvSemantic &uv, std::uint64_t material,
                              SourceBinding binding) {
        const ShaderKey key {uv.name, uv.index, uv.register_index, material,
                             binding.texture_slot, binding.sampler_slot};
        if (const auto found = shaders.find(key); found != shaders.end()) return found->second;
        if (uv.name.empty() || !std::all_of(uv.name.begin(), uv.name.end(),
                [](unsigned char value) { return std::isalnum(value) != 0 || value == '_'; }))
            return nullptr;
        std::ostringstream text;
        if (binding.valid())
            text << "Texture2D<float4> source_texture : register(t" << binding.texture_slot
                 << "); SamplerState source_sampler : register(s" << binding.sampler_slot
                 << ");\n";
        text << "struct Input { float4 position : SV_Position; float2 uv : "
             << uv.name << uv.index << "; };\n"
             << "struct Output { uint4 surface : SV_Target0; float4 gradients : SV_Target1; "
                "float4 source : SV_Target2; float depth : SV_Target3; };\n"
             << "Output main(Input input) { Output o; o.surface=uint4("
             << static_cast<std::uint32_t>(material) << "u,"
             << static_cast<std::uint32_t>(material >> 32)
             << "u,asuint(input.uv.x),asuint(input.uv.y));"
                "float2 dx=ddx(input.uv); float2 dy=ddy(input.uv);"
                "o.gradients=float4(dx.x,dy.x,dx.y,dy.y); o.depth=input.position.z;";
        if (binding.valid())
            text << "o.source=source_texture.Sample(source_sampler,input.uv);"
                    "clip(o.source.a-(0.5f/255.0f));";
        else
            text << "o.source=float4(asfloat(0x7fc00000u),asfloat(0x7fc00000u),"
                    "asfloat(0x7fc00000u),asfloat(0x7fc00000u));";
        text << "return o;}";
        const auto source_text = text.str();
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const auto compiled = D3DCompile(source_text.data(), source_text.size(),
            "NeuralPassD3D10Capture", nullptr, nullptr, "main", "ps_4_0",
            D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &bytecode, &errors);
        release(errors);
        if (FAILED(compiled) || bytecode == nullptr) { release(bytecode); return nullptr; }
        ID3D11ShaderReflection *reflection = nullptr;
        bool linkage_matches = false;
        if (SUCCEEDED(D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                __uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(&reflection)))) {
            D3D11_SHADER_DESC desc {};
            if (SUCCEEDED(reflection->GetDesc(&desc)))
                for (UINT index = 0; index < desc.InputParameters; ++index) {
                    D3D11_SIGNATURE_PARAMETER_DESC parameter {};
                    if (SUCCEEDED(reflection->GetInputParameterDesc(index, &parameter)) &&
                        parameter.SemanticName != nullptr &&
                        _stricmp(parameter.SemanticName, uv.name.c_str()) == 0 &&
                        parameter.SemanticIndex == uv.index &&
                        parameter.Register == uv.register_index) {
                        linkage_matches = true;
                        break;
                    }
                }
        }
        release(reflection);
        if (!linkage_matches) { release(bytecode); return nullptr; }
        ID3D10PixelShader *result = nullptr;
        const auto created = device->CreatePixelShader(
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), &result);
        release(bytecode);
        if (FAILED(created)) return nullptr;
        shaders.emplace(key, result);
        return result;
    }

    ID3D10DepthStencilState *capture_depth(ID3D10DepthStencilState *original) {
        if (const auto found = depth_variants.find(original); found != depth_variants.end())
            return found->second.capture;
        D3D10_DEPTH_STENCIL_DESC desc {};
        if (original != nullptr) original->GetDesc(&desc);
        else {
            desc.DepthEnable = TRUE;
            desc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ALL;
            desc.DepthFunc = D3D10_COMPARISON_LESS;
            desc.StencilReadMask = D3D10_DEFAULT_STENCIL_READ_MASK;
            desc.StencilWriteMask = D3D10_DEFAULT_STENCIL_WRITE_MASK;
        }
        desc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ZERO;
        if (desc.DepthEnable) desc.DepthFunc = D3D10_COMPARISON_EQUAL;
        desc.FrontFace.StencilPassOp = D3D10_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFailOp = D3D10_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP;
        desc.BackFace.StencilPassOp = D3D10_STENCIL_OP_KEEP;
        desc.BackFace.StencilFailOp = D3D10_STENCIL_OP_KEEP;
        desc.BackFace.StencilDepthFailOp = D3D10_STENCIL_OP_KEEP;
        ID3D10DepthStencilState *capture_state = nullptr;
        if (FAILED(device->CreateDepthStencilState(&desc, &capture_state))) return nullptr;
        if (original != nullptr) original->AddRef();
        depth_variants.emplace(original, DepthVariant {original, capture_state});
        return capture_state;
    }

    bool target_matches(ID3D10RenderTargetView *view) const {
        if (view == nullptr) return false;
        ID3D10Resource *resource = nullptr;
        ID3D10Texture2D *texture = nullptr;
        view->GetResource(&resource);
        if (resource != nullptr)
            resource->QueryInterface(__uuidof(ID3D10Texture2D),
                                     reinterpret_cast<void **>(&texture));
        release(resource);
        if (texture == nullptr) return false;
        D3D10_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        release(texture);
        return desc.Width == width && desc.Height == height && desc.SampleDesc.Count == 1;
    }

    ID3D10ShaderResourceView *replacement(std::uint64_t material,
        SourceBinding binding, ID3D10ShaderResourceView *source_srv) {
        const auto found = replacements.find(material);
        if (found == replacements.end() || !binding.valid() || source_srv == nullptr ||
            found->second.mips.empty()) return nullptr;
        auto &entry = found->second;
        ID3D10Resource *source_resource = nullptr;
        ID3D10Texture2D *source_texture = nullptr;
        source_srv->GetResource(&source_resource);
        if (source_resource != nullptr)
            source_resource->QueryInterface(__uuidof(ID3D10Texture2D),
                                            reinterpret_cast<void **>(&source_texture));
        if (source_texture == nullptr) { release(source_resource); return nullptr; }
        if (entry.rejected_source == source_resource) {
            release(source_texture); release(source_resource); return nullptr;
        }
        D3D10_TEXTURE2D_DESC desc {};
        D3D10_SHADER_RESOURCE_VIEW_DESC view_desc {};
        source_texture->GetDesc(&desc);
        source_srv->GetDesc(&view_desc);
        const bool byte_color = view_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            view_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        auto reject = [&]() -> ID3D10ShaderResourceView * {
            release(entry.rejected_source);
            entry.rejected_source = source_resource;
            entry.rejected_source->AddRef();
            ++stats.rejected_replacements;
            release(source_texture); release(source_resource);
            return nullptr;
        };
        if (!byte_color || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
            view_desc.ViewDimension != D3D10_SRV_DIMENSION_TEXTURE2D)
            return reject();
        if (entry.source != source_resource) {
            release(entry.view); release(entry.texture); release(entry.source);
            release(entry.rejected_source);
            auto replacement_desc = desc;
            replacement_desc.Usage = D3D10_USAGE_DEFAULT;
            replacement_desc.CPUAccessFlags = 0;
            replacement_desc.BindFlags |= D3D10_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&replacement_desc, nullptr, &entry.texture)) ||
                FAILED(device->CreateShaderResourceView(entry.texture, &view_desc, &entry.view))) {
                release(entry.view);
                release(entry.texture);
                return reject();
            }
            entry.source = source_resource;
            source_resource->AddRef();
            entry.dirty = true;
        }
        if (entry.dirty) {
            device->CopyResource(entry.texture, source_texture);
            auto patch_desc = desc;
            patch_desc.Usage = D3D10_USAGE_DEFAULT;
            patch_desc.BindFlags = 0;
            patch_desc.CPUAccessFlags = 0;
            patch_desc.MiscFlags = 0;
            ID3D10Texture2D *patch_texture = nullptr;
            if (FAILED(device->CreateTexture2D(&patch_desc, nullptr, &patch_texture)))
                return reject();
            const bool bgra = view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
                view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
            const auto first_mip = view_desc.Texture2D.MostDetailedMip;
            const auto view_levels = view_desc.Texture2D.MipLevels == UINT_MAX
                ? desc.MipLevels - first_mip : view_desc.Texture2D.MipLevels;
            const auto levels = std::min<std::size_t>(view_levels, entry.mips.size());
            for (std::size_t level = 0; level < levels; ++level) {
                const auto &mip = entry.mips[level];
                if (mip.width == 0 || mip.height == 0 ||
                    mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4 ||
                    mip.coverage.size() != static_cast<std::size_t>(mip.width) * mip.height)
                    continue;
                const auto subresource = first_mip + static_cast<UINT>(level);
                const auto target_width = std::max(1u, desc.Width >> subresource);
                const auto target_height = std::max(1u, desc.Height >> subresource);
                std::vector<std::uint8_t> patch(
                    static_cast<std::size_t>(target_width) * target_height * 4);
                for (std::uint32_t y = 0; y < target_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / target_height);
                    for (std::uint32_t x = 0; x < target_width; ++x) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / target_width);
                        const auto sample = static_cast<std::size_t>(ay) * mip.width + ax;
                        const auto *rgba = mip.rgba.data() + sample * 4;
                        auto *out = patch.data() +
                            (static_cast<std::size_t>(y) * target_width + x) * 4;
                        out[0] = rgba[bgra ? 2 : 0]; out[1] = rgba[1];
                        out[2] = rgba[bgra ? 0 : 2]; out[3] = rgba[3];
                    }
                }
                device->UpdateSubresource(patch_texture, subresource, nullptr,
                                          patch.data(), target_width * 4, 0);
                for (std::uint32_t y = 0; y < target_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / target_height);
                    std::uint32_t x = 0;
                    while (x < target_width) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / target_width);
                        if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + ax] == 0) {
                            ++x; continue;
                        }
                        const auto begin = x++;
                        while (x < target_width) {
                            const auto sample_x = std::min(
                                mip.width - 1, x * mip.width / target_width);
                            if (mip.coverage[static_cast<std::size_t>(ay) * mip.width +
                                             sample_x] == 0) break;
                            ++x;
                        }
                        const D3D10_BOX box {begin, y, 0, x, y + 1, 1};
                        device->CopySubresourceRegion(entry.texture, subresource,
                            begin, y, 0, patch_texture, subresource, &box);
                    }
                }
            }
            release(patch_texture);
            entry.dirty = false;
        }
        release(source_texture); release(source_resource);
        return entry.view;
    }

    template <typename Draw>
    bool replay(const UvSemantic &uv, std::uint64_t material, int source_override,
                Draw &&draw) {
        if (device == nullptr || identity_view == nullptr || !uv.valid() || material == 0)
            return false;
        std::array<ID3D10RenderTargetView *, D3D10_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
        ID3D10DepthStencilView *dsv = nullptr;
        device->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
        std::uint32_t rtv_count = 0;
        for (std::uint32_t index = 0; index < rtvs.size(); ++index)
            if (rtvs[index] != nullptr) rtv_count = index + 1;
        if (rtv_count == 0 || !target_matches(rtvs[0])) {
            for (auto *&rtv : rtvs) release(rtv);
            release(dsv);
            return false;
        }
        if (!cleared) {
            device->ClearRenderTargetView(identity_view, k_clear.data());
            device->ClearRenderTargetView(gradients_view, k_clear.data());
            device->ClearRenderTargetView(source_view, k_missing.data());
            device->ClearRenderTargetView(depth_view, k_missing.data());
            cleared = true;
        }
        const auto source_binding = select_source_binding(device, source_override);
        ID3D10ShaderResourceView *original_source = nullptr;
        if (source_binding.texture_slot >= 0)
            device->PSGetShaderResources(static_cast<UINT>(source_binding.texture_slot),
                                         1, &original_source);
        auto *replacement_view = replacement(material, source_binding, original_source);
        if (replacement_view != nullptr) {
            device->PSSetShaderResources(static_cast<UINT>(source_binding.texture_slot),
                                         1, &replacement_view);
            ++stats.replacement_draws;
        }
        draw();
        if (replacement_view != nullptr)
            device->PSSetShaderResources(static_cast<UINT>(source_binding.texture_slot),
                                         1, &original_source);

        ID3D10PixelShader *original_shader = nullptr;
        ID3D10BlendState *original_blend = nullptr;
        ID3D10DepthStencilState *original_depth = nullptr;
        FLOAT blend_factor[4] {};
        UINT sample_mask = 0;
        UINT stencil_reference = 0;
        device->PSGetShader(&original_shader);
        device->OMGetBlendState(&original_blend, blend_factor, &sample_mask);
        device->OMGetDepthStencilState(&original_depth, &stencil_reference);
        auto *capture_shader = shader(uv, material, source_binding);
        auto *capture_depth_state = capture_depth(original_depth);
        if (capture_shader != nullptr && capture_depth_state != nullptr) {
            ID3D10RenderTargetView *capture_targets[4] {
                identity_view, gradients_view, source_view, depth_view};
            device->OMSetRenderTargets(4, capture_targets, dsv);
            device->OMSetBlendState(nullptr, nullptr, UINT_MAX);
            device->OMSetDepthStencilState(capture_depth_state, stencil_reference);
            device->PSSetShader(capture_shader);
            draw();
            ++stats.replayed_draws;
        }
        device->PSSetShader(original_shader);
        device->OMSetDepthStencilState(original_depth, stencil_reference);
        device->OMSetBlendState(original_blend, blend_factor, sample_mask);
        device->OMSetRenderTargets(rtv_count, rtvs.data(), dsv);
        release(original_shader); release(original_blend); release(original_depth);
        release(original_source);
        for (auto *&rtv : rtvs) release(rtv);
        release(dsv);
        return true;
    }
};

SurfaceCapture::~SurfaceCapture() { reset(); }

capture::GraphicsBackend SurfaceCapture::backend() const noexcept {
    return capture::GraphicsBackend::d3d10;
}

capture::CaptureCapabilities SurfaceCapture::capabilities() const noexcept {
    return {.direct_draws=true, .indexed_draws=true, .indirect_draws=false,
            .replacement_textures=true, .asynchronous_readback=true,
            .shader_coverage_preserved=false};
}

bool SurfaceCapture::initialize(void *native_device, std::uint32_t width,
                                std::uint32_t height) {
    return initialize(static_cast<ID3D10Device *>(native_device), width, height);
}

bool SurfaceCapture::initialize(ID3D10Device *device, std::uint32_t width,
                                std::uint32_t height) {
    reset();
    if (device == nullptr || width == 0 || height == 0) return false;
    auto *implementation = new Impl;
    implementation->device = device;
    device->AddRef();
    implementation->width = width;
    implementation->height = height;
    D3D10_TEXTURE2D_DESC desc {};
    desc.Width = width; desc.Height = height; desc.MipLevels = 1; desc.ArraySize = 1;
    desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    desc.SampleDesc.Count = 1; desc.Usage = D3D10_USAGE_DEFAULT;
    desc.BindFlags = D3D10_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&desc, nullptr, &implementation->identity)) ||
        FAILED(device->CreateRenderTargetView(
            implementation->identity, nullptr, &implementation->identity_view))) {
        delete implementation; return false;
    }
    auto float_desc = desc;
    float_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (FAILED(device->CreateTexture2D(&float_desc, nullptr, &implementation->gradients)) ||
        FAILED(device->CreateRenderTargetView(
            implementation->gradients, nullptr, &implementation->gradients_view)) ||
        FAILED(device->CreateTexture2D(&float_desc, nullptr, &implementation->source)) ||
        FAILED(device->CreateRenderTargetView(
            implementation->source, nullptr, &implementation->source_view))) {
        delete implementation; return false;
    }
    auto depth_desc = desc;
    depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device->CreateTexture2D(&depth_desc, nullptr, &implementation->depth)) ||
        FAILED(device->CreateRenderTargetView(
            implementation->depth, nullptr, &implementation->depth_view))) {
        delete implementation; return false;
    }
    auto staging_identity = desc;
    staging_identity.Usage = D3D10_USAGE_STAGING;
    staging_identity.BindFlags = 0;
    staging_identity.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    auto staging_float = float_desc;
    staging_float.Usage = D3D10_USAGE_STAGING;
    staging_float.BindFlags = 0;
    staging_float.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    auto staging_depth = depth_desc;
    staging_depth.Usage = D3D10_USAGE_STAGING;
    staging_depth.BindFlags = 0;
    staging_depth.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    const D3D10_QUERY_DESC query_desc {D3D10_QUERY_EVENT, 0};
    for (auto &slot : implementation->readback) {
        if (FAILED(device->CreateTexture2D(&staging_identity, nullptr, &slot.identity)) ||
            FAILED(device->CreateTexture2D(&staging_float, nullptr, &slot.gradients)) ||
            FAILED(device->CreateTexture2D(&staging_float, nullptr, &slot.source)) ||
            FAILED(device->CreateTexture2D(&staging_depth, nullptr, &slot.depth)) ||
            FAILED(device->CreateQuery(&query_desc, &slot.query))) {
            delete implementation; return false;
        }
    }
    impl_ = implementation;
    return true;
}

void SurfaceCapture::reset() { delete impl_; impl_ = nullptr; }

bool SurfaceCapture::replay(void *, const UvSemantic &uv, std::uint64_t material,
                            const capture::DrawCommand &draw, int source_override) {
    if (impl_ == nullptr) return false;
    switch (draw.kind) {
    case capture::DrawKind::direct:
        return impl_->replay(uv, material, source_override, [&] {
            impl_->device->DrawInstanced(draw.vertex_or_index_count, draw.instance_count,
                                         draw.first_vertex_or_index, draw.first_instance);
        });
    case capture::DrawKind::indexed:
        return impl_->replay(uv, material, source_override, [&] {
            impl_->device->DrawIndexedInstanced(draw.vertex_or_index_count,
                draw.instance_count, draw.first_vertex_or_index,
                draw.vertex_offset, draw.first_instance);
        });
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
        if (!slot.in_flight || slot.query->GetData(nullptr, 0,
                D3D10_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        D3D10_MAPPED_TEXTURE2D identity {}, gradients {}, source {}, depth {};
        const auto identity_result = slot.identity->Map(0, D3D10_MAP_READ, 0, &identity);
        const auto gradients_result = slot.gradients->Map(0, D3D10_MAP_READ, 0, &gradients);
        const auto source_result = slot.source->Map(0, D3D10_MAP_READ, 0, &source);
        const auto depth_result = slot.depth->Map(0, D3D10_MAP_READ, 0, &depth);
        if (SUCCEEDED(identity_result) && SUCCEEDED(gradients_result) &&
            SUCCEEDED(source_result) && SUCCEEDED(depth_result)) {
            SurfaceCaptureFrame frame(impl_->width, impl_->height, slot.frame_index);
            for (std::uint32_t y = 0; y < impl_->height; ++y) {
                const auto *identity_row = static_cast<const std::uint8_t *>(identity.pData) +
                    static_cast<std::size_t>(y) * identity.RowPitch;
                const auto *gradient_row = static_cast<const std::uint8_t *>(gradients.pData) +
                    static_cast<std::size_t>(y) * gradients.RowPitch;
                const auto *source_row = static_cast<const std::uint8_t *>(source.pData) +
                    static_cast<std::size_t>(y) * source.RowPitch;
                const auto *depth_row = static_cast<const std::uint8_t *>(depth.pData) +
                    static_cast<std::size_t>(y) * depth.RowPitch;
                for (std::uint32_t x = 0; x < impl_->width; ++x) {
                    const auto *encoded = reinterpret_cast<const std::uint32_t *>(identity_row) + x * 4;
                    const auto *gradient = reinterpret_cast<const float *>(gradient_row) + x * 4;
                    const auto *sample = reinterpret_cast<const float *>(source_row) + x * 4;
                    auto &pixel = frame.pixels().at(x, y);
                    pixel.material_id = static_cast<std::uint64_t>(encoded[0]) |
                        (static_cast<std::uint64_t>(encoded[1]) << 32);
                    std::memcpy(&pixel.u, encoded + 2, sizeof(float));
                    std::memcpy(&pixel.v, encoded + 3, sizeof(float));
                    pixel.confidence = pixel.material_id == 0 ? 0.0f : 1.0f;
                    pixel.du_dx = gradient[0]; pixel.du_dy = gradient[1];
                    pixel.dv_dx = gradient[2]; pixel.dv_dy = gradient[3];
                    pixel.source_r = sample[0]; pixel.source_g = sample[1];
                    pixel.source_b = sample[2]; pixel.source_a = sample[3];
                    pixel.framebuffer_depth = reinterpret_cast<const float *>(depth_row)[x];
                    pixel.hit_depth = pixel.framebuffer_depth;
                }
            }
            result = std::move(frame);
        }
        if (SUCCEEDED(depth_result)) slot.depth->Unmap(0);
        if (SUCCEEDED(source_result)) slot.source->Unmap(0);
        if (SUCCEEDED(gradients_result)) slot.gradients->Unmap(0);
        if (SUCCEEDED(identity_result)) slot.identity->Unmap(0);
        slot.in_flight = false;
    }
    if (!schedule_next) return result;
    auto &next = impl_->readback[impl_->next_readback];
    if (!next.in_flight) {
        if (!impl_->cleared) {
            impl_->device->ClearRenderTargetView(impl_->identity_view, k_clear.data());
            impl_->device->ClearRenderTargetView(impl_->gradients_view, k_clear.data());
            impl_->device->ClearRenderTargetView(impl_->source_view, k_missing.data());
            impl_->device->ClearRenderTargetView(impl_->depth_view, k_missing.data());
            impl_->cleared = true;
        }
        impl_->device->CopyResource(next.identity, impl_->identity);
        impl_->device->CopyResource(next.gradients, impl_->gradients);
        impl_->device->CopyResource(next.source, impl_->source);
        impl_->device->CopyResource(next.depth, impl_->depth);
        next.query->End();
        next.in_flight = true;
        next.frame_index = impl_->next_frame_index++;
        impl_->next_readback = (impl_->next_readback + 1) % impl_->readback.size();
    } else {
        ++impl_->stats.dropped_frames;
    }
    impl_->device->ClearRenderTargetView(impl_->identity_view, k_clear.data());
    impl_->device->ClearRenderTargetView(impl_->gradients_view, k_clear.data());
    impl_->device->ClearRenderTargetView(impl_->source_view, k_missing.data());
    impl_->device->ClearRenderTargetView(impl_->depth_view, k_missing.data());
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

} // namespace neuralpass::d3d10_capture
