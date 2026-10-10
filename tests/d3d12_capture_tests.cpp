#include "d3d12_surface_capture.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <d3dcompiler.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

using Microsoft::WRL::ComPtr;

namespace {

template <typename T> void release(T *&object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void test_warp_surfaces_and_barriers() {
    IDXGIFactory6 *factory = nullptr;
    IDXGIAdapter *adapter = nullptr;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *queue = nullptr;
    ID3D12CommandAllocator *allocator = nullptr;
    ID3D12GraphicsCommandList *commands = nullptr;
    ID3D12Fence *fence = nullptr;
    HANDLE event = nullptr;
    try {
        require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))),
                "could not create DXGI factory");
        require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),
                "could not enumerate WARP adapter");
        require(SUCCEEDED(D3D12CreateDevice(adapter, D3D_FEATURE_LEVEL_11_0,
                                            IID_PPV_ARGS(&device))),
                "could not create D3D12 WARP device");
        D3D12_COMMAND_QUEUE_DESC queue_desc {};
        queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
        require(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))),
                "could not create D3D12 command queue");
        require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
                                                         IID_PPV_ARGS(&allocator))),
                "could not create D3D12 command allocator");
        require(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
                allocator, nullptr, IID_PPV_ARGS(&commands))),
                "could not create D3D12 command list");

        neuralpass::d3d12_capture::SurfaceCapture capture;
        capture.attach_native_queue(queue);
        neuralpass::capture::SurfaceCaptureBackend &backend = capture;
        const auto capabilities = backend.capabilities();
        require(backend.backend() == neuralpass::capture::GraphicsBackend::d3d12 &&
                capabilities.direct_draws && capabilities.indexed_draws &&
                capabilities.indirect_draws && capabilities.replacement_textures &&
                capabilities.asynchronous_readback,
                "D3D12 backend reported inaccurate capabilities");
        require(backend.initialize(device, 8, 8),
                "could not initialize D3D12 capture surfaces");
        require(backend.width() == 8 && backend.height() == 8,
                "D3D12 capture dimensions were not retained");
        require(!backend.finish_frame(commands, false).has_value(),
                "paced D3D12 poll unexpectedly queued or completed a capture");
        require(!backend.finish_frame(commands).has_value(),
                "first D3D12 readback unexpectedly completed synchronously");
        require(SUCCEEDED(commands->Close()), "could not close D3D12 command list");
        ID3D12CommandList *lists[] {commands};
        queue->ExecuteCommandLists(1, lists);
        require(capture.signal_submitted(), "capture readback fence was not signaled");
        require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE,
                                              IID_PPV_ARGS(&fence))),
                "could not create completion fence");
        require(SUCCEEDED(queue->Signal(fence, 1)), "could not signal completion fence");
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        require(event != nullptr, "could not create completion event");
        require(SUCCEEDED(fence->SetEventOnCompletion(1, event)),
                "could not arm completion event");
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0,
                "D3D12 WARP capture commands timed out");
        require(SUCCEEDED(allocator->Reset()), "could not reset D3D12 allocator");
        require(SUCCEEDED(commands->Reset(allocator, nullptr)),
                "could not reset D3D12 command list");
        const auto frame = backend.finish_frame(commands);
        require(frame.has_value() && frame->width() == 8 && frame->height() == 8,
                "D3D12 readback ring did not return the completed capture");
        require(SUCCEEDED(commands->Close()), "could not close second D3D12 command list");
        queue->ExecuteCommandLists(1, lists);
        require(capture.signal_submitted(), "second capture readback fence was not signaled");
        require(SUCCEEDED(queue->Signal(fence, 2)), "could not signal second completion fence");
        require(SUCCEEDED(fence->SetEventOnCompletion(2, event)),
                "could not arm second completion event");
        require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0,
                "second D3D12 WARP capture command timed out");
        require(backend.initialize(device, 5, 3) &&
                backend.width() == 5 && backend.height() == 3,
                "D3D12 capture resources did not survive resize reinitialization");
        capture.reset();
    } catch (...) {
        if (event != nullptr) CloseHandle(event);
        release(fence); release(commands); release(allocator); release(queue);
        release(device); release(adapter); release(factory);
        throw;
    }
    if (event != nullptr) CloseHandle(event);
    release(fence); release(commands); release(allocator); release(queue);
    release(device); release(adapter); release(factory);
}

