#include "../addon/d3d10_surface_capture.hpp"

#include <d3dcompiler.h>
#include <d3d11shader.h>

#include <array>
#include <cmath>
#include <cstring>
#include <cstdlib>
#include <iostream>
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
    const auto result = D3DCompile(source, std::strlen(source), "d3d10-capture-test",
        nullptr, nullptr, "main", profile, D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
        &shader, &errors);
    if (FAILED(result)) {
        const std::string message = errors != nullptr
            ? static_cast<const char *>(errors->GetBufferPointer()) : "shader compile failed";
        release(errors);
        throw std::runtime_error(message);
    }
    release(errors);
    return shader;
}

neuralpass::capture::UvInput inspect_uv(const void *code, std::size_t size) {
    ID3D11ShaderReflection *reflection = nullptr;
    require(SUCCEEDED(D3DReflect(code, size, __uuidof(ID3D11ShaderReflection),
        reinterpret_cast<void **>(&reflection))), "could not reflect D3D10 vertex shader");
    D3D11_SHADER_DESC desc {};
    require(SUCCEEDED(reflection->GetDesc(&desc)), "could not inspect D3D10 vertex shader");
    neuralpass::capture::UvInput result;
    for (UINT index = 0; index < desc.OutputParameters; ++index) {
        D3D11_SIGNATURE_PARAMETER_DESC parameter {};
        if (SUCCEEDED(reflection->GetOutputParameterDesc(index, &parameter)) &&
            parameter.SemanticName != nullptr &&
            _stricmp(parameter.SemanticName, "TEXCOORD") == 0 &&
            (parameter.Mask & 3u) == 3u) {
            result = {parameter.SemanticName, parameter.SemanticIndex, parameter.Register};
            break;
        }
    }
    release(reflection);
    return result;
}

std::array<std::uint8_t, 4> read_pixel(ID3D10Device *device,
                                       ID3D10Texture2D *texture,
                                       std::uint32_t x, std::uint32_t y) {
    D3D10_TEXTURE2D_DESC desc {};
    texture->GetDesc(&desc);
    desc.Usage = D3D10_USAGE_STAGING;
    desc.BindFlags = 0;
    desc.CPUAccessFlags = D3D10_CPU_ACCESS_READ;
    desc.MiscFlags = 0;
    ID3D10Texture2D *staging = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&desc, nullptr, &staging)),
        "could not create D3D10 staging texture");
    device->CopyResource(staging, texture);
    D3D10_MAPPED_TEXTURE2D mapped {};
    require(SUCCEEDED(staging->Map(0, D3D10_MAP_READ, 0, &mapped)),
        "could not map D3D10 staging texture");
    std::array<std::uint8_t, 4> pixel {};
    std::memcpy(pixel.data(), static_cast<const std::uint8_t *>(mapped.pData) +
        static_cast<std::size_t>(y) * mapped.RowPitch + x * 4, pixel.size());
    staging->Unmap(0);
    release(staging);
    return pixel;
}

