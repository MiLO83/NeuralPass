#include "d3d11_surface_capture.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
#include <limits>
#include <sstream>
#include <unordered_map>

namespace neuralpass::d3d11_capture {
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
        result ^= std::hash<std::uint32_t> {}(key.semantic_index) + 0x9e3779b9u +
            (result << 6) + (result >> 2);
        result ^= std::hash<std::uint32_t> {}(key.register_index) + 0x9e3779b9u +
            (result << 6) + (result >> 2);
        result ^= std::hash<std::uint64_t> {}(key.material_id) + 0x9e3779b9u +
            (result << 6) + (result >> 2);
        result ^= std::hash<int> {}(key.source_slot) + 0x9e3779b9u +
            (result << 6) + (result >> 2);
        result ^= std::hash<int> {}(key.sampler_slot) + 0x9e3779b9u +
            (result << 6) + (result >> 2);
        return result;
    }
};

struct DepthVariant {
    ID3D11DepthStencilState *source = nullptr;
    ID3D11DepthStencilState *capture = nullptr;
};

struct ReadbackSlot {
    ID3D11Texture2D *texture = nullptr;
    ID3D11Texture2D *gradients = nullptr;
    ID3D11Texture2D *source = nullptr;
    ID3D11Texture2D *depth = nullptr;
    ID3D11Query *query = nullptr;
    bool in_flight = false;
    std::uint64_t frame_index = 0;
};

struct ReplacementTexture {
    ID3D11Resource *source = nullptr;
    ID3D11Resource *rejected_source = nullptr;
    ID3D11Texture2D *texture = nullptr;
    ID3D11ShaderResourceView *view = nullptr;
    std::vector<ReplacementMip> mips;
    bool dirty = true;
};

constexpr std::array<float, 4> k_clear {0.0f, 0.0f, 0.0f, 0.0f};
const std::array<float, 4> k_missing_source {
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN(),
    std::numeric_limits<float>::quiet_NaN()};

struct SourceBinding {
    int texture_slot = -1;
    int sampler_slot = -1;
    [[nodiscard]] bool valid() const noexcept {
        return texture_slot >= 0 && sampler_slot >= 0;
    }
};

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
    case DXGI_FORMAT_BC7_UNORM:
    case DXGI_FORMAT_BC7_UNORM_SRGB:
    case DXGI_FORMAT_R16G16B16A16_FLOAT:
    case DXGI_FORMAT_R32G32B32A32_FLOAT:
        return true;
    default:
        return false;
    }
}

