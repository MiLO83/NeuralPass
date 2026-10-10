#include "neuralpass/inference.hpp"

#include <Windows.h>

#include <cmath>
#include <iostream>
#include <stdexcept>
#include <string>

int main(int argc, char **argv) {
    try {
        if (argc < 3)
            throw std::runtime_error("usage: worker_smoke WORKER MODEL [directml] [device]");
        const bool directml = argc >= 4 && std::string(argv[3]) == "directml";
        const auto device = argc >= 5 ? static_cast<std::uint32_t>(std::stoul(argv[4])) : 0;
        auto backend = neuralpass::make_worker_backend(argv[1], argv[2], directml, device);
        if (!backend || !backend->ready()) throw std::runtime_error("worker backend is not ready");
        backend->set_generations({17, 23, 41});
        neuralpass::Image<neuralpass::Color> input(73, 51);
        for (std::uint32_t y = 0; y < input.height(); ++y)
            for (std::uint32_t x = 0; x < input.width(); ++x)
                input.at(x, y) = {x / 72.0f, y / 50.0f, (x + y) / 122.0f, 1.0f};
        const auto output = backend->run(input);
        if (output.width() != input.width() || output.height() != input.height())
            throw std::runtime_error("worker output dimensions mismatch");
        for (const auto &pixel : output.pixels())
            if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) ||
                !std::isfinite(pixel.b) || pixel.r < 0.0f || pixel.r > 1.0f ||
                pixel.g < 0.0f || pixel.g > 1.0f || pixel.b < 0.0f || pixel.b > 1.0f)
                throw std::runtime_error("worker returned an invalid pixel");
        backend->set_generations({18, 29, 43});
        const auto second = backend->run(input);
        if (second.width() != input.width() || second.height() != input.height())
            throw std::runtime_error("worker failed after generation rollover");

        // A full display frame exceeds the bounded cross-architecture mailbox.
        // The client must fit it coherently, run one request, and restore the
        // original dimensions instead of decomposing it into visible tiles.
        neuralpass::Image<neuralpass::Color> fullscreen(960, 540);
        for (std::uint32_t y = 0; y < fullscreen.height(); ++y)
            for (std::uint32_t x = 0; x < fullscreen.width(); ++x)
                fullscreen.at(x, y) = {
                    x / 959.0f, y / 539.0f, (x + y) / 1498.0f, 1.0f};
        const auto fullscreen_output = backend->run(fullscreen);
        if (fullscreen_output.width() != fullscreen.width() ||
            fullscreen_output.height() != fullscreen.height())
            throw std::runtime_error("worker failed coherent full-frame resizing");
        for (const auto &pixel : fullscreen_output.pixels())
            if (!std::isfinite(pixel.r) || !std::isfinite(pixel.g) ||
                !std::isfinite(pixel.b))
                throw std::runtime_error("worker full-frame output is not finite");
        backend.reset();

        SetEnvironmentVariableW(L"NEURALPASS_WORKER_TEST_EXIT_REQUEST", L"1");
        auto recovering = neuralpass::make_worker_backend(argv[1], argv[2], directml, device);
        SetEnvironmentVariableW(L"NEURALPASS_WORKER_TEST_EXIT_REQUEST", nullptr);
        recovering->set_generations({31, 37, 47});
        const auto recovered = recovering->run(input);
        if (recovered.width() != input.width() || recovered.height() != input.height())
            throw std::runtime_error("worker did not recover after forced process exit");
        std::cout << recovering->name() << " cross-process smoke and recovery test passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Worker smoke test failed: " << error.what() << '\n';
        return 1;
    }
}