void test_descriptor_replacement_isolation() {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))),
            "replacement test could not create DXGI factory");
    require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),
            "replacement test could not enumerate WARP");
    require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device))),
            "replacement test could not create WARP device");

    D3D12_DESCRIPTOR_RANGE range {};
    range.RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    range.NumDescriptors = 1;
    range.BaseShaderRegister = 0;
    range.OffsetInDescriptorsFromTableStart = 0;
    D3D12_ROOT_PARAMETER parameter {};
    parameter.ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    parameter.DescriptorTable = {1, &range};
    parameter.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    D3D12_STATIC_SAMPLER_DESC sampler {};
    sampler.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
    sampler.AddressU = sampler.AddressV = sampler.AddressW =
        D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
    sampler.ShaderRegister = 0;
    sampler.ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    sampler.MaxLOD = D3D12_FLOAT32_MAX;
    D3D12_ROOT_SIGNATURE_DESC root_desc {};
    root_desc.NumParameters = 1;
    root_desc.pParameters = &parameter;
    root_desc.NumStaticSamplers = 1;
    root_desc.pStaticSamplers = &sampler;
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, errors;
    require(SUCCEEDED(D3D12SerializeRootSignature(&root_desc,
            D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors)),
            "could not serialize replacement root signature");
    ComPtr<ID3D12RootSignature> root;
    require(SUCCEEDED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&root))),
            "could not create replacement root signature");

    constexpr char shader[] =
        "Texture2D<float4> tex:register(t0); SamplerState smp:register(s0);"
        "float4 vs(uint id:SV_VertexID):SV_Position {"
        "float2 p=float2((id<<1)&2,id&2); return float4(p*float2(2,-2)+float2(-1,1),0,1); }"
        "float4 ps(float4 p:SV_Position):SV_Target { return tex.SampleLevel(smp,float2(.5,.5),0); }";
    ComPtr<ID3DBlob> vs, ps;
    require(SUCCEEDED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr,
            "vs", "vs_5_0", 0, 0, &vs, &errors)), "could not compile replacement VS");
    require(SUCCEEDED(D3DCompile(shader, sizeof(shader), nullptr, nullptr, nullptr,
            "ps", "ps_5_0", 0, 0, &ps, &errors)), "could not compile replacement PS");
    D3D12_GRAPHICS_PIPELINE_STATE_DESC pso_desc {};
    pso_desc.pRootSignature = root.Get();
    pso_desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    pso_desc.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    pso_desc.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    pso_desc.SampleMask = UINT_MAX;
    pso_desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    pso_desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    pso_desc.RasterizerState.DepthClipEnable = TRUE;
    pso_desc.DepthStencilState.DepthEnable = FALSE;
    pso_desc.DepthStencilState.StencilEnable = FALSE;
    pso_desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    pso_desc.NumRenderTargets = 1;
    pso_desc.RTVFormats[0] = DXGI_FORMAT_R8G8B8A8_UNORM;
    pso_desc.SampleDesc.Count = 1;
    ComPtr<ID3D12PipelineState> pso;
    require(SUCCEEDED(device->CreateGraphicsPipelineState(&pso_desc,
            IID_PPV_ARGS(&pso))), "could not create replacement PSO");

    D3D12_HEAP_PROPERTIES default_heap {};
    default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    auto texture_desc = [](UINT width, D3D12_RESOURCE_FLAGS flags) {
        D3D12_RESOURCE_DESC desc {};
        desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        desc.Width = width; desc.Height = 1; desc.DepthOrArraySize = 1;
        desc.MipLevels = 1; desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
        desc.SampleDesc.Count = 1; desc.Flags = flags;
        return desc;
    };
    const auto sample_desc = texture_desc(1, D3D12_RESOURCE_FLAG_NONE);
    const auto target_desc = texture_desc(3, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET);
    ComPtr<ID3D12Resource> original, replacement, target;
    require(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &sample_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&original))), "could not create original texture");
    require(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &sample_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&replacement))), "could not create replacement texture");
    require(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr,
            IID_PPV_ARGS(&target))), "could not create isolation target");

    D3D12_HEAP_PROPERTIES upload_heap {}; upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
    D3D12_RESOURCE_DESC buffer_desc {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    buffer_desc.Width = 1024; buffer_desc.Height = 1; buffer_desc.DepthOrArraySize = 1;
    buffer_desc.MipLevels = 1; buffer_desc.SampleDesc.Count = 1;
    buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> upload;
    require(SUCCEEDED(device->CreateCommittedResource(&upload_heap, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
            IID_PPV_ARGS(&upload))), "could not create replacement upload");
    std::uint8_t *mapped = nullptr;
    require(SUCCEEDED(upload->Map(0, nullptr, reinterpret_cast<void **>(&mapped))),
            "could not map replacement upload");
    mapped[0] = 255; mapped[1] = 0; mapped[2] = 0; mapped[3] = 255;
    mapped[512] = 0; mapped[513] = 255; mapped[514] = 0; mapped[515] = 255;
    upload->Unmap(0, nullptr);

    D3D12_DESCRIPTOR_HEAP_DESC srv_heap_desc {};
    srv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV;
    srv_heap_desc.NumDescriptors = 2;
    srv_heap_desc.Flags = D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE;
    ComPtr<ID3D12DescriptorHeap> srv_heap;
    require(SUCCEEDED(device->CreateDescriptorHeap(&srv_heap_desc, IID_PPV_ARGS(&srv_heap))),
            "could not create isolation SRV heap");
    const auto descriptor_size = device->GetDescriptorHandleIncrementSize(
        D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    auto original_cpu = srv_heap->GetCPUDescriptorHandleForHeapStart();
    auto replacement_cpu = original_cpu; replacement_cpu.ptr += descriptor_size;
    D3D12_SHADER_RESOURCE_VIEW_DESC srv {};
    srv.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    srv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    srv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    srv.Texture2D.MipLevels = 1;
    device->CreateShaderResourceView(original.Get(), &srv, original_cpu);
    device->CopyDescriptorsSimple(1, replacement_cpu, original_cpu,
                                  D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
    device->CreateShaderResourceView(replacement.Get(), &srv, replacement_cpu);

    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
    rtv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap))),
            "could not create isolation RTV heap");
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);

    ComPtr<ID3D12CommandQueue> queue;
    D3D12_COMMAND_QUEUE_DESC queue_desc {}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    require(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))),
            "could not create isolation queue");
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))), "could not create isolation allocator");
    require(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), pso.Get(), IID_PPV_ARGS(&list))),
            "could not create isolation command list");
    for (UINT index = 0; index < 2; ++index) {
        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = upload.Get(); source.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        source.PlacedFootprint.Offset = index * 512;
        source.PlacedFootprint.Footprint = {DXGI_FORMAT_R8G8B8A8_UNORM, 1, 1, 1, 256};
        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = index == 0 ? original.Get() : replacement.Get();
        destination.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        list->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
    }
    D3D12_RESOURCE_BARRIER barriers[2] {};
    for (UINT index = 0; index < 2; ++index) {
        barriers[index].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[index].Transition.pResource = index == 0 ? original.Get() : replacement.Get();
        barriers[index].Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
        barriers[index].Transition.StateAfter = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        barriers[index].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    }
    list->ResourceBarrier(2, barriers);
    ID3D12DescriptorHeap *heaps[] {srv_heap.Get()};
    list->SetDescriptorHeaps(1, heaps);
    list->SetGraphicsRootSignature(root.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    auto original_gpu = srv_heap->GetGPUDescriptorHandleForHeapStart();
    auto replacement_gpu = original_gpu; replacement_gpu.ptr += descriptor_size;
    const D3D12_RECT scissors[] {{0, 0, 1, 1}, {1, 0, 2, 1}, {2, 0, 3, 1}};
    const D3D12_GPU_DESCRIPTOR_HANDLE tables[] {
        original_gpu, replacement_gpu, original_gpu};
    for (UINT index = 0; index < 3; ++index) {
        const D3D12_VIEWPORT viewport {
            static_cast<float>(index), 0.0f, 1.0f, 1.0f, 0.0f, 1.0f};
        list->RSSetViewports(1, &viewport);
        list->RSSetScissorRects(1, &scissors[index]);
        list->SetGraphicsRootDescriptorTable(0, tables[index]);
        list->DrawInstanced(3, 1, 0, 0);
    }

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT read_footprint {};
    std::uint64_t read_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &read_footprint,
                                  nullptr, nullptr, &read_size);
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    buffer_desc.Width = read_size;
    ComPtr<ID3D12Resource> readback;
    require(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))), "could not create isolation readback");
    D3D12_RESOURCE_BARRIER target_barrier {};
    target_barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    target_barrier.Transition.pResource = target.Get();
    target_barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    target_barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    target_barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &target_barrier);
    D3D12_TEXTURE_COPY_LOCATION target_source {};
    target_source.pResource = target.Get();
    target_source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION read_destination {};
    read_destination.pResource = readback.Get();
    read_destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    read_destination.PlacedFootprint = read_footprint;
    list->CopyTextureRegion(&read_destination, 0, 0, 0, &target_source, nullptr);
    require(SUCCEEDED(list->Close()), "could not close isolation command list");
    ID3D12CommandList *lists[] {list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
            "could not create isolation fence");
    require(SUCCEEDED(queue->Signal(fence.Get(), 1)), "could not signal isolation fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "could not create isolation event");
    require(SUCCEEDED(fence->SetEventOnCompletion(1, event)), "could not arm isolation event");
    require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0,
            "replacement isolation draw timed out");
    CloseHandle(event);
    const std::uint8_t *pixels = nullptr;
    D3D12_RANGE read_range {0, static_cast<SIZE_T>(read_size)};
    require(SUCCEEDED(readback->Map(0, &read_range,
            reinterpret_cast<void **>(const_cast<std::uint8_t **>(&pixels)))),
            "could not map isolation result");
    require(pixels[0] > 240 && pixels[1] < 16 &&
            pixels[4] < 16 && pixels[5] > 240 &&
            pixels[8] > 240 && pixels[9] < 16,
            "shadow descriptor leaked into the original table or was not restored");
    D3D12_RANGE no_write {0, 0}; readback->Unmap(0, &no_write);
}