SourceBinding select_source_binding(ID3D11DeviceContext *context,
                                    int texture_override) {
    std::array<ID3D11ShaderResourceView *, D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT> views {};
    std::array<ID3D11SamplerState *, D3D11_COMMONSHADER_SAMPLER_SLOT_COUNT> samplers {};
    context->PSGetShaderResources(0, static_cast<UINT>(views.size()), views.data());
    context->PSGetSamplers(0, static_cast<UINT>(samplers.size()), samplers.data());
    int best_slot = -1;
    std::uint64_t best_score = 0;
    for (std::size_t slot = 0; slot < views.size(); ++slot) {
        auto *view = views[slot];
        if (view == nullptr) continue;
        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
        view->GetDesc(&view_desc);
        if (view_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D ||
            !color_sample_format(view_desc.Format)) continue;
        ID3D11Resource *resource = nullptr;
        ID3D11Texture2D *texture = nullptr;
        view->GetResource(&resource);
        if (resource != nullptr)
            resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                     reinterpret_cast<void **>(&texture));
        release(resource);
        if (texture == nullptr) continue;
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        release(texture);
        if (desc.SampleDesc.Count != 1 || desc.Width < 4 || desc.Height < 4) continue;
        std::uint64_t score = static_cast<std::uint64_t>(desc.Width) * desc.Height;
        score += static_cast<std::uint64_t>(desc.MipLevels) << 24;
        if ((desc.BindFlags & (D3D11_BIND_RENDER_TARGET | D3D11_BIND_DEPTH_STENCIL)) == 0)
            score += std::uint64_t {1} << 56;
        if (texture_override >= 0) {
            if (static_cast<int>(slot) == texture_override) best_slot = texture_override;
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
    ID3D11Device *device = nullptr;
    ID3D11Texture2D *target = nullptr;
    ID3D11RenderTargetView *target_view = nullptr;
    ID3D11Texture2D *gradient_target = nullptr;
    ID3D11RenderTargetView *gradient_target_view = nullptr;
    ID3D11Texture2D *source_target = nullptr;
    ID3D11RenderTargetView *source_target_view = nullptr;
    ID3D11Texture2D *depth_target = nullptr;
    ID3D11RenderTargetView *depth_target_view = nullptr;
    std::array<ReadbackSlot, 3> readback;
    std::unordered_map<ShaderKey, ID3D11PixelShader *, ShaderKeyHash> shaders;
    std::unordered_map<ID3D11DepthStencilState *, DepthVariant> depth_variants;
    std::unordered_map<std::uint64_t, ReplacementTexture> replacements;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t next_readback = 0;
    std::uint64_t next_frame_index = 1;
    std::uint64_t replayed_draws = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t replacement_draws = 0;
    std::uint64_t rejected_replacements = 0;
    bool target_cleared = false;

    ID3D11ShaderResourceView *replacement(
        ID3D11DeviceContext *context, std::uint64_t material_id,
        SourceBinding source_binding, ID3D11ShaderResourceView *source_view) {
        const auto found = replacements.find(material_id);
        if (found == replacements.end() || !source_binding.valid() || source_view == nullptr ||
            found->second.mips.empty()) return nullptr;
        auto &entry = found->second;
        ID3D11Resource *source_resource = nullptr;
        ID3D11Texture2D *source_texture = nullptr;
        source_view->GetResource(&source_resource);
        if (source_resource != nullptr)
            source_resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                             reinterpret_cast<void **>(&source_texture));
        if (source_texture == nullptr) {
            release(source_resource);
            return nullptr;
        }
        if (entry.rejected_source == source_resource) {
            release(source_texture);
            release(source_resource);
            return nullptr;
        }
        D3D11_TEXTURE2D_DESC desc {};
        source_texture->GetDesc(&desc);
        D3D11_SHADER_RESOURCE_VIEW_DESC view_desc {};
        source_view->GetDesc(&view_desc);
        const bool byte_color = view_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM ||
            view_desc.Format == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB ||
            view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM ||
            view_desc.Format == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
        if (!byte_color || desc.SampleDesc.Count != 1 || desc.ArraySize != 1 ||
            view_desc.ViewDimension != D3D11_SRV_DIMENSION_TEXTURE2D) {
            release(entry.rejected_source);
            entry.rejected_source = source_resource;
            entry.rejected_source->AddRef();
            ++rejected_replacements;
            release(source_texture);
            release(source_resource);
            return nullptr;
        }
        if (entry.source != source_resource) {
            release(entry.view);
            release(entry.texture);
            release(entry.source);
            release(entry.rejected_source);
            D3D11_TEXTURE2D_DESC replacement_desc = desc;
            replacement_desc.Usage = D3D11_USAGE_DEFAULT;
            replacement_desc.CPUAccessFlags = 0;
            replacement_desc.BindFlags |= D3D11_BIND_SHADER_RESOURCE;
            if (FAILED(device->CreateTexture2D(&replacement_desc, nullptr, &entry.texture)) ||
                FAILED(device->CreateShaderResourceView(entry.texture, &view_desc, &entry.view))) {
                release(entry.view);
                release(entry.texture);
                entry.rejected_source = source_resource;
                entry.rejected_source->AddRef();
                ++rejected_replacements;
                release(source_texture);
                release(source_resource);
                return nullptr;
            }
            entry.source = source_resource;
            source_resource->AddRef();
            entry.dirty = true;
        }
        if (entry.dirty) {
            context->CopyResource(entry.texture, source_texture);
            D3D11_TEXTURE2D_DESC patch_desc = desc;
            patch_desc.Usage = D3D11_USAGE_DEFAULT;
            patch_desc.BindFlags = 0;
            patch_desc.CPUAccessFlags = 0;
            patch_desc.MiscFlags = 0;
            ID3D11Texture2D *patch_texture = nullptr;
            if (FAILED(device->CreateTexture2D(&patch_desc, nullptr, &patch_texture))) {
                release(entry.rejected_source);
                entry.rejected_source = source_resource;
                entry.rejected_source->AddRef();
                ++rejected_replacements;
                release(source_texture);
                release(source_resource);
                return nullptr;
            }
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
                const auto source_width = std::max(1u, desc.Width >> subresource);
                const auto source_height = std::max(1u, desc.Height >> subresource);
                std::vector<std::uint8_t> patch(
                    static_cast<std::size_t>(source_width) * source_height * 4);
                for (std::uint32_t y = 0; y < source_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / source_height);
                    for (std::uint32_t x = 0; x < source_width; ++x) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / source_width);
                        const auto sample = static_cast<std::size_t>(ay) * mip.width + ax;
                        const auto *rgba = mip.rgba.data() + sample * 4;
                        auto *destination = patch.data() +
                            (static_cast<std::size_t>(y) * source_width + x) * 4;
                        destination[0] = rgba[bgra ? 2 : 0];
                        destination[1] = rgba[1];
                        destination[2] = rgba[bgra ? 0 : 2];
                        destination[3] = rgba[3];
                    }
                }
                context->UpdateSubresource(patch_texture, subresource, nullptr, patch.data(),
                                           source_width * 4, 0);
                for (std::uint32_t y = 0; y < source_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / source_height);
                    std::uint32_t x = 0;
                    while (x < source_width) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / source_width);
                        if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + ax] == 0) {
                            ++x;
                            continue;
                        }
                        const auto begin = x++;
                        while (x < source_width) {
                            const auto sample_x = std::min(
                                mip.width - 1, x * mip.width / source_width);
                            if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + sample_x] == 0)
                                break;
                            ++x;
                        }
                        const D3D11_BOX box {begin, y, 0, x, y + 1, 1};
                        context->CopySubresourceRegion(entry.texture, subresource,
                            begin, y, 0, patch_texture, subresource, &box);
                    }
                }
            }
            release(patch_texture);
            entry.dirty = false;
        }
        release(source_texture);
        release(source_resource);
        return entry.view;
    }

    ID3D11PixelShader *shader(const UvSemantic &uv, std::uint64_t material_id,
                             SourceBinding source_binding) {
        const ShaderKey key {uv.name, uv.index, uv.register_index, material_id,
                             source_binding.texture_slot, source_binding.sampler_slot};
        if (const auto known = shaders.find(key); known != shaders.end()) return known->second;

        if (uv.name.empty() || !std::all_of(uv.name.begin(), uv.name.end(), [](unsigned char value) {
                return std::isalnum(value) != 0 || value == '_';
            })) return nullptr;
        std::ostringstream source;
        if (source_binding.valid())
            source << "Texture2D<float4> source_texture : register(t"
                   << source_binding.texture_slot << "); SamplerState source_sampler : register(s"
                   << source_binding.sampler_slot << ");\n";
        // SV_Position occupies input register zero in ordinary rasterized
        // vertex outputs. Declaring it keeps the selected TEXCOORD on the same
        // linkage register as the game's vertex shader rather than accidentally
        // reading clip-space XY as UV.
        source << "struct Input { float4 position : SV_Position; float2 uv : "
               << uv.name << uv.index << "; };\n"
               << "struct Output { uint4 surface : SV_Target0; float4 gradients : SV_Target1; "
               << "float4 source : SV_Target2; float depth : SV_Target3; };\n"
               << "Output main(Input input) { Output output; output.surface = uint4("
               << static_cast<std::uint32_t>(material_id) << "u,"
               << static_cast<std::uint32_t>(material_id >> 32) << "u,"
               << "asuint(input.uv.x),asuint(input.uv.y)); "
               << "float2 dx = ddx(input.uv); float2 dy = ddy(input.uv); "
               << "output.gradients = float4(dx.x,dy.x,dx.y,dy.y); "
               << "output.depth = input.position.z; "
               << (source_binding.valid()
                    ? "output.source = source_texture.Sample(source_sampler,input.uv); "
                      "clip(output.source.a - (0.5f/255.0f)); "
                    : "output.source = float4(asfloat(0x7fc00000u),asfloat(0x7fc00000u),"
                      "asfloat(0x7fc00000u),asfloat(0x7fc00000u)); ")
               << "return output; }\n";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const auto text = source.str();
        const HRESULT compiled = D3DCompile(text.data(), text.size(), "NeuralPassCapture",
            nullptr, nullptr, "main", "ps_5_0", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        release(errors);
        if (FAILED(compiled) || bytecode == nullptr) {
            release(bytecode);
            return nullptr;
        }
        ID3D11ShaderReflection *reflection = nullptr;
        bool linkage_matches = false;
        if (SUCCEEDED(D3DReflect(bytecode->GetBufferPointer(), bytecode->GetBufferSize(),
                __uuidof(ID3D11ShaderReflection), reinterpret_cast<void **>(&reflection)))) {
            D3D11_SHADER_DESC desc {};
            if (SUCCEEDED(reflection->GetDesc(&desc))) {
                for (UINT input = 0; input < desc.InputParameters; ++input) {
                    D3D11_SIGNATURE_PARAMETER_DESC parameter {};
                    if (SUCCEEDED(reflection->GetInputParameterDesc(input, &parameter)) &&
                        parameter.SemanticName != nullptr &&
                        _stricmp(parameter.SemanticName, uv.name.c_str()) == 0 &&
                        parameter.SemanticIndex == uv.index &&
                        parameter.Register == uv.register_index) {
                        linkage_matches = true;
                        break;
                    }
                }
            }
        }
        release(reflection);
        if (!linkage_matches) {
            release(bytecode);
            return nullptr;
        }
        ID3D11PixelShader *result = nullptr;
        const HRESULT created = device->CreatePixelShader(
            bytecode->GetBufferPointer(), bytecode->GetBufferSize(), nullptr, &result);
        release(bytecode);
        if (FAILED(created)) return nullptr;
        shaders.emplace(key, result);
        return result;
    }

    ID3D11DepthStencilState *capture_depth(ID3D11DepthStencilState *source) {
        if (source == nullptr) return nullptr;
        if (const auto known = depth_variants.find(source); known != depth_variants.end())
            return known->second.capture;
        D3D11_DEPTH_STENCIL_DESC desc {};
        source->GetDesc(&desc);
        desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
        if (desc.DepthEnable) desc.DepthFunc = D3D11_COMPARISON_EQUAL;
        desc.FrontFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.FrontFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace.StencilPassOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace.StencilFailOp = D3D11_STENCIL_OP_KEEP;
        desc.BackFace.StencilDepthFailOp = D3D11_STENCIL_OP_KEEP;
        ID3D11DepthStencilState *capture = nullptr;
        if (FAILED(device->CreateDepthStencilState(&desc, &capture))) return nullptr;
        source->AddRef();
        depth_variants.emplace(source, DepthVariant {source, capture});
        return capture;
    }

    bool target_matches(ID3D11RenderTargetView *view) const {
        if (view == nullptr) return false;
        ID3D11Resource *resource = nullptr;
        view->GetResource(&resource);
        ID3D11Texture2D *texture = nullptr;
        if (resource != nullptr) resource->QueryInterface(__uuidof(ID3D11Texture2D),
                                                          reinterpret_cast<void **>(&texture));
        release(resource);
        if (texture == nullptr) return false;
        D3D11_TEXTURE2D_DESC desc {};
        texture->GetDesc(&desc);
        release(texture);
        return desc.Width == width && desc.Height == height && desc.SampleDesc.Count == 1;
    }

    template <typename Draw>
    bool replay(ID3D11DeviceContext *context, const UvSemantic &uv,
                std::uint64_t material_id, int source_texture_override, Draw &&draw) {
        if (context == nullptr || target_view == nullptr || !uv.valid() || material_id == 0)
            return false;
        D3D11_DEVICE_CONTEXT_TYPE type = context->GetType();
        if (type != D3D11_DEVICE_CONTEXT_IMMEDIATE) return false;

        std::array<ID3D11RenderTargetView *, D3D11_SIMULTANEOUS_RENDER_TARGET_COUNT> rtvs {};
        std::array<ID3D11UnorderedAccessView *, D3D11_PS_CS_UAV_REGISTER_COUNT> uavs {};
        ID3D11DepthStencilView *dsv = nullptr;
        context->OMGetRenderTargets(static_cast<UINT>(rtvs.size()), rtvs.data(), &dsv);
        context->OMGetRenderTargetsAndUnorderedAccessViews(
            0, nullptr, nullptr, 0, static_cast<UINT>(uavs.size()), uavs.data());
        std::uint32_t rtv_count = 0;
        for (std::uint32_t index = 0; index < rtvs.size(); ++index)
            if (rtvs[index] != nullptr) rtv_count = index + 1;
        if (rtv_count == 0 || !target_matches(rtvs[0])) {
            for (auto *&rtv : rtvs) release(rtv);
            for (auto *&uav : uavs) release(uav);
            release(dsv);
            return false;
        }
        if (!target_cleared) {
            context->ClearRenderTargetView(target_view, k_clear.data());
            context->ClearRenderTargetView(gradient_target_view, k_clear.data());
            context->ClearRenderTargetView(source_target_view, k_missing_source.data());
            context->ClearRenderTargetView(depth_target_view, k_missing_source.data());
            target_cleared = true;
        }
        std::uint32_t uav_count = 0;
        for (std::uint32_t index = rtv_count; index < uavs.size(); ++index)
            if (uavs[index] != nullptr) uav_count = index + 1;

        const auto source_binding = select_source_binding(context, source_texture_override);
        ID3D11ShaderResourceView *original_source = nullptr;
        if (source_binding.texture_slot >= 0)
            context->PSGetShaderResources(static_cast<UINT>(source_binding.texture_slot), 1,
                                          &original_source);
        auto *replacement_view = replacement(
            context, material_id, source_binding, original_source);
        if (replacement_view != nullptr)
            context->PSSetShaderResources(static_cast<UINT>(source_binding.texture_slot), 1,
                                          &replacement_view);
        if (replacement_view != nullptr) ++replacement_draws;
        // Execute the application's draw first. The callback returns true after
        // a successful replay so ReShade does not execute it a second time.
        draw();
        if (replacement_view != nullptr)
            context->PSSetShaderResources(static_cast<UINT>(source_binding.texture_slot), 1,
                                          &original_source);

        ID3D11PixelShader *original_pixel_shader = nullptr;
        context->PSGetShader(&original_pixel_shader, nullptr, nullptr);
        ID3D11BlendState *original_blend = nullptr;
        FLOAT blend_factor[4] {};
        UINT sample_mask = 0;
        context->OMGetBlendState(&original_blend, blend_factor, &sample_mask);
        ID3D11DepthStencilState *original_depth = nullptr;
        UINT stencil_reference = 0;
        context->OMGetDepthStencilState(&original_depth, &stencil_reference);

        auto *capture_shader = shader(uv, material_id, source_binding);
        auto *capture_depth_state = capture_depth(original_depth);
        if (capture_shader != nullptr && (original_depth == nullptr || capture_depth_state != nullptr)) {
            ID3D11RenderTargetView *capture_targets[4] {
                target_view, gradient_target_view, source_target_view, depth_target_view};
            context->OMSetRenderTargets(4, capture_targets, dsv);
            context->OMSetBlendState(nullptr, nullptr, UINT_MAX);
            context->OMSetDepthStencilState(capture_depth_state, stencil_reference);
            context->PSSetShader(capture_shader, nullptr, 0);
            draw();
            ++replayed_draws;
        }

        context->PSSetShader(original_pixel_shader, nullptr, 0);
        context->OMSetDepthStencilState(original_depth, stencil_reference);
        context->OMSetBlendState(original_blend, blend_factor, sample_mask);
        if (uav_count > rtv_count) {
            std::array<UINT, D3D11_PS_CS_UAV_REGISTER_COUNT> initial_counts {};
            initial_counts.fill(UINT_MAX);
            context->OMSetRenderTargetsAndUnorderedAccessViews(
                rtv_count, rtvs.data(), dsv, rtv_count, uav_count - rtv_count,
                uavs.data() + rtv_count, initial_counts.data() + rtv_count);
        } else {
            context->OMSetRenderTargets(rtv_count, rtvs.data(), dsv);
        }

        release(original_pixel_shader);
        release(original_blend);
        release(original_depth);
        release(original_source);
        for (auto *&rtv : rtvs) release(rtv);
        for (auto *&uav : uavs) release(uav);
        release(dsv);
        return true;
    }

    ~Impl() {
        for (auto &[key, shader] : shaders) {
            (void)key;
            release(shader);
        }
        for (auto &[source, variant] : depth_variants) {
            (void)source;
            release(variant.source);
            release(variant.capture);
        }
        for (auto &[material_id, replacement] : replacements) {
            (void)material_id;
            release(replacement.view);
            release(replacement.texture);
            release(replacement.source);
            release(replacement.rejected_source);
        }
        for (auto &slot : readback) {
            release(slot.texture);
            release(slot.gradients);
            release(slot.source);
            release(slot.depth);
            release(slot.query);
        }
        release(target_view);
        release(target);
        release(gradient_target_view);
        release(gradient_target);
        release(source_target_view);
        release(source_target);
        release(depth_target_view);
        release(depth_target);
        release(device);
    }
};

