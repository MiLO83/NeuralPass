#include "d3d11_surface_capture.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cstring>
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
    ID3D11Query *query = nullptr;
    bool in_flight = false;
    std::uint64_t frame_index = 0;
};

constexpr std::array<float, 4> k_clear {0.0f, 0.0f, 0.0f, 0.0f};

} // namespace

struct SurfaceCapture::Impl {
    ID3D11Device *device = nullptr;
    ID3D11Texture2D *target = nullptr;
    ID3D11RenderTargetView *target_view = nullptr;
    ID3D11Texture2D *gradient_target = nullptr;
    ID3D11RenderTargetView *gradient_target_view = nullptr;
    std::array<ReadbackSlot, 3> readback;
    std::unordered_map<ShaderKey, ID3D11PixelShader *, ShaderKeyHash> shaders;
    std::unordered_map<ID3D11DepthStencilState *, DepthVariant> depth_variants;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t next_readback = 0;
    std::uint64_t next_frame_index = 1;
    std::uint64_t replayed_draws = 0;
    std::uint64_t dropped_frames = 0;
    bool target_cleared = false;

    ID3D11PixelShader *shader(const UvSemantic &uv, std::uint64_t material_id) {
        const ShaderKey key {uv.name, uv.index, uv.register_index, material_id};
        if (const auto known = shaders.find(key); known != shaders.end()) return known->second;

        if (uv.name.empty() || !std::all_of(uv.name.begin(), uv.name.end(), [](unsigned char value) {
                return std::isalnum(value) != 0 || value == '_';
            })) return nullptr;
        std::ostringstream source;
        // SV_Position occupies input register zero in ordinary rasterized
        // vertex outputs. Declaring it keeps the selected TEXCOORD on the same
        // linkage register as the game's vertex shader rather than accidentally
        // reading clip-space XY as UV.
        source << "struct Input { float4 position : SV_Position; float2 uv : "
               << uv.name << uv.index << "; };\n"
               << "struct Output { uint4 surface : SV_Target0; float4 gradients : SV_Target1; };\n"
               << "Output main(Input input) { Output output; output.surface = uint4("
               << static_cast<std::uint32_t>(material_id) << "u,"
               << static_cast<std::uint32_t>(material_id >> 32) << "u,"
               << "asuint(input.uv.x),asuint(input.uv.y)); "
               << "float2 dx = ddx(input.uv); float2 dy = ddy(input.uv); "
               << "output.gradients = float4(dx.x,dy.x,dx.y,dy.y); return output; }\n";
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
                std::uint64_t material_id, Draw &&draw) {
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
            target_cleared = true;
        }
        std::uint32_t uav_count = 0;
        for (std::uint32_t index = rtv_count; index < uavs.size(); ++index)
            if (uavs[index] != nullptr) uav_count = index + 1;

        // Execute the application's draw first. The callback returns true after
        // a successful replay so ReShade does not execute it a second time.
        draw();

        ID3D11PixelShader *original_pixel_shader = nullptr;
        context->PSGetShader(&original_pixel_shader, nullptr, nullptr);
        ID3D11BlendState *original_blend = nullptr;
        FLOAT blend_factor[4] {};
        UINT sample_mask = 0;
        context->OMGetBlendState(&original_blend, blend_factor, &sample_mask);
        ID3D11DepthStencilState *original_depth = nullptr;
        UINT stencil_reference = 0;
        context->OMGetDepthStencilState(&original_depth, &stencil_reference);