void test_backend_replay_restores_pso_and_target() {
    ComPtr<IDXGIFactory6> factory;
    ComPtr<IDXGIAdapter> adapter;
    ComPtr<ID3D12Device> device;
    require(SUCCEEDED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory))),
            "replay test could not create DXGI factory");
    require(SUCCEEDED(factory->EnumWarpAdapter(IID_PPV_ARGS(&adapter))),
            "replay test could not enumerate WARP");
    require(SUCCEEDED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0,
                                        IID_PPV_ARGS(&device))),
            "replay test could not create WARP device");
    D3D12_ROOT_SIGNATURE_DESC root_desc {};
    root_desc.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> root_blob, errors;
    require(SUCCEEDED(D3D12SerializeRootSignature(&root_desc,
            D3D_ROOT_SIGNATURE_VERSION_1, &root_blob, &errors)),
            "could not serialize replay root signature");
    ComPtr<ID3D12RootSignature> root;
    require(SUCCEEDED(device->CreateRootSignature(0, root_blob->GetBufferPointer(),
            root_blob->GetBufferSize(), IID_PPV_ARGS(&root))),
            "could not create replay root signature");
    constexpr char source[] =
        "struct O{float4 p:SV_Position;float2 uv:TEXCOORD0;};"
        "O vs(uint id:SV_VertexID){float2 q=float2((id<<1)&2,id&2);O o;"
        "o.p=float4(q*float2(2,-2)+float2(-1,1),0,1);o.uv=q;return o;}"
        "float4 ps(O i):SV_Target{return float4(1,0,0,1);}"
        "struct C{uint4 id:SV_Target0;float4 g:SV_Target1;"
        "float4 s:SV_Target2;float d:SV_Target3;};"
        "C capture(O i){C o;o.id=uint4(1,2,asuint(i.uv.x),asuint(i.uv.y));"
        "o.g=0;o.s=0;o.d=i.p.z;return o;}";
    ComPtr<ID3DBlob> vs, ps, capture_ps;
    require(SUCCEEDED(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr,
            "vs", "vs_5_0", 0, 0, &vs, &errors)), "could not compile replay VS");
    require(SUCCEEDED(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr,
            "ps", "ps_5_0", 0, 0, &ps, &errors)), "could not compile replay PS");
    require(SUCCEEDED(D3DCompile(source, sizeof(source), nullptr, nullptr, nullptr,
            "capture", "ps_5_0", 0, 0, &capture_ps, &errors)),
            "could not compile replay capture PS");
    auto make_pso = [&](ID3DBlob *pixel, std::array<DXGI_FORMAT, 4> formats,
                        UINT count, ComPtr<ID3D12PipelineState> &out) {
        D3D12_GRAPHICS_PIPELINE_STATE_DESC desc {};
        desc.pRootSignature = root.Get();
        desc.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
        desc.PS = {pixel->GetBufferPointer(), pixel->GetBufferSize()};
        for (UINT index = 0; index < count; ++index) {
            desc.RTVFormats[index] = formats[index];
            desc.BlendState.RenderTarget[index].RenderTargetWriteMask =
                D3D12_COLOR_WRITE_ENABLE_ALL;
        }
        desc.NumRenderTargets = count;
        desc.SampleMask = UINT_MAX;
        desc.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
        desc.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
        desc.RasterizerState.DepthClipEnable = TRUE;
        desc.DepthStencilState.DepthEnable = FALSE;
        desc.DepthStencilState.StencilEnable = FALSE;
        desc.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
        desc.SampleDesc.Count = 1;
        return SUCCEEDED(device->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&out)));
    };
    ComPtr<ID3D12PipelineState> original_pso, capture_pso;
    require(make_pso(ps.Get(), {DXGI_FORMAT_R8G8B8A8_UNORM}, 1, original_pso),
            "could not create original replay PSO");
    require(make_pso(capture_ps.Get(), {DXGI_FORMAT_R32G32B32A32_UINT,
            DXGI_FORMAT_R32G32B32A32_FLOAT, DXGI_FORMAT_R32G32B32A32_FLOAT,
            DXGI_FORMAT_R32_FLOAT}, 4, capture_pso),
            "could not create capture replay PSO");

    D3D12_HEAP_PROPERTIES default_heap {}; default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
    D3D12_RESOURCE_DESC target_desc {};
    target_desc.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    target_desc.Width = 2; target_desc.Height = 1; target_desc.DepthOrArraySize = 1;
    target_desc.MipLevels = 1; target_desc.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    target_desc.SampleDesc.Count = 1; target_desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE clear_value {}; clear_value.Format = target_desc.Format;
    ComPtr<ID3D12Resource> target;
    require(SUCCEEDED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
            &target_desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear_value,
            IID_PPV_ARGS(&target))), "could not create replay target");
    D3D12_DESCRIPTOR_HEAP_DESC rtv_heap_desc {};
    rtv_heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV; rtv_heap_desc.NumDescriptors = 1;
    ComPtr<ID3D12DescriptorHeap> rtv_heap;
    require(SUCCEEDED(device->CreateDescriptorHeap(&rtv_heap_desc, IID_PPV_ARGS(&rtv_heap))),
            "could not create replay RTV heap");
    const auto rtv = rtv_heap->GetCPUDescriptorHandleForHeapStart();
    device->CreateRenderTargetView(target.Get(), nullptr, rtv);
    D3D12_COMMAND_QUEUE_DESC queue_desc {}; queue_desc.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ComPtr<ID3D12CommandQueue> queue;
    ComPtr<ID3D12CommandAllocator> allocator;
    ComPtr<ID3D12GraphicsCommandList> list;
    require(SUCCEEDED(device->CreateCommandQueue(&queue_desc, IID_PPV_ARGS(&queue))),
            "could not create replay queue");
    require(SUCCEEDED(device->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT,
            IID_PPV_ARGS(&allocator))), "could not create replay allocator");
    require(SUCCEEDED(device->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT,
            allocator.Get(), original_pso.Get(), IID_PPV_ARGS(&list))),
            "could not create replay command list");
    list->SetGraphicsRootSignature(root.Get());
    list->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    list->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    const float black[4] {};
    list->ClearRenderTargetView(rtv, black, 0, nullptr);
    const D3D12_RECT scissors[] {{0, 0, 1, 1}, {1, 0, 2, 1}};
    D3D12_VIEWPORT viewport {0, 0, 1, 1, 0, 1};
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissors[0]);

    neuralpass::d3d12_capture::SurfaceCapture capture;
    capture.attach_native_queue(queue.Get());
    require(capture.initialize(device.Get(), 2, 1), "could not initialize replay backend");
    neuralpass::capture::UvInput uv {"TEXCOORD", 0, 0};
    neuralpass::capture::DrawCommand draw {};
    draw.kind = neuralpass::capture::DrawKind::direct;
    draw.vertex_or_index_count = 3; draw.instance_count = 1;
    draw.pipeline = reinterpret_cast<std::uint64_t>(original_pso.Get());
    draw.render_target_views[0] = rtv.ptr; draw.render_target_count = 1;
    draw.target_compatible = true;
    constexpr std::uint64_t material = 0x1122334455667788ull;
    capture.register_prebuilt_variant(draw.pipeline, uv, material, draw,
        reinterpret_cast<std::uint64_t>(capture_pso.Get()));
    require(capture.replay(list.Get(), uv, material, draw),
            "actual D3D12 backend replay was rejected");
    viewport.TopLeftX = 1;
    list->RSSetViewports(1, &viewport); list->RSSetScissorRects(1, &scissors[1]);
    // Deliberately do not restore the PSO or RTV here. This draw succeeds only
    // when SurfaceCapture::replay restored both before returning.
    list->DrawInstanced(3, 1, 0, 0);

    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    std::uint64_t read_size = 0;
    device->GetCopyableFootprints(&target_desc, 0, 1, 0, &footprint,
                                  nullptr, nullptr, &read_size);
    D3D12_HEAP_PROPERTIES readback_heap {}; readback_heap.Type = D3D12_HEAP_TYPE_READBACK;
    D3D12_RESOURCE_DESC buffer_desc {};
    buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER; buffer_desc.Width = read_size;
    buffer_desc.Height = 1; buffer_desc.DepthOrArraySize = 1; buffer_desc.MipLevels = 1;
    buffer_desc.SampleDesc.Count = 1; buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ComPtr<ID3D12Resource> readback;
    require(SUCCEEDED(device->CreateCommittedResource(&readback_heap, D3D12_HEAP_FLAG_NONE,
            &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
            IID_PPV_ARGS(&readback))), "could not create replay readback");
    D3D12_RESOURCE_BARRIER barrier {};
    barrier.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    barrier.Transition.pResource = target.Get();
    barrier.Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
    barrier.Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    barrier.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    list->ResourceBarrier(1, &barrier);
    D3D12_TEXTURE_COPY_LOCATION source_location {};
    source_location.pResource = target.Get();
    source_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
    D3D12_TEXTURE_COPY_LOCATION destination {};
    destination.pResource = readback.Get();
    destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
    destination.PlacedFootprint = footprint;
    list->CopyTextureRegion(&destination, 0, 0, 0, &source_location, nullptr);
    require(SUCCEEDED(list->Close()), "could not close replay command list");
    ID3D12CommandList *lists[] {list.Get()}; queue->ExecuteCommandLists(1, lists);
    ComPtr<ID3D12Fence> fence;
    require(SUCCEEDED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))),
            "could not create replay fence");
    require(SUCCEEDED(queue->Signal(fence.Get(), 1)), "could not signal replay fence");
    const HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    require(event != nullptr, "could not create replay event");
    require(SUCCEEDED(fence->SetEventOnCompletion(1, event)), "could not arm replay event");
    require(WaitForSingleObject(event, 5000) == WAIT_OBJECT_0, "backend replay timed out");
    CloseHandle(event);
    std::uint8_t *pixels = nullptr;
    D3D12_RANGE read_range {0, static_cast<SIZE_T>(read_size)};
    require(SUCCEEDED(readback->Map(0, &read_range, reinterpret_cast<void **>(&pixels))),
            "could not map replay result");
    require(pixels[0] > 240 && pixels[1] < 16 &&
            pixels[4] > 240 && pixels[5] < 16,
            "backend did not restore the original PSO and render target");
    D3D12_RANGE no_write {0, 0}; readback->Unmap(0, &no_write);
    require(capture.statistics().replayed_draws == 1,
            "backend replay statistic did not advance");
    capture.reset();
}

} // namespace

int main() {
    try {
        for (int device_lifetime = 0; device_lifetime != 2; ++device_lifetime) {
            test_warp_surfaces_and_barriers();
            test_descriptor_replacement_isolation();
            test_backend_replay_restores_pso_and_target();
        }
        std::cout << "D3D12 WARP capture and device recreation tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "D3D12 capture test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