UvSemantic inspect_uv_output(const void *bytecode, std::size_t size) {
    if (bytecode == nullptr || size == 0) return {};
    ID3D11ShaderReflection *reflection = nullptr;
    if (FAILED(D3DReflect(bytecode, size, __uuidof(ID3D11ShaderReflection),
                          reinterpret_cast<void **>(&reflection)))) return {};
    D3D11_SHADER_DESC shader_desc {};
    if (FAILED(reflection->GetDesc(&shader_desc))) {
        release(reflection);
        return {};
    }
    UvSemantic result;
    for (UINT index = 0; index < shader_desc.OutputParameters; ++index) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter {};
        if (FAILED(reflection->GetOutputParameterDesc(index, &parameter)) ||
            parameter.SemanticName == nullptr ||
            _stricmp(parameter.SemanticName, "TEXCOORD") != 0 ||
            (parameter.Mask & 0x3u) != 0x3u ||
            parameter.ComponentType != D3D_REGISTER_COMPONENT_FLOAT32)
            continue;
        result = {parameter.SemanticName, parameter.SemanticIndex, parameter.Register};
        break;
    }
    release(reflection);
    return result;
}

SurfaceCapture::~SurfaceCapture() { reset(); }

bool SurfaceCapture::initialize(ID3D11Device *device, std::uint32_t width,
                                std::uint32_t height) {
    reset();
    if (device == nullptr || width == 0 || height == 0) return false;
    auto *implementation = new Impl;
    implementation->device = device;
    device->AddRef();
    implementation->width = width;
    implementation->height = height;

    D3D11_TEXTURE2D_DESC target_desc {};
    target_desc.Width = width;
    target_desc.Height = height;
    target_desc.MipLevels = 1;
    target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R32G32B32A32_UINT;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    if (FAILED(device->CreateTexture2D(&target_desc, nullptr, &implementation->target)) ||
        FAILED(device->CreateRenderTargetView(implementation->target, nullptr,
                                               &implementation->target_view))) {
        delete implementation;
        return false;
    }
    D3D11_TEXTURE2D_DESC gradient_desc = target_desc;
    gradient_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (FAILED(device->CreateTexture2D(&gradient_desc, nullptr,
                                      &implementation->gradient_target)) ||
        FAILED(device->CreateRenderTargetView(implementation->gradient_target, nullptr,
                                              &implementation->gradient_target_view))) {
        delete implementation;
        return false;
    }
    D3D11_TEXTURE2D_DESC source_desc = target_desc;
    source_desc.Format = DXGI_FORMAT_R32G32B32A32_FLOAT;
    if (FAILED(device->CreateTexture2D(&source_desc, nullptr,
                                      &implementation->source_target)) ||
        FAILED(device->CreateRenderTargetView(implementation->source_target, nullptr,
                                              &implementation->source_target_view))) {
        delete implementation;
        return false;
    }
    D3D11_TEXTURE2D_DESC depth_desc = target_desc;
    depth_desc.Format = DXGI_FORMAT_R32_FLOAT;
    if (FAILED(device->CreateTexture2D(&depth_desc, nullptr,
                                      &implementation->depth_target)) ||
        FAILED(device->CreateRenderTargetView(implementation->depth_target, nullptr,
                                              &implementation->depth_target_view))) {
        delete implementation;
        return false;
    }
    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query_desc {D3D11_QUERY_EVENT, 0};
    D3D11_TEXTURE2D_DESC gradient_staging = gradient_desc;
    gradient_staging.Usage = D3D11_USAGE_STAGING;
    gradient_staging.BindFlags = 0;
    gradient_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_TEXTURE2D_DESC source_staging = source_desc;
    source_staging.Usage = D3D11_USAGE_STAGING;
    source_staging.BindFlags = 0;
    source_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_TEXTURE2D_DESC depth_staging = depth_desc;
    depth_staging.Usage = D3D11_USAGE_STAGING;
    depth_staging.BindFlags = 0;
    depth_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &slot : implementation->readback) {
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &slot.texture)) ||
            FAILED(device->CreateTexture2D(&gradient_staging, nullptr, &slot.gradients)) ||
            FAILED(device->CreateTexture2D(&source_staging, nullptr, &slot.source)) ||
            FAILED(device->CreateTexture2D(&depth_staging, nullptr, &slot.depth)) ||
            FAILED(device->CreateQuery(&query_desc, &slot.query))) {
            delete implementation;
            return false;
        }
    }
    impl_ = implementation;
    return true;
}

