#include "../addon/d3d11_surface_capture.hpp"

#include <d3dcompiler.h>

#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>

namespace {

template <typename T> void release(T *&object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

ID3DBlob *compile(const char *source, const char *profile) {
    ID3DBlob *shader = nullptr;
    ID3DBlob *errors = nullptr;
    const auto result = D3DCompile(source, std::strlen(source), "capture-test", nullptr,
        nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0, &shader, &errors);
    if (FAILED(result)) {
        const std::string message = errors != nullptr
            ? static_cast<const char *>(errors->GetBufferPointer()) : "shader compile failed";
        release(errors);
        throw std::runtime_error(message);
    }
    release(errors);
    return shader;
}

std::array<std::uint8_t, 4> read_pixel(ID3D11Device *device,
                                      ID3D11DeviceContext *context,
                                      ID3D11Texture2D *texture,
                                      std::uint32_t x, std::uint32_t y) {
    D3D11_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    require(x < desc.Width && y < desc.Height, "pixel readback is out of bounds");
    desc.Usage = D3D11_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D11Texture2D *staging = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)),
        "could not create pixel readback texture");
    context->CopyResource(staging, texture);
    D3D11_MAPPED_SUBRESOURCE mapped {};
    require(SUCCEEDED(context->Map(staging, 0, D3D11_MAP_READ, 0, &mapped)),
        "could not map pixel readback texture");
    std::array<std::uint8_t, 4> pixel {};
    std::memcpy(pixel.data(), static_cast<const std::uint8_t *>(mapped.pData) +
        static_cast<std::size_t>(y) * mapped.RowPitch + x * 4, pixel.size());
    context->Unmap(staging, 0);
    release(staging);
    return pixel;
}