        auto *capture_shader = shader(uv, material_id);
        auto *capture_depth_state = capture_depth(original_depth);
        if (capture_shader != nullptr && (original_depth == nullptr || capture_depth_state != nullptr)) {
            ID3D11RenderTargetView *capture_targets[2] {target_view, gradient_target_view};
            context->OMSetRenderTargets(2, capture_targets, dsv);
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
        for (auto &slot : readback) {
            release(slot.texture);
            release(slot.gradients);
            release(slot.query);
        }
        release(target_view);
        release(target);
        release(gradient_target_view);
        release(gradient_target);
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
    D3D11_TEXTURE2D_DESC staging_desc = target_desc;
    staging_desc.Usage = D3D11_USAGE_STAGING;
    staging_desc.BindFlags = 0;
    staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    D3D11_QUERY_DESC query_desc {D3D11_QUERY_EVENT, 0};
    D3D11_TEXTURE2D_DESC gradient_staging = gradient_desc;
    gradient_staging.Usage = D3D11_USAGE_STAGING;
    gradient_staging.BindFlags = 0;
    gradient_staging.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (auto &slot : implementation->readback) {
        if (FAILED(device->CreateTexture2D(&staging_desc, nullptr, &slot.texture)) ||
            FAILED(device->CreateTexture2D(&gradient_staging, nullptr, &slot.gradients)) ||
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

std::uint32_t SurfaceCapture::width() const noexcept {
    return impl_ != nullptr ? impl_->width : 0;
}

std::uint32_t SurfaceCapture::height() const noexcept {
    return impl_ != nullptr ? impl_->height : 0;
}

bool SurfaceCapture::draw(ID3D11DeviceContext *context, const UvSemantic &uv,
                          std::uint64_t material_id, std::uint32_t vertex_count,
                          std::uint32_t instance_count, std::uint32_t first_vertex,
                          std::uint32_t first_instance) {
    if (impl_ == nullptr) return false;
    return impl_->replay(context, uv, material_id, [&] {
        context->DrawInstanced(vertex_count, instance_count, first_vertex, first_instance);
    });
}

bool SurfaceCapture::draw_indexed(ID3D11DeviceContext *context, const UvSemantic &uv,
                                  std::uint64_t material_id, std::uint32_t index_count,
                                  std::uint32_t instance_count, std::uint32_t first_index,
                                  std::int32_t vertex_offset,
                                  std::uint32_t first_instance) {
    if (impl_ == nullptr) return false;
    return impl_->replay(context, uv, material_id, [&] {
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
        if (SUCCEEDED(context->Map(slot.texture, 0, D3D11_MAP_READ, 0, &mapped)) &&
            SUCCEEDED(context->Map(slot.gradients, 0, D3D11_MAP_READ, 0, &gradient_mapped))) {
            SurfaceCaptureFrame frame(impl_->width, impl_->height, slot.frame_index);
            for (std::uint32_t y = 0; y < impl_->height; ++y) {
                const auto *row = static_cast<const std::uint8_t *>(mapped.pData) +
                    static_cast<std::size_t>(y) * mapped.RowPitch;
                const auto *gradient_row = static_cast<const std::uint8_t *>(gradient_mapped.pData) +
                    static_cast<std::size_t>(y) * gradient_mapped.RowPitch;
                for (std::uint32_t x = 0; x < impl_->width; ++x) {
                    const auto *encoded = reinterpret_cast<const std::uint32_t *>(row) + x * 4;
                    const auto *gradient = reinterpret_cast<const float *>(gradient_row) + x * 4;
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
                }
            }
            context->Unmap(slot.gradients, 0);
            context->Unmap(slot.texture, 0);
            result = std::move(frame);
        } else {
            // Mapping the identity surface may have succeeded before the
            // gradient surface failed; unmap only that first resource.
            if (mapped.pData != nullptr) context->Unmap(slot.texture, 0);
        }
        slot.in_flight = false;
    }

    auto &next = impl_->readback[impl_->next_readback];
    if (!next.in_flight) {
        if (!impl_->target_cleared) {
            context->ClearRenderTargetView(impl_->target_view, k_clear.data());
            context->ClearRenderTargetView(impl_->gradient_target_view, k_clear.data());
            impl_->target_cleared = true;
        }
        context->CopyResource(next.texture, impl_->target);
        context->CopyResource(next.gradients, impl_->gradient_target);
        context->End(next.query);
        next.in_flight = true;
        next.frame_index = impl_->next_frame_index++;
        impl_->next_readback = (impl_->next_readback + 1) % impl_->readback.size();
    } else {
        ++impl_->dropped_frames;
    }
    context->ClearRenderTargetView(impl_->target_view, k_clear.data());
    context->ClearRenderTargetView(impl_->gradient_target_view, k_clear.data());
    return result;
}

} // namespace neuralpass::d3d11_capture