void SurfaceCapture::reset() {
    delete impl_;
    impl_ = nullptr;
}

std::uint64_t SurfaceCapture::replayed_draws() const noexcept {
    return impl_ != nullptr ? impl_->replayed_draws : 0;
}

std::uint64_t SurfaceCapture::dropped_frames() const noexcept {
    return impl_ != nullptr ? impl_->dropped_frames : 0;
}

std::uint64_t SurfaceCapture::replacement_draws() const noexcept {
    return impl_ != nullptr ? impl_->replacement_draws : 0;
}

std::uint64_t SurfaceCapture::rejected_replacements() const noexcept {
    return impl_ != nullptr ? impl_->rejected_replacements : 0;
}

std::uint32_t SurfaceCapture::width() const noexcept {
    return impl_ != nullptr ? impl_->width : 0;
}

std::uint32_t SurfaceCapture::height() const noexcept {
    return impl_ != nullptr ? impl_->height : 0;
}

void SurfaceCapture::queue_replacement(
    std::uint64_t material_id, std::vector<ReplacementMip> mips) {
    if (impl_ == nullptr || material_id == 0 || mips.empty()) return;
    auto &entry = impl_->replacements[material_id];
    release(entry.rejected_source);
    entry.mips = std::move(mips);
    entry.dirty = true;
}