void test_triangle_replay_produces_material_uv() {
    ID3D11Device *device = nullptr;
    ID3D11DeviceContext *context = nullptr;
    D3D_FEATURE_LEVEL feature_level {};
    require(SUCCEEDED(D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, 0,
        nullptr, 0, D3D11_SDK_VERSION, &device, &feature_level, &context)),
        "could not create the D3D11 WARP test device");
    require(feature_level >= D3D_FEATURE_LEVEL_11_0,
        "D3D11 WARP did not expose shader model 5");

    constexpr char vertex_source[] =
        "struct Input { float2 position : POSITION; float2 uv : TEXCOORD0; };"
        "struct Output { float4 position : SV_Position; float2 uv : TEXCOORD0; };"
        "Output main(Input input) { Output o; o.position=float4(input.position,0.5,1);"
        "o.uv=input.uv; return o; }";
    constexpr char pixel_source[] =
        "Texture2D<float4> color_texture : register(t5);"
        "SamplerState color_sampler : register(s5);"
        "float4 main(float2 uv : TEXCOORD0) : SV_Target {"
        "return color_texture.Sample(color_sampler,uv); }";
    ID3DBlob *vertex_bytecode = compile(vertex_source, "vs_5_0");
    ID3DBlob *pixel_bytecode = compile(pixel_source, "ps_5_0");
    ID3D11VertexShader *vertex_shader = nullptr;
    ID3D11PixelShader *pixel_shader = nullptr;
    require(SUCCEEDED(device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), nullptr, &vertex_shader)),
        "could not create the test vertex shader");
    require(SUCCEEDED(device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
        pixel_bytecode->GetBufferSize(), nullptr, &pixel_shader)),
        "could not create the test pixel shader");

    const auto uv_semantic = neuralpass::d3d11_capture::inspect_uv_output(
        vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize());
    require(uv_semantic.valid() && uv_semantic.name == "TEXCOORD" && uv_semantic.index == 0,
        "vertex reflection did not find the rasterized UV output");

    struct Vertex { float x, y, u, v; };
    const std::array<Vertex, 3> vertices {{
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 0.0f,  1.0f, 0.5f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
    }};
    D3D11_BUFFER_DESC buffer_desc {};
    buffer_desc.ByteWidth = sizeof(vertices);
    buffer_desc.Usage = D3D11_USAGE_IMMUTABLE;
    buffer_desc.BindFlags = D3D11_BIND_VERTEX_BUFFER;
    D3D11_SUBRESOURCE_DATA buffer_data {vertices.data(), 0, 0};
    ID3D11Buffer *vertex_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&buffer_desc, &buffer_data, &vertex_buffer)),
        "could not create the test vertex buffer");
    constexpr std::array<std::uint16_t, 3> indices {0, 1, 2};
    D3D11_BUFFER_DESC index_desc {};
    index_desc.ByteWidth = sizeof(indices);
    index_desc.Usage = D3D11_USAGE_IMMUTABLE;
    index_desc.BindFlags = D3D11_BIND_INDEX_BUFFER;
    D3D11_SUBRESOURCE_DATA index_data {indices.data(), 0, 0};
    ID3D11Buffer *index_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&index_desc, &index_data, &index_buffer)),
        "could not create the test index buffer");
    const D3D11_DRAW_INSTANCED_INDIRECT_ARGS draw_arguments {3, 2, 0, 0};
    D3D11_BUFFER_DESC draw_arguments_desc {};
    draw_arguments_desc.ByteWidth = sizeof(draw_arguments);
    draw_arguments_desc.Usage = D3D11_USAGE_DEFAULT;
    draw_arguments_desc.MiscFlags = D3D11_RESOURCE_MISC_DRAWINDIRECT_ARGS;
    D3D11_SUBRESOURCE_DATA draw_arguments_data {&draw_arguments, 0, 0};
    ID3D11Buffer *draw_arguments_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&draw_arguments_desc, &draw_arguments_data,
                                            &draw_arguments_buffer)),
        "could not create non-indexed indirect arguments");
    const D3D11_DRAW_INDEXED_INSTANCED_INDIRECT_ARGS indexed_arguments {3, 2, 0, 0, 0};
    D3D11_BUFFER_DESC indexed_arguments_desc = draw_arguments_desc;
    indexed_arguments_desc.ByteWidth = sizeof(indexed_arguments);
    D3D11_SUBRESOURCE_DATA indexed_arguments_data {&indexed_arguments, 0, 0};
    ID3D11Buffer *indexed_arguments_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&indexed_arguments_desc, &indexed_arguments_data,
                                            &indexed_arguments_buffer)),
        "could not create indexed indirect arguments");
    const std::array<D3D11_INPUT_ELEMENT_DESC, 2> input_elements {{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D11_INPUT_PER_VERTEX_DATA, 0},
    }};
    ID3D11InputLayout *input_layout = nullptr;
    require(SUCCEEDED(device->CreateInputLayout(input_elements.data(), input_elements.size(),
        vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize(), &input_layout)),
        "could not create the test input layout");

    D3D11_TEXTURE2D_DESC target_desc {};
    target_desc.Width = 8;
    target_desc.Height = 8;
    target_desc.MipLevels = 1;
    target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D11_USAGE_DEFAULT;
    target_desc.BindFlags = D3D11_BIND_RENDER_TARGET;
    ID3D11Texture2D *target = nullptr;
    ID3D11RenderTargetView *target_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&target_desc, nullptr, &target)) &&
            SUCCEEDED(device->CreateRenderTargetView(target, nullptr, &target_view)),
        "could not create the test render target");

    ID3D11Texture2D *secondary_target = nullptr;
    ID3D11RenderTargetView *secondary_target_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&target_desc, nullptr, &secondary_target)) &&
            SUCCEEDED(device->CreateRenderTargetView(
                secondary_target, nullptr, &secondary_target_view)),
        "could not create the secondary render target");
    D3D11_TEXTURE2D_DESC depth_texture_desc = target_desc;
    depth_texture_desc.Format = DXGI_FORMAT_D32_FLOAT;
    depth_texture_desc.BindFlags = D3D11_BIND_DEPTH_STENCIL;
    ID3D11Texture2D *depth_texture = nullptr;
    ID3D11DepthStencilView *depth_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&depth_texture_desc, nullptr, &depth_texture)) &&
            SUCCEEDED(device->CreateDepthStencilView(depth_texture, nullptr, &depth_view)),
        "could not create the depth target");
    D3D11_BUFFER_DESC uav_buffer_desc {};
    uav_buffer_desc.ByteWidth = 4 * sizeof(std::uint32_t);
    uav_buffer_desc.Usage = D3D11_USAGE_DEFAULT;
    uav_buffer_desc.BindFlags = D3D11_BIND_UNORDERED_ACCESS;
    uav_buffer_desc.MiscFlags = D3D11_RESOURCE_MISC_BUFFER_STRUCTURED;
    uav_buffer_desc.StructureByteStride = sizeof(std::uint32_t);
    ID3D11Buffer *uav_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&uav_buffer_desc, nullptr, &uav_buffer)),
        "could not create the output-merger UAV buffer");
    D3D11_UNORDERED_ACCESS_VIEW_DESC uav_desc {};
    uav_desc.Format = DXGI_FORMAT_UNKNOWN;
    uav_desc.ViewDimension = D3D11_UAV_DIMENSION_BUFFER;
    uav_desc.Buffer.NumElements = 4;
    uav_desc.Buffer.Flags = D3D11_BUFFER_UAV_FLAG_APPEND;
    ID3D11UnorderedAccessView *application_uav = nullptr;
    require(SUCCEEDED(device->CreateUnorderedAccessView(
                uav_buffer, &uav_desc, &application_uav)),
        "could not create the output-merger UAV");

    constexpr UINT stride = sizeof(Vertex);
    constexpr UINT offset = 0;
    context->IASetInputLayout(input_layout);
    context->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    context->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    context->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
    context->VSSetShader(vertex_shader, nullptr, 0);
    context->PSSetShader(pixel_shader, nullptr, 0);
    ID3D11RenderTargetView *application_targets[2] {target_view, secondary_target_view};
    constexpr UINT initial_uav_count = 2;
    context->OMSetRenderTargetsAndUnorderedAccessViews(
        2, application_targets, depth_view, 2, 1, &application_uav, &initial_uav_count);
    D3D11_BLEND_DESC blend_desc {};
    blend_desc.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    ID3D11BlendState *blend_state = nullptr;
    require(SUCCEEDED(device->CreateBlendState(&blend_desc, &blend_state)),
        "could not create the test blend state");
    constexpr FLOAT blend_factor[4] {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr UINT sample_mask = 0x5a5a5a5bu;
    context->OMSetBlendState(blend_state, blend_factor, sample_mask);
    D3D11_DEPTH_STENCIL_DESC depth_desc {};
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D11_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D11_COMPARISON_ALWAYS;
    ID3D11DepthStencilState *depth_state = nullptr;
    require(SUCCEEDED(device->CreateDepthStencilState(&depth_desc, &depth_state)),
        "could not create the test depth state");
    context->OMSetDepthStencilState(depth_state, 37);
    const D3D11_VIEWPORT viewport {0.0f, 0.0f, 8.0f, 8.0f, 0.0f, 1.0f};
    context->RSSetViewports(1, &viewport);

    std::array<std::uint8_t, 4 * 4 * 4> source_pixels {};
    for (std::size_t index = 0; index < source_pixels.size(); index += 4) {
        source_pixels[index + 0] = 64;
        source_pixels[index + 1] = 128;
        source_pixels[index + 2] = 192;
        source_pixels[index + 3] = 77;
    }
    D3D11_TEXTURE2D_DESC source_desc {};
    source_desc.Width = source_desc.Height = 4;
    source_desc.MipLevels = source_desc.ArraySize = 1;
    source_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D11_USAGE_IMMUTABLE;
    source_desc.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    const D3D11_SUBRESOURCE_DATA source_data {source_pixels.data(), 4 * 4, 0};
    ID3D11Texture2D *source_texture = nullptr;
    ID3D11ShaderResourceView *source_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&source_desc, &source_data, &source_texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(source_texture, nullptr, &source_view)),
        "could not create source texture capture fixture");
    D3D11_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D11_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        D3D11_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D11_FLOAT32_MAX;
    ID3D11SamplerState *source_sampler = nullptr;
    require(SUCCEEDED(device->CreateSamplerState(&sampler_desc, &source_sampler)),
        "could not create source sampler fixture");
    context->PSSetShaderResources(3, 1, &source_view);
    context->PSSetSamplers(3, 1, &source_sampler);
    auto override_pixels = source_pixels;
    for (std::size_t index = 0; index < override_pixels.size(); index += 4) {
        override_pixels[index + 0] = 16;
        override_pixels[index + 1] = 32;
        override_pixels[index + 2] = 240;
        override_pixels[index + 3] = 191;
    }
    // The bottom-left source texel is a cutout. The second replay must discard
    // it, leaving the first draw's identity in the capture target at that pixel.
    override_pixels[(3 * 4 + 0) * 4 + 3] = 0;
    const D3D11_SUBRESOURCE_DATA override_data {override_pixels.data(), 4 * 4, 0};
    ID3D11Texture2D *override_texture = nullptr;
    ID3D11ShaderResourceView *override_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&source_desc, &override_data, &override_texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(override_texture, nullptr, &override_view)),
        "could not create source override fixture");
    context->PSSetShaderResources(5, 1, &override_view);
    context->PSSetSamplers(5, 1, &source_sampler);

    neuralpass::d3d11_capture::SurfaceCapture capture;
    require(capture.initialize(device, 8, 8), "could not initialize surface capture");
    neuralpass::capture::SurfaceCaptureBackend &backend = capture;
    const auto capabilities = backend.capabilities();
    require(backend.backend() == neuralpass::capture::GraphicsBackend::d3d11 &&
            capabilities.direct_draws && capabilities.indexed_draws &&
            capabilities.indirect_draws && capabilities.replacement_textures &&
            capabilities.asynchronous_readback &&
            !capabilities.shader_coverage_preserved,
        "D3D11 backend reported inaccurate capabilities");
    constexpr std::uint64_t first_material_id = 0x12345678abcdef01ull;
    constexpr std::uint64_t material_id = 0xfedcba9876543210ull;
    neuralpass::d3d11_capture::ReplacementMip replacement;
    replacement.width = replacement.height = 4;
    replacement.rgba.resize(4 * 4 * 4);
    replacement.coverage.resize(4 * 4, 2);
    for (std::size_t index = 0; index < replacement.rgba.size(); index += 4) {
        replacement.rgba[index + 0] = 200;
        replacement.rgba[index + 1] = 10;
        replacement.rgba[index + 2] = 20;
        replacement.rgba[index + 3] = 191;
    }
    for (std::size_t y = 0; y < 4; ++y) {
        replacement.coverage[y * 4 + 0] = 0;
    }
    capture.queue_replacement(material_id, {std::move(replacement)});
    constexpr std::uint64_t shared_source_material_id = 0x0badf00d12344321ull;
    neuralpass::d3d11_capture::ReplacementMip shared_source_replacement;
    shared_source_replacement.width = shared_source_replacement.height = 4;
    shared_source_replacement.rgba.resize(4 * 4 * 4);
    shared_source_replacement.coverage.resize(4 * 4, 2);
    for (std::size_t index = 0; index < shared_source_replacement.rgba.size(); index += 4) {
        shared_source_replacement.rgba[index + 0] = 12;
        shared_source_replacement.rgba[index + 1] = 210;
        shared_source_replacement.rgba[index + 2] = 30;
        shared_source_replacement.rgba[index + 3] = 191;
    }
    capture.queue_replacement(shared_source_material_id,
                              {std::move(shared_source_replacement)});
    D3D11_TEXTURE2D_DESC unsupported_desc = source_desc;
    unsupported_desc.Format = DXGI_FORMAT_R16G16B16A16_FLOAT;
    unsupported_desc.Usage = D3D11_USAGE_DEFAULT;
    ID3D11Texture2D *unsupported_texture = nullptr;
    ID3D11ShaderResourceView *unsupported_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&unsupported_desc, nullptr, &unsupported_texture)) &&
            SUCCEEDED(device->CreateShaderResourceView(
                unsupported_texture, nullptr, &unsupported_view)),
        "could not create unsupported replacement fixture");
    context->PSSetShaderResources(6, 1, &unsupported_view);
    context->PSSetSamplers(6, 1, &source_sampler);
    constexpr std::uint64_t unsupported_material_id = 0x1111222233334444ull;
    neuralpass::d3d11_capture::ReplacementMip unsupported_replacement;
    unsupported_replacement.width = unsupported_replacement.height = 4;
    unsupported_replacement.rgba.resize(4 * 4 * 4);
    unsupported_replacement.coverage.resize(4 * 4, 2);
    capture.queue_replacement(unsupported_material_id, {std::move(unsupported_replacement)});
    require(capture.draw(context, uv_semantic, unsupported_material_id, 0, 1, 0, 0, 6) &&
            capture.draw(context, uv_semantic, unsupported_material_id, 0, 1, 0, 0, 6),
        "unsupported replacement fixture draw was not handled");
    require(capture.rejected_replacements() == 1,
        "unsupported replacement circuit breaker retried the same source");
    neuralpass::capture::DrawCommand direct_draw;
    direct_draw.kind = neuralpass::capture::DrawKind::direct;
    direct_draw.vertex_or_index_count = 3;
    direct_draw.instance_count = 2;
    require(backend.replay(context, uv_semantic, first_material_id, direct_draw),
        "capture adapter did not handle the triangle draw");
    require(capture.draw_indexed(context, uv_semantic, material_id, 3, 2, 0, 0, 0, 5),
        "capture adapter did not handle the indexed triangle draw");
    const auto first_binding_pixel = read_pixel(device, context, target, 7, 7);
    require(std::abs(static_cast<int>(first_binding_pixel[0]) - 200) <= 1 &&
            std::abs(static_cast<int>(first_binding_pixel[1]) - 10) <= 1 &&
            std::abs(static_cast<int>(first_binding_pixel[2]) - 20) <= 1,
        "first binding did not use its replacement for the shared source");
    require(capture.draw_indexed(context, uv_semantic, shared_source_material_id,
                                 3, 1, 0, 0, 0, 5),
        "second binding sharing a source was not handled");
    const auto second_binding_pixel = read_pixel(device, context, target, 7, 7);
    require(std::abs(static_cast<int>(second_binding_pixel[0]) - 12) <= 1 &&
            std::abs(static_cast<int>(second_binding_pixel[1]) - 210) <= 1 &&
            std::abs(static_cast<int>(second_binding_pixel[2]) - 30) <= 1,
        "second binding reused the first binding's replacement");
    require(capture.draw_indexed(context, uv_semantic, material_id, 3, 1, 0, 0, 0, 5),
        "first shared-source binding could not be restored");
    const auto restored_binding_pixel = read_pixel(device, context, target, 7, 7);
    require(std::abs(static_cast<int>(restored_binding_pixel[0]) - 200) <= 1 &&
            std::abs(static_cast<int>(restored_binding_pixel[1]) - 10) <= 1 &&
            std::abs(static_cast<int>(restored_binding_pixel[2]) - 20) <= 1,
        "first binding replacement was contaminated by the second binding");
    require(capture.draw_indexed_indirect(context, uv_semantic, shared_source_material_id,
                                          indexed_arguments_buffer, 0, 5),
        "capture adapter did not handle the indexed instanced indirect draw");
    require(capture.draw_indirect(context, uv_semantic, material_id,
                                  draw_arguments_buffer, 0, 5),
        "capture adapter did not handle the instanced indirect draw");
    require(capture.replayed_draws() == 8, "draw variants were not each replayed once");
    require(capture.replacement_draws() == 5,
        "replacement draw accounting did not report all substituted draws");

    ID3D11PixelShader *restored_pixel_shader = nullptr;
    context->PSGetShader(&restored_pixel_shader, nullptr, nullptr);
    require(restored_pixel_shader == pixel_shader, "pixel shader state was not restored");
    release(restored_pixel_shader);
    ID3D11BlendState *restored_blend = nullptr;
    FLOAT restored_factor[4] {};
    UINT restored_mask = 0;
    context->OMGetBlendState(&restored_blend, restored_factor, &restored_mask);
    require(restored_blend == blend_state && restored_mask == sample_mask &&
            std::equal(std::begin(restored_factor), std::end(restored_factor),
                       std::begin(blend_factor)),
        "blend state was not restored");
    release(restored_blend);
    ID3D11DepthStencilState *restored_depth = nullptr;
    UINT restored_stencil = 0;
    context->OMGetDepthStencilState(&restored_depth, &restored_stencil);
    require(restored_depth == depth_state && restored_stencil == 37,
        "depth-stencil state was not restored");
    release(restored_depth);
    ID3D11RenderTargetView *restored_targets[2] {};
    ID3D11DepthStencilView *restored_depth_view = nullptr;
    context->OMGetRenderTargets(2, restored_targets, &restored_depth_view);
    require(restored_targets[0] == target_view &&
            restored_targets[1] == secondary_target_view,
        "multiple render target state was not restored");
    require(restored_depth_view == depth_view, "depth target state was not restored");
    release(restored_targets[0]);
    release(restored_targets[1]);
    release(restored_depth_view);
    ID3D11UnorderedAccessView *restored_uav = nullptr;
    context->OMGetRenderTargetsAndUnorderedAccessViews(
        0, nullptr, nullptr, 2, 1, &restored_uav);
    require(restored_uav == application_uav,
        "output-merger UAV state was not restored");
    release(restored_uav);
    ID3D11ShaderResourceView *restored_source = nullptr;
    context->PSGetShaderResources(5, 1, &restored_source);
    require(restored_source == override_view, "source texture binding was not restored");
    release(restored_source);
    D3D11_TEXTURE2D_DESC target_staging_desc = target_desc;
    target_staging_desc.Usage = D3D11_USAGE_STAGING;
    target_staging_desc.BindFlags = 0;
    target_staging_desc.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    ID3D11Texture2D *target_staging = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&target_staging_desc, nullptr, &target_staging)),
        "could not create replacement verification staging texture");
    context->CopyResource(target_staging, target);
    D3D11_MAPPED_SUBRESOURCE target_mapped {};
    require(SUCCEEDED(context->Map(target_staging, 0, D3D11_MAP_READ, 0, &target_mapped)),
        "could not read replacement verification target");
    const auto *target_covered = static_cast<const std::uint8_t *>(target_mapped.pData) +
        7 * target_mapped.RowPitch + 7 * 4;
    require(std::abs(static_cast<int>(target_covered[0]) - 200) <= 1 &&
            std::abs(static_cast<int>(target_covered[1]) - 10) <= 1 &&
            std::abs(static_cast<int>(target_covered[2]) - 20) <= 1,
        "covered atlas texel was not substituted into the game draw");
    const auto *target_uncovered = static_cast<const std::uint8_t *>(target_mapped.pData) +
        7 * target_mapped.RowPitch;
    require(std::abs(static_cast<int>(target_uncovered[0]) - 16) <= 1 &&
            std::abs(static_cast<int>(target_uncovered[1]) - 32) <= 1 &&
            std::abs(static_cast<int>(target_uncovered[2]) - 240) <= 1,
        "uncovered replacement texel did not retain the original source");
    context->Unmap(target_staging, 0);
    require(!capture.finish_frame(context).has_value(),
        "readback completed before it was queued");
    context->Flush();

    std::optional<neuralpass::SurfaceCaptureFrame> captured;
    for (int attempt = 0; attempt < 100 && !captured; ++attempt)
        captured = capture.finish_frame(context);
    require(captured.has_value(), "asynchronous capture did not complete");
    const auto &center = captured->pixels().at(4, 4);
    require(center.material_id == material_id, "captured material ID was corrupted");
    if (!(std::isfinite(center.u) && std::isfinite(center.v) &&
          center.u > 0.4f && center.u < 0.7f && center.v > 0.3f && center.v < 0.8f))
        std::cerr << "captured center UV: " << center.u << ", " << center.v << '\n';
    require(std::isfinite(center.u) && std::isfinite(center.v) &&
            center.u > 0.4f && center.u < 0.7f && center.v > 0.3f && center.v < 0.8f,
        "captured interpolated UV is invalid");
    require(std::isfinite(center.du_dx) && std::isfinite(center.du_dy) &&
            std::isfinite(center.dv_dx) && std::isfinite(center.dv_dy) &&
            std::abs(center.du_dx) + std::abs(center.du_dy) +
                std::abs(center.dv_dx) + std::abs(center.dv_dy) > 0.01f,
        "captured UV gradients are invalid");
    require(std::isfinite(center.framebuffer_depth) && std::isfinite(center.hit_depth) &&
            std::abs(center.framebuffer_depth - 0.5f) < 0.001f &&
            std::abs(center.hit_depth - center.framebuffer_depth) < 0.0001f,
        "captured device-depth correspondence is invalid");
    require(std::isfinite(center.source_r) && std::isfinite(center.source_g) &&
            std::isfinite(center.source_b) && std::isfinite(center.source_a) &&
            std::abs(center.source_r - 16.0f/255.0f) < 0.01f &&
            std::abs(center.source_g - 32.0f/255.0f) < 0.01f &&
            std::abs(center.source_b - 240.0f/255.0f) < 0.01f &&
            std::abs(center.source_a - 191.0f/255.0f) < 0.01f,
        "per-binding source slot override did not select the requested texture");
    const auto &cutout = captured->pixels().at(1, 6);
    require(cutout.material_id == first_material_id,
        "fully transparent source texel was incorrectly captured as visible geometry");

    capture.reset();
    release(target_staging);
    release(unsupported_view);
    release(unsupported_texture);
    release(override_view);
    release(override_texture);
    release(source_sampler);
    release(source_view);
    release(source_texture);
    release(depth_state);
    release(blend_state);
    release(application_uav);
    release(uav_buffer);
    release(depth_view);
    release(depth_texture);
    release(secondary_target_view);
    release(secondary_target);
    release(target_view);
    release(target);
    release(input_layout);
    release(indexed_arguments_buffer);
    release(draw_arguments_buffer);
    release(index_buffer);
    release(vertex_buffer);
    release(pixel_shader);
    release(vertex_shader);
    release(pixel_bytecode);
    release(vertex_bytecode);
    release(context);
    release(device);
}

} // namespace

int main() {
    try {
        test_triangle_replay_produces_material_uv();
        std::cout << "NeuralPass D3D11 surface capture test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
