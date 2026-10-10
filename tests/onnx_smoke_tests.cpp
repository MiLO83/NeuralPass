#include "neuralpass/inference.hpp"

#include <cmath>
#include <cstdlib>
#include <iostream>
#include <stdexcept>
#include <string>
#ifdef _WIN32
#include <Windows.h>
#include <dxgi.h>
#endif

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

void run_backend(const std::string &model, bool directml, std::uint32_t device_id) {
#ifdef _WIN32
    if (directml) {
        IDXGIFactory *factory = nullptr;
        IDXGIAdapter *adapter = nullptr;
        require(SUCCEEDED(CreateDXGIFactory(__uuidof(IDXGIFactory),
            reinterpret_cast<void **>(&factory))), "could not enumerate DXGI adapters");
        const HRESULT selected = factory->EnumAdapters(device_id, &adapter);
        factory->Release();
        require(SUCCEEDED(selected) && adapter != nullptr,
            "DirectML device ID does not identify a DXGI adapter");
        DXGI_ADAPTER_DESC desc {};
        require(SUCCEEDED(adapter->GetDesc(&desc)), "could not describe DXGI adapter");
        adapter->Release();
        std::wcout << L"DirectML adapter " << device_id << L": " << desc.Description
                   << L" (vendor=0x" << std::hex << desc.VendorId
                   << L", device=0x" << desc.DeviceId << std::dec << L")\n";
    }
#endif
    auto backend = neuralpass::make_onnx_backend(model, directml, device_id);
    require(backend != nullptr && backend->ready(), "ONNX backend is not ready");
    require(backend->name() == (directml ? "onnx/directml" : "onnx/cpu"),
        "ONNX provider name is inaccurate");
    neuralpass::Image<neuralpass::Color> input(64, 64);
    for (std::uint32_t y = 0; y < input.height(); ++y)
        for (std::uint32_t x = 0; x < input.width(); ++x)
            input.at(x, y) = {
                static_cast<float>(x) / 63.0f,
                static_cast<float>(y) / 63.0f,
                static_cast<float>(x + y) / 126.0f,
                1.0f};
    const auto output = backend->run(input);
    require(output.width() == input.width() && output.height() == input.height(),
        "ONNX output dimensions differ from the input");
    double sum = 0.0;
    for (const auto &pixel : output.pixels()) {
        require(std::isfinite(pixel.r) && std::isfinite(pixel.g) &&
                std::isfinite(pixel.b), "ONNX output contains non-finite color");
        require(pixel.r >= 0.0f && pixel.r <= 1.0f &&
                pixel.g >= 0.0f && pixel.g <= 1.0f &&
                pixel.b >= 0.0f && pixel.b <= 1.0f,
            "ONNX output is outside normalized color range");
        sum += pixel.r + pixel.g + pixel.b;
    }
    require(sum > 1.0, "ONNX output is unexpectedly empty");
}

} // namespace

int main(int argc, char **argv) {
    try {
        require(argc == 3 || argc == 4,
            "usage: neuralpass_onnx_smoke_tests MODEL_PATH cpu|directml [device-id]");
        const std::string provider = argv[2];
        require(provider == "cpu" || provider == "directml",
            "provider must be cpu or directml");
        const auto device_id = argc == 4
            ? static_cast<std::uint32_t>(std::stoul(argv[3])) : 0u;
        run_backend(argv[1], provider == "directml", device_id);
        std::cout << "NeuralPass ONNX " << provider << " smoke test passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