void SurfaceCapture::clear_replacements() {
    if (impl_ == nullptr) return;
    for (auto &[material_id, replacement] : impl_->replacements) {
        (void)material_id;
        release(replacement.view);
        release(replacement.texture);
        release(replacement.source);
        release(replacement.rejected_source);
    }
    impl_->replacements.clear();
}

bool SurfaceCapture::draw(ID3D11DeviceContext *context, const UvSemantic &uv,
                          std::uint64_t material_id, std::uint32_t vertex_count,
                          std::uint32_t instance_count, std::uint32_t first_vertex,
                          std::uint32_t first_instance,
                          int source_texture_override) {
    if (impl_ == nullptr) return false;
    return impl_->replay(context, uv, material_id, source_texture_override, [&] {
        context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
    });
}

bool SurfaceCapture::draw_indexed(ID3D11DeviceContext *context, const UvSemantic &uv,
                                  std::uint64_t material_id, std::uint32_t index_count,
                                  std::uint32_t instance_count, std::uint32_t first_index,
                                  std::int32_t vertex_offset,
                                  std::uint32_t first_instance,
                                  int source_texture_override) {
    if (impl_ == nullptr) return false;
    return impl_->replay(context, uv, material_id, source_texture_override, [&] {
        context->DrawIndexedInstanced(index_count, instance_count, first_index,
                                      vertex_offset, first_instance);
    });
}

