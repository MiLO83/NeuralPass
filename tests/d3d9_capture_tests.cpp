#include "../addon/d3d9_surface_capture.hpp"

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
    const auto result = D3DCompile(source, std::strlen(source), "d3d9-capture-test",
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

LRESULT CALLBACK window_proc(HWND window, UINT message, WPARAM wparam, LPARAM lparam) {
    return DefWindowProcW(window, message, wparam, lparam);
}

HWND make_window() {
    const wchar_t class_name[] = L"NeuralPassD3D9CaptureTest";
    WNDCLASSW window_class {};
    window_class.lpfnWndProc = window_proc;
    window_class.hInstance = GetModuleHandleW(nullptr);
    window_class.lpszClassName = class_name;
    RegisterClassW(&window_class);
    return CreateWindowExW(0, class_name, L"NeuralPass D3D9 test", WS_OVERLAPPED,
        0, 0, 16, 16, nullptr, nullptr, window_class.hInstance, nullptr);
}

D3DPRESENT_PARAMETERS presentation(HWND window) {
    D3DPRESENT_PARAMETERS present {};
    present.BackBufferWidth = present.BackBufferHeight = 8;
    present.BackBufferFormat = D3DFMT_A8R8G8B8;
    present.BackBufferCount = 1;
    present.MultiSampleType = D3DMULTISAMPLE_NONE;
    present.SwapEffect = D3DSWAPEFFECT_DISCARD;
    present.hDeviceWindow = window;
    present.Windowed = TRUE;
    present.PresentationInterval = D3DPRESENT_INTERVAL_IMMEDIATE;
    return present;
}

IDirect3DDevice9 *make_device(HWND window, IDirect3D9 **direct3d_out) {
    auto *direct3d = Direct3DCreate9(D3D_SDK_VERSION);
    require(direct3d != nullptr, "Direct3DCreate9 failed");
    auto present = presentation(window);
    IDirect3DDevice9 *device = nullptr;
    const std::array<D3DDEVTYPE, 2> types {D3DDEVTYPE_HAL, D3DDEVTYPE_REF};
    for (const auto type : types) {
        if (SUCCEEDED(direct3d->CreateDevice(D3DADAPTER_DEFAULT, type, window,
                D3DCREATE_SOFTWARE_VERTEXPROCESSING | D3DCREATE_FPU_PRESERVE,
                &present, &device))) break;
    }
    if (device == nullptr) {
        release(direct3d);
        throw std::runtime_error("could not create D3D9 HAL or reference device");
    }
    *direct3d_out = direct3d;
    return device;
}

std::array<std::uint8_t, 4> read_pixel(IDirect3DDevice9 *device,
                                       IDirect3DSurface9 *source,
                                       UINT x, UINT y) {
    D3DSURFACE_DESC desc {};
    source->GetDesc(&desc);
    IDirect3DSurface9 *staging = nullptr;
    require(SUCCEEDED(device->CreateOffscreenPlainSurface(desc.Width, desc.Height,
        desc.Format, D3DPOOL_SYSTEMMEM, &staging, nullptr)),
        "could not create D3D9 staging surface");
    require(SUCCEEDED(device->GetRenderTargetData(source, staging)),
        "could not read D3D9 render target");
    D3DLOCKED_RECT mapped {};
    require(SUCCEEDED(staging->LockRect(&mapped, nullptr, D3DLOCK_READONLY)),
        "could not lock D3D9 staging surface");
    const auto *pixel = static_cast<const std::uint8_t *>(mapped.pBits) +
        static_cast<std::size_t>(y) * mapped.Pitch + x * 4;
    // A8R8G8B8 is BGRA in little-endian memory; expose RGBA to assertions.
    const std::array<std::uint8_t, 4> result {pixel[2], pixel[1], pixel[0], pixel[3]};
    staging->UnlockRect();
    release(staging);
    return result;
}

void fill_texture(IDirect3DTexture9 *texture, std::array<std::uint8_t, 4> rgba) {
    D3DLOCKED_RECT mapped {};
    require(SUCCEEDED(texture->LockRect(0, &mapped, nullptr, 0)),
        "could not lock D3D9 texture fixture");
    for (UINT y = 0; y < 4; ++y) {
        auto *row = static_cast<std::uint8_t *>(mapped.pBits) + y * mapped.Pitch;
        for (UINT x = 0; x < 4; ++x) {
            row[x * 4 + 0] = rgba[2]; row[x * 4 + 1] = rgba[1];
            row[x * 4 + 2] = rgba[0]; row[x * 4 + 3] = rgba[3];
        }
    }
    texture->UnlockRect(0);
}

neuralpass::capture::ReplacementMip replacement(
        std::uint8_t red, std::uint8_t green, std::uint8_t blue) {
    neuralpass::capture::ReplacementMip result;
    result.width = result.height = 4;
    result.rgba.resize(4 * 4 * 4);
    result.coverage.resize(4 * 4, 2);
    for (std::size_t index = 0; index < result.rgba.size(); index += 4) {
        result.rgba[index] = red;
        result.rgba[index + 1] = green;
        result.rgba[index + 2] = blue;
        result.rgba[index + 3] = 191;
    }
    return result;
}

void test_d3d9_geometry_capture() {
    const auto window = make_window();
    require(window != nullptr, "could not create D3D9 test window");
    IDirect3D9 *direct3d = nullptr;
    auto *device = make_device(window, &direct3d);
    D3DCAPS9 device_caps {};
    require(SUCCEEDED(device->GetDeviceCaps(&device_caps)),
        "could not query D3D9 test device capabilities");

    constexpr char vertex_source[] =
        "struct Input { float2 position:POSITION0; float2 uv:TEXCOORD0; };"
        "struct Output { float4 position:POSITION0; float2 uv:TEXCOORD0; };"
        "Output main(Input i) { Output o; o.position=float4(i.position,0.5,1);"
        "o.uv=i.uv; return o; }";
    constexpr char pixel_source[] =
        "sampler2D source_texture:register(s5);"
        "float4 main(float2 uv:TEXCOORD0):COLOR0{return tex2D(source_texture,uv);}";
    auto *vertex_bytecode = compile(vertex_source, "vs_3_0");
    auto *pixel_bytecode = compile(pixel_source, "ps_3_0");
    const auto uv = neuralpass::d3d9_capture::inspect_uv_output(
        vertex_bytecode->GetBufferPointer(), vertex_bytecode->GetBufferSize());
    require(uv.valid() && uv.name == "TEXCOORD" && uv.index == 0,
        "D3D9 token parser did not find the rasterized UV output");
    std::array<DWORD, 2> malformed {D3DVS_VERSION(3, 0), 0x7f00001fu};
    require(!neuralpass::d3d9_capture::inspect_uv_output(
        malformed.data(), sizeof(malformed)).valid(),
        "D3D9 token parser accepted malformed bytecode");
    IDirect3DVertexShader9 *vertex_shader = nullptr;
    IDirect3DPixelShader9 *pixel_shader = nullptr;
    require(SUCCEEDED(device->CreateVertexShader(
        static_cast<const DWORD *>(vertex_bytecode->GetBufferPointer()), &vertex_shader)),
        "could not create D3D9 vertex shader");
    require(SUCCEEDED(device->CreatePixelShader(
        static_cast<const DWORD *>(pixel_bytecode->GetBufferPointer()), &pixel_shader)),
        "could not create D3D9 pixel shader");

    const D3DVERTEXELEMENT9 elements[] {
        {0, 0, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_POSITION, 0},
        {0, 8, D3DDECLTYPE_FLOAT2, D3DDECLMETHOD_DEFAULT, D3DDECLUSAGE_TEXCOORD, 0},
        D3DDECL_END(),
    };
    IDirect3DVertexDeclaration9 *declaration = nullptr;
    require(SUCCEEDED(device->CreateVertexDeclaration(elements, &declaration)),
        "could not create D3D9 vertex declaration");
    struct Vertex { float x, y, u, v; };
    constexpr std::array<Vertex, 3> vertices {{
        {-1.0f, -1.0f, 0.0f, 1.0f},
        { 0.0f,  1.0f, 0.5f, 0.0f},
        { 1.0f, -1.0f, 1.0f, 1.0f},
    }};
    IDirect3DVertexBuffer9 *vertex_buffer = nullptr;
    require(SUCCEEDED(device->CreateVertexBuffer(sizeof(vertices), 0, 0,
        D3DPOOL_MANAGED, &vertex_buffer, nullptr)), "could not create D3D9 vertex buffer");
    void *vertex_memory = nullptr;
    vertex_buffer->Lock(0, sizeof(vertices), &vertex_memory, 0);
    std::memcpy(vertex_memory, vertices.data(), sizeof(vertices));
    vertex_buffer->Unlock();
    constexpr std::array<std::uint16_t, 3> indices {0, 1, 2};
    IDirect3DIndexBuffer9 *index_buffer = nullptr;
    require(SUCCEEDED(device->CreateIndexBuffer(sizeof(indices), 0, D3DFMT_INDEX16,
        D3DPOOL_MANAGED, &index_buffer, nullptr)), "could not create D3D9 index buffer");
    void *index_memory = nullptr;
    index_buffer->Lock(0, sizeof(indices), &index_memory, 0);
    std::memcpy(index_memory, indices.data(), sizeof(indices));
    index_buffer->Unlock();
    IDirect3DTexture9 *source_texture = nullptr;
    require(SUCCEEDED(device->CreateTexture(4, 4, 1, 0, D3DFMT_A8R8G8B8,
        D3DPOOL_MANAGED, &source_texture, nullptr)), "could not create D3D9 source texture");
    fill_texture(source_texture, {16, 32, 240, 191});

    device->SetVertexDeclaration(declaration);
    device->SetStreamSource(0, vertex_buffer, 0, sizeof(Vertex));
    device->SetIndices(index_buffer);
    device->SetVertexShader(vertex_shader);
    device->SetPixelShader(pixel_shader);
    device->SetTexture(5, source_texture);
    device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    D3DVIEWPORT9 viewport {0, 0, 8, 8, 0.0f, 1.0f};
    device->SetViewport(&viewport);
    IDirect3DSurface9 *backbuffer = nullptr;
    device->GetRenderTarget(0, &backbuffer);

    neuralpass::d3d9_capture::SurfaceCapture capture;
    require(capture.initialize(device, 8, 8),
        "could not initialize D3D9 four-target surface capture");
    const auto capabilities = capture.capabilities();
    require(capture.backend() == neuralpass::capture::GraphicsBackend::d3d9 &&
        capabilities.direct_draws && capabilities.indexed_draws &&
        !capabilities.indirect_draws && capabilities.replacement_textures &&
        !capabilities.asynchronous_readback,
        "D3D9 backend reported inaccurate capabilities");
    constexpr std::uint64_t material = 0xfedcba9876543210ull;
    capture.queue_replacement(material, {replacement(200, 10, 20)});
    constexpr std::uint64_t second_material = 0x0badf00d12344321ull;
    capture.queue_replacement(second_material, {replacement(12, 210, 30)});
    neuralpass::capture::DrawCommand draw;
    draw.kind = neuralpass::capture::DrawKind::indexed;
    draw.vertex_or_index_count = 3;
    draw.instance_count = 1;
    draw.primitive_topology = 4; // reshade::api::primitive_topology::triangle_list
    require(SUCCEEDED(device->BeginScene()), "could not begin D3D9 test scene");
    require(capture.replay(device, uv, material, draw, 5), "D3D9 indexed replay failed");
    require(SUCCEEDED(device->EndScene()), "could not end D3D9 test scene");
    if (capture.statistics().replayed_draws != 1)
        std::cerr << "D3D9 capture draw was not accepted by the native device\n";
    const auto replacement_pixel = read_pixel(device, backbuffer, 4, 5);
    require(std::abs(static_cast<int>(replacement_pixel[0]) - 200) <= 2 &&
        std::abs(static_cast<int>(replacement_pixel[1]) - 10) <= 2 &&
        std::abs(static_cast<int>(replacement_pixel[2]) - 20) <= 2,
        "D3D9 application draw did not receive its replacement");
    draw.kind = neuralpass::capture::DrawKind::direct;
    require(SUCCEEDED(device->BeginScene()), "could not begin second D3D9 scene");
    require(capture.replay(device, uv, second_material, draw, 5), "D3D9 direct replay failed");
    require(SUCCEEDED(device->EndScene()), "could not end second D3D9 scene");
    const auto second_pixel = read_pixel(device, backbuffer, 4, 5);
    require(std::abs(static_cast<int>(second_pixel[0]) - 12) <= 2 &&
        std::abs(static_cast<int>(second_pixel[1]) - 210) <= 2 &&
        std::abs(static_cast<int>(second_pixel[2]) - 30) <= 2,
        "D3D9 same-source material reused another replacement");
    draw.kind = neuralpass::capture::DrawKind::indexed;
    require(SUCCEEDED(device->BeginScene()), "could not begin third D3D9 scene");
    require(capture.replay(device, uv, material, draw, 5),
        "D3D9 original same-source material could not be restored");
    require(SUCCEEDED(device->EndScene()), "could not end third D3D9 scene");
    const auto restored_pixel = read_pixel(device, backbuffer, 4, 5);
    require(std::abs(static_cast<int>(restored_pixel[0]) - 200) <= 2 &&
        std::abs(static_cast<int>(restored_pixel[1]) - 10) <= 2 &&
        std::abs(static_cast<int>(restored_pixel[2]) - 20) <= 2,
        "D3D9 replacement resources contaminated each other");

    IDirect3DPixelShader9 *restored_shader = nullptr;
    IDirect3DBaseTexture9 *restored_texture = nullptr;
    IDirect3DSurface9 *restored_target = nullptr;
    device->GetPixelShader(&restored_shader);
    device->GetTexture(5, &restored_texture);
    device->GetRenderTarget(0, &restored_target);
    require(restored_shader == pixel_shader && restored_texture == source_texture &&
        restored_target == backbuffer, "D3D9 replay did not restore native state");
    release(restored_shader); release(restored_texture); release(restored_target);

    require(!capture.finish_frame().has_value(), "D3D9 readback completed before queueing");
    device->Present(nullptr, nullptr, nullptr, nullptr);
    std::optional<neuralpass::SurfaceCaptureFrame> captured;
    for (int attempt = 0; attempt < 100 && !captured; ++attempt) {
        captured = capture.finish_frame();
        if (!captured) device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    require(captured.has_value(), "D3D9 asynchronous capture did not complete");
    const auto &center = captured->pixels().at(4, 5);
    if (!(center.material_id == material && std::isfinite(center.u) &&
          std::isfinite(center.v) && std::isfinite(center.source_r) &&
          std::isnan(center.framebuffer_depth)))
        std::cerr << "D3D9 center: material=" << std::hex << center.material_id
                  << std::dec << " uv=" << center.u << ',' << center.v
                  << " source=" << center.source_r << ',' << center.source_g << ','
                  << center.source_b << ',' << center.source_a
                  << " depth=" << center.framebuffer_depth << '\n';
    std::size_t supported_pixels = 0;
    for (const auto &pixel : captured->pixels().pixels())
        if (pixel.material_id != 0) ++supported_pixels;
    if (supported_pixels == 0) std::cerr << "D3D9 capture surface is entirely empty\n";
    require(center.material_id == material && std::isfinite(center.u) &&
        std::isfinite(center.v) && std::isfinite(center.source_r) &&
        std::isnan(center.framebuffer_depth),
        "D3D9 captured material/UV/source contract is invalid");
    require(capture.statistics().replayed_draws == 3 &&
        capture.statistics().replacement_draws == 3,
        "D3D9 capture statistics are incorrect");

    std::cout << "D3D9 test device: "
              << (device_caps.DeviceType == D3DDEVTYPE_HAL ? "HAL" : "reference") << '\n';

    // ReShade emits destroy_swapchain(resize=true) before IDirect3DDevice9::Reset.
    // Mirror that exact lifecycle: release every NeuralPass default-pool object,
    // reset the real device, then rebuild and prove replay/readback still works.
    capture.reset();
    release(backbuffer);
    auto reset_present = presentation(window);
    require(SUCCEEDED(device->Reset(&reset_present)),
        "D3D9 device reset failed after capture teardown");
    require(SUCCEEDED(device->GetRenderTarget(0, &backbuffer)) && backbuffer != nullptr,
        "D3D9 reset did not recreate its backbuffer");
    device->SetVertexDeclaration(declaration);
    device->SetStreamSource(0, vertex_buffer, 0, sizeof(Vertex));
    device->SetIndices(index_buffer);
    device->SetVertexShader(vertex_shader);
    device->SetPixelShader(pixel_shader);
    device->SetTexture(5, source_texture);
    device->SetSamplerState(5, D3DSAMP_MINFILTER, D3DTEXF_POINT);
    device->SetSamplerState(5, D3DSAMP_MAGFILTER, D3DTEXF_POINT);
    device->SetSamplerState(5, D3DSAMP_MIPFILTER, D3DTEXF_NONE);
    device->SetRenderState(D3DRS_ZENABLE, FALSE);
    device->SetRenderState(D3DRS_ALPHABLENDENABLE, FALSE);
    device->SetViewport(&viewport);
    require(capture.initialize(device, 8, 8) && capture.width() == 8 && capture.height() == 8,
        "D3D9 capture resources did not recreate after device reset");
    require(!capture.finish_frame().has_value(),
        "D3D9 reset recreation retained stale readback state");
    capture.queue_replacement(material, {replacement(80, 90, 220)});
    require(SUCCEEDED(device->BeginScene()), "could not begin post-reset D3D9 scene");
    require(capture.replay(device, uv, material, draw, 5),
        "D3D9 replay failed after device reset");
    require(SUCCEEDED(device->EndScene()), "could not end post-reset D3D9 scene");
    const auto post_reset_pixel = read_pixel(device, backbuffer, 4, 5);
    require(std::abs(static_cast<int>(post_reset_pixel[0]) - 80) <= 2 &&
        std::abs(static_cast<int>(post_reset_pixel[1]) - 90) <= 2 &&
        std::abs(static_cast<int>(post_reset_pixel[2]) - 220) <= 2,
        "D3D9 post-reset draw did not receive its rebuilt replacement");
    device->Present(nullptr, nullptr, nullptr, nullptr);
    std::optional<neuralpass::SurfaceCaptureFrame> post_reset_capture;
    for (int attempt = 0; attempt < 100 && !post_reset_capture; ++attempt) {
        post_reset_capture = capture.finish_frame();
        if (!post_reset_capture) device->Present(nullptr, nullptr, nullptr, nullptr);
    }
    require(post_reset_capture.has_value() &&
        post_reset_capture->pixels().at(4, 5).material_id == material,
        "D3D9 capture did not produce a valid frame after device reset");
    capture.reset();
    release(backbuffer); release(source_texture); release(index_buffer);
    release(vertex_buffer); release(declaration); release(pixel_shader);
    release(vertex_shader); release(pixel_bytecode); release(vertex_bytecode);
    release(device); release(direct3d);
    DestroyWindow(window);
}

} // namespace

int main() {
    try {
        test_d3d9_geometry_capture();
        std::cout << "NeuralPass D3D9 surface capture test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