void test_d3d10_geometry_capture() {
    ID3D10Device *device = nullptr;
    require(SUCCEEDED(D3D10CreateDevice(nullptr, D3D10_DRIVER_TYPE_WARP, nullptr, 0,
        D3D10_SDK_VERSION, &device)), "could not create D3D10 WARP device");

    constexpr char vertex_source[] =
        "struct Input { float2 position:POSITION; float2 uv:TEXCOORD0; };"
        "struct Output { float4 position:SV_Position; float2 uv:TEXCOORD0; };"
        "Output main(Input i) { Output o; o.position=float4(i.position,0.5,1);"
        "o.uv=i.uv; return o; }";
    constexpr char pixel_source[] =
        "Texture2D<float4> tex:register(t5); SamplerState samp:register(s5);"
        "float4 main(float2 uv:TEXCOORD0):SV_Target{return tex.Sample(samp,uv);}";
    auto *vertex_bytecode = compile(vertex_source, "vs_4_0");
    auto *pixel_bytecode = compile(pixel_source, "ps_4_0");
    ID3D10VertexShader *vertex_shader = nullptr;
    ID3D10PixelShader *pixel_shader = nullptr;
    require(SUCCEEDED(device->CreateVertexShader(vertex_bytecode->GetBufferPointer(),
        vertex_bytecode->GetBufferSize(), &vertex_shader)), "could not create D3D10 VS");
    require(SUCCEEDED(device->CreatePixelShader(pixel_bytecode->GetBufferPointer(),
        pixel_bytecode->GetBufferSize(), &pixel_shader)), "could not create D3D10 PS");
    const auto uv = inspect_uv(vertex_bytecode->GetBufferPointer(),
                               vertex_bytecode->GetBufferSize());
    require(uv.valid(), "D3D10 reflection did not find the UV varying");

    struct Vertex { float x, y, u, v; };
    constexpr std::array<Vertex, 3> vertices {{
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 0.0f,  1.0f, 0.5f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
    }};
    D3D10_BUFFER_DESC vertex_desc {};
    vertex_desc.ByteWidth = sizeof(vertices);
    vertex_desc.Usage = D3D10_USAGE_IMMUTABLE;
    vertex_desc.BindFlags = D3D10_BIND_VERTEX_BUFFER;
    const D3D10_SUBRESOURCE_DATA vertex_data {vertices.data(), 0, 0};
    ID3D10Buffer *vertex_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&vertex_desc, &vertex_data, &vertex_buffer)),
        "could not create D3D10 vertex buffer");
    constexpr std::array<std::uint16_t, 3> indices {0, 1, 2};
    auto index_desc = vertex_desc;
    index_desc.ByteWidth = sizeof(indices);
    index_desc.BindFlags = D3D10_BIND_INDEX_BUFFER;
    const D3D10_SUBRESOURCE_DATA index_data {indices.data(), 0, 0};
    ID3D10Buffer *index_buffer = nullptr;
    require(SUCCEEDED(device->CreateBuffer(&index_desc, &index_data, &index_buffer)),
        "could not create D3D10 index buffer");
    const std::array<D3D10_INPUT_ELEMENT_DESC, 2> elements {{
        {"POSITION", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 0,
            D3D10_INPUT_PER_VERTEX_DATA, 0},
        {"TEXCOORD", 0, DXGI_FORMAT_R32G32_FLOAT, 0, 8,
            D3D10_INPUT_PER_VERTEX_DATA, 0},
    }};
    ID3D10InputLayout *input_layout = nullptr;
    require(SUCCEEDED(device->CreateInputLayout(elements.data(), elements.size(),
        vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize(),
        &input_layout)), "could not create D3D10 input layout");

    D3D10_TEXTURE2D_DESC target_desc {};
    target_desc.Width = target_desc.Height = 8;
    target_desc.MipLevels = target_desc.ArraySize = 1;
    target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1;
    target_desc.Usage = D3D10_USAGE_DEFAULT;
    target_desc.BindFlags = D3D10_BIND_RENDER_TARGET;
    ID3D10Texture2D *target = nullptr;
    ID3D10RenderTargetView *target_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&target_desc, nullptr, &target)) &&
        SUCCEEDED(device->CreateRenderTargetView(target, nullptr, &target_view)),
        "could not create D3D10 target");
    ID3D10Texture2D *second_target = nullptr;
    ID3D10RenderTargetView *second_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&target_desc, nullptr, &second_target)) &&
        SUCCEEDED(device->CreateRenderTargetView(second_target, nullptr, &second_view)),
        "could not create second D3D10 target");

    std::array<std::uint8_t, 4 * 4 * 4> source_pixels {};
    for (std::size_t i = 0; i < source_pixels.size(); i += 4) {
        source_pixels[i] = 16; source_pixels[i + 1] = 32;
        source_pixels[i + 2] = 240; source_pixels[i + 3] = 191;
    }
    D3D10_TEXTURE2D_DESC source_desc {};
    source_desc.Width = source_desc.Height = 4;
    source_desc.MipLevels = source_desc.ArraySize = 1;
    source_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    source_desc.SampleDesc.Count = 1;
    source_desc.Usage = D3D10_USAGE_IMMUTABLE;
    source_desc.BindFlags = D3D10_BIND_SHADER_RESOURCE;
    const D3D10_SUBRESOURCE_DATA source_data {source_pixels.data(), 16, 0};
    ID3D10Texture2D *source_texture = nullptr;
    ID3D10ShaderResourceView *source_view = nullptr;
    require(SUCCEEDED(device->CreateTexture2D(&source_desc, &source_data, &source_texture)) &&
        SUCCEEDED(device->CreateShaderResourceView(source_texture, nullptr, &source_view)),
        "could not create D3D10 source texture");
    D3D10_SAMPLER_DESC sampler_desc {};
    sampler_desc.Filter = D3D10_FILTER_MIN_MAG_MIP_POINT;
    sampler_desc.AddressU = sampler_desc.AddressV = sampler_desc.AddressW =
        D3D10_TEXTURE_ADDRESS_CLAMP;
    sampler_desc.MaxLOD = D3D10_FLOAT32_MAX;
    ID3D10SamplerState *sampler = nullptr;
    require(SUCCEEDED(device->CreateSamplerState(&sampler_desc, &sampler)),
        "could not create D3D10 sampler");

    constexpr UINT stride = sizeof(Vertex), offset = 0;
    device->IASetInputLayout(input_layout);
    device->IASetPrimitiveTopology(D3D10_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    device->IASetVertexBuffers(0, 1, &vertex_buffer, &stride, &offset);
    device->IASetIndexBuffer(index_buffer, DXGI_FORMAT_R16_UINT, 0);
    device->VSSetShader(vertex_shader);
    device->PSSetShader(pixel_shader);
    device->PSSetShaderResources(5, 1, &source_view);
    device->PSSetSamplers(5, 1, &sampler);
    ID3D10RenderTargetView *targets[2] {target_view, second_view};
    device->OMSetRenderTargets(2, targets, nullptr);
    D3D10_BLEND_DESC blend_desc {};
    blend_desc.BlendEnable[0] = FALSE;
    blend_desc.RenderTargetWriteMask[0] = D3D10_COLOR_WRITE_ENABLE_ALL;
    ID3D10BlendState *blend_state = nullptr;
    require(SUCCEEDED(device->CreateBlendState(&blend_desc, &blend_state)),
        "could not create D3D10 blend state");
    constexpr FLOAT blend_factor[4] {0.25f, 0.5f, 0.75f, 1.0f};
    constexpr UINT sample_mask = 0x5a5a5a5bu;
    device->OMSetBlendState(blend_state, blend_factor, sample_mask);
    D3D10_DEPTH_STENCIL_DESC depth_desc {};
    depth_desc.DepthEnable = FALSE;
    depth_desc.DepthWriteMask = D3D10_DEPTH_WRITE_MASK_ZERO;
    depth_desc.DepthFunc = D3D10_COMPARISON_ALWAYS;
    ID3D10DepthStencilState *depth_state = nullptr;
    require(SUCCEEDED(device->CreateDepthStencilState(&depth_desc, &depth_state)),
        "could not create D3D10 depth state");
    device->OMSetDepthStencilState(depth_state, 37);
    const D3D10_VIEWPORT viewport {0, 0, 8, 8, 0.0f, 1.0f};
    device->RSSetViewports(1, &viewport);

    neuralpass::d3d10_capture::SurfaceCapture capture;
    require(capture.initialize(device, 8, 8), "could not initialize D3D10 capture");
    const auto capabilities = capture.capabilities();
    require(capture.backend() == neuralpass::capture::GraphicsBackend::d3d10 &&
        capabilities.direct_draws && capabilities.indexed_draws &&
        !capabilities.indirect_draws && capabilities.replacement_textures &&
        capabilities.asynchronous_readback,
        "D3D10 backend reported inaccurate capabilities");
    constexpr std::uint64_t material = 0xfedcba9876543210ull;
    neuralpass::capture::ReplacementMip replacement;
    replacement.width = replacement.height = 4;
    replacement.rgba.resize(4 * 4 * 4);
    replacement.coverage.resize(4 * 4, 2);
    for (std::size_t i = 0; i < replacement.rgba.size(); i += 4) {
        replacement.rgba[i] = 200; replacement.rgba[i + 1] = 10;
        replacement.rgba[i + 2] = 20; replacement.rgba[i + 3] = 191;
    }
    for (std::size_t y = 0; y < 4; ++y) replacement.coverage[y * 4] = 0;
    capture.queue_replacement(material, {std::move(replacement)});
    constexpr std::uint64_t second_material = 0x0badf00d12344321ull;
    neuralpass::capture::ReplacementMip second_replacement;
    second_replacement.width = second_replacement.height = 4;
    second_replacement.rgba.resize(4 * 4 * 4);
    second_replacement.coverage.resize(4 * 4, 2);
    for (std::size_t i = 0; i < second_replacement.rgba.size(); i += 4) {
        second_replacement.rgba[i] = 12; second_replacement.rgba[i + 1] = 210;
        second_replacement.rgba[i + 2] = 30; second_replacement.rgba[i + 3] = 191;
    }
    capture.queue_replacement(second_material, {std::move(second_replacement)});
    neuralpass::capture::DrawCommand draw;
    draw.kind = neuralpass::capture::DrawKind::indexed;
    draw.vertex_or_index_count = 3;
    draw.instance_count = 1;
    require(capture.replay(device, uv, material, draw, 5),
        "D3D10 indexed replay failed");
    const auto covered = read_pixel(device, target, 7, 7);
    const auto uncovered = read_pixel(device, target, 0, 7);
    require(std::abs(static_cast<int>(covered[0]) - 200) <= 1 &&
        std::abs(static_cast<int>(covered[1]) - 10) <= 1 &&
        std::abs(static_cast<int>(covered[2]) - 20) <= 1,
        "D3D10 covered texel was not replaced");
    require(std::abs(static_cast<int>(uncovered[0]) - 16) <= 1 &&
        std::abs(static_cast<int>(uncovered[1]) - 32) <= 1 &&
        std::abs(static_cast<int>(uncovered[2]) - 240) <= 1,
        "D3D10 uncovered texel did not retain source color");
    draw.kind = neuralpass::capture::DrawKind::direct;
    require(capture.replay(device, uv, second_material, draw, 5),
        "D3D10 direct replay failed");
    const auto second_covered = read_pixel(device, target, 7, 7);
    require(std::abs(static_cast<int>(second_covered[0]) - 12) <= 1 &&
        std::abs(static_cast<int>(second_covered[1]) - 210) <= 1 &&
        std::abs(static_cast<int>(second_covered[2]) - 30) <= 1,
        "D3D10 same-source material reused another replacement");
    draw.kind = neuralpass::capture::DrawKind::indexed;
    require(capture.replay(device, uv, material, draw, 5),
        "D3D10 original same-source material could not be restored");
    const auto restored_replacement = read_pixel(device, target, 7, 7);
    require(std::abs(static_cast<int>(restored_replacement[0]) - 200) <= 1 &&
        std::abs(static_cast<int>(restored_replacement[1]) - 10) <= 1 &&
        std::abs(static_cast<int>(restored_replacement[2]) - 20) <= 1,
        "D3D10 replacement resources contaminated each other");

    ID3D10PixelShader *restored_shader = nullptr;
    ID3D10BlendState *restored_blend = nullptr;
    ID3D10DepthStencilState *restored_depth = nullptr;
    FLOAT restored_factor[4] {};
    UINT restored_mask = 0, restored_stencil = 0;
    device->PSGetShader(&restored_shader);
    device->OMGetBlendState(&restored_blend, restored_factor, &restored_mask);
    device->OMGetDepthStencilState(&restored_depth, &restored_stencil);
    require(restored_shader == pixel_shader && restored_blend == blend_state &&
        restored_depth == depth_state && restored_mask == sample_mask &&
        restored_stencil == 37, "D3D10 replay did not restore pipeline state");
    release(restored_shader); release(restored_blend); release(restored_depth);
    ID3D10RenderTargetView *restored_targets[2] {};
    ID3D10DepthStencilView *restored_dsv = nullptr;
    device->OMGetRenderTargets(2, restored_targets, &restored_dsv);
    require(restored_targets[0] == target_view && restored_targets[1] == second_view,
        "D3D10 replay did not restore all render targets");
    release(restored_targets[0]); release(restored_targets[1]); release(restored_dsv);

    require(!capture.finish_frame().has_value(), "D3D10 readback completed before queueing");
    device->Flush();
    std::optional<neuralpass::SurfaceCaptureFrame> captured;
    for (int attempt = 0; attempt < 100 && !captured; ++attempt)
        captured = capture.finish_frame();
    require(captured.has_value(), "D3D10 asynchronous readback did not complete");
    const auto &center = captured->pixels().at(4, 4);
    require(center.material_id == material && std::isfinite(center.u) &&
        std::isfinite(center.v) && std::isfinite(center.source_r) &&
        std::abs(center.framebuffer_depth - 0.5f) < 0.001f,
        "D3D10 captured surface data is invalid");
    require(capture.statistics().replayed_draws == 3 &&
        capture.statistics().replacement_draws == 3,
        "D3D10 capture statistics are incorrect");

    require(capture.initialize(device, 5, 3) && capture.width() == 5 && capture.height() == 3,
        "D3D10 capture resources did not survive resize reinitialization");
    require(!capture.finish_frame().has_value(),
        "D3D10 reinitialization retained stale readback state");
    capture.reset();
    release(depth_state); release(blend_state); release(sampler);
    release(source_view); release(source_texture);
    release(second_view); release(second_target); release(target_view); release(target);
    release(input_layout); release(index_buffer); release(vertex_buffer);
    release(pixel_shader); release(vertex_shader);
    release(pixel_bytecode); release(vertex_bytecode); release(device);
}

} // namespace

int main() {
    try {
        test_d3d10_geometry_capture();
        test_d3d10_geometry_capture();
        std::cout << "NeuralPass D3D10 surface capture and device recreation test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