std::optional<SurfaceCaptureFrame> SurfaceCapture::finish_frame(
    ID3D11DeviceContext *context) {
    if (impl_ == nullptr || context == nullptr) return std::nullopt;
    std::optional<SurfaceCaptureFrame> result;
    for (auto &slot : impl_->readback) {
        if (!slot.in_flight || context->GetData(slot.query, nullptr, 0,
                D3D11_ASYNC_GETDATA_DONOTFLUSH) != S_OK) continue;
        D3D11_MAPPED_SUBRESOURCE mapped {};
        D3D11_MAPPED_SUBRESOURCE gradient_mapped {};
        D3D11_MAPPED_SUBRESOURCE source_mapped {};
        D3D11_MAPPED_SUBRESOURCE depth_mapped {};
        if (SUCCEEDED(context->Map(slot.texture, 0, D3D11_MAP_READ, 0, &mapped)) &&
            SUCCEEDED(context->Map(slot.gradients, 0, D3D11_MAP_READ, 0, &gradient_mapped)) &&
            SUCCEEDED(context->Map(slot.source, 0, D3D11_MAP_READ, 0, &source_mapped)) &&
            SUCCEEDED(context->Map(slot.depth, 0, D3D11_MAP_READ, 0, &depth_mapped))) {
            SurfaceCaptureFrame frame(impl_->width, impl_->height, slot.frame_index);
            for (std::uint32_t y = 0; y < impl_->height; ++y) {
                const auto *row = static_cast<const std::uint8_t *>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch;
                const auto *gradient_row = static_cast<const std::uint8_t *>(gradient_mapped.pData) +
                    static_cast<std::size_t>(y) * gradient_mapped.RowPitch;
                const auto *source_row = static_cast<const std::uint8_t *>(source_mapped.pData) +
                    static_cast<std::size_t>(y) * source_mapped.RowPitch;
                const auto *depth_row = static_cast<const std::uint8_t *>(depth_mapped.pData) +
                    static_cast<std::size_t>(y) * depth_mapped.RowPitch;
                for (std::uint32_t x = 0; x < impl_->width; ++x) {
                    const auto *encoded = reinterpret_cast<const std::uint32_t *>(row) + x * 4;
                    const auto *gradient = reinterpret_cast<const float *>(gradient_row) + x * 4;
                    const auto *source = reinterpret_cast<const float *>(source_row) + x * 4;
                    const auto depth = reinterpret_cast<const float *>(depth_row)[x];
                    auto &pixel = frame.pixels().at(x, y);
                    pixel.material_id = static_cast<std::uint64_t>(encoded[0]) |
                        (static_cast<std::uint64_t>(encoded[1]) << 32);
                    std::memcpy(&pixel.u, encoded + 2, sizeof(float));
                    std::memcpy(&pixel.v, encoded + 3, sizeof(float));
                    pixel.confidence = pixel.material_id == 0 ? 0.0f : 1.0f;
                    pixel.du_dx = gradient[0];
                    pixel.du_dy = gradient[1];
                    pixel.dv_dx = gradient[2];
                    pixel.dv_dy = gradient[3];
                    pixel.source_r = source[0];
                    pixel.source_g = source[1];
                    pixel.source_b = source[2];
                    pixel.source_a = source[3];
                    pixel.framebuffer_depth = depth;
                    pixel.hit_depth = depth;
                }
            }
            context->Unmap(slot.depth, 0);
            context->Unmap(slot.source, 0);
            context->Unmap(slot.gradients, 0);
            context->Unmap(slot.texture, 0);
            result = std::move(frame);
        } else {
            // Mapping the identity surface may have succeeded before the
            // gradient surface failed; unmap only that first resource.
            if (depth_mapped.pData != nullptr) context->Unmap(slot.depth, 0);
            if (source_mapped.pData != nullptr) context->Unmap(slot.source, 0);
            if (gradient_mapped.pData != nullptr) context->Unmap(slot.gradients, 0);
            if (mapped.pData != nullptr) context->Unmap(slot.texture, 0);
        }
        slot.in_flight = false;
    }

    auto &next = impl_->readback[impl_->next_readback];
    if (!next.in_flight) {
        if (!impl_->target_cleared) {
            context->ClearRenderTargetView(impl_->target_view, k_clear.data());
            context->ClearRenderTargetView(impl_->gradient_target_view, k_clear.data());
            context->ClearRenderTargetView(impl_->source_target_view, k_missing_source.data());
            context->ClearRenderTargetView(impl_->depth_target_view, k_missing_source.data());
            impl_->target_cleared = true;
        }
        context->CopyResource(next.texture, impl_->target);
        context->CopyResource(next.gradients, impl_->gradient_target);
        context->CopyResource(next.source, impl_->source_target);
        context->CopyResource(next.depth, impl_->depth_target);
        context->End(next.query);
        next.in_flight = true;
        next.frame_index = impl_->next_frame_index++;
        impl_->next_readback = (impl_->next_readback + 1) % impl_->readback.size();
    } else {
        ++impl_->dropped_frames;
    }
    context->ClearRenderTargetView(impl_->target_view, k_clear.data());
    context->ClearRenderTargetView(impl_->gradient_target_view, k_clear.data());
    context->ClearRenderTargetView(impl_->source_target_view, k_missing_source.data());
    context->ClearRenderTargetView(impl_->depth_target_view, k_missing_source.data());
    return result;
}

} // namespace neuralpass::d3d11_capture
