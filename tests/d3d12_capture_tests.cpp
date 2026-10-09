#include "d3d12_surface_capture.hpp"

#include <Windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdlib>
#include <iostream>
#include <stdexcept>

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
                capabilities.indirect_draws && !capabilities.replacement_textures &&
                capabilities.asynchronous_readback,
                "D3D12 backend reported inaccurate capabilities");
        require(backend.initialize(device, 8, 8),
                "could not initialize D3D12 capture surfaces");
        require(backend.width() == 8 && backend.height() == 8,
                "D3D12 capture dimensions were not retained");
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

} // namespace

int main() {
    try {
        test_warp_surfaces_and_barriers();
        std::cout << "D3D12 WARP capture tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "D3D12 capture test failure: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
