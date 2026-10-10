#include "neuralpass/inference.hpp"
#include "neuralpass/worker_protocol.hpp"

#include <Windows.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <iostream>
#include <stdexcept>
#include <string>
#include <string_view>

namespace {

bool write_exact(HANDLE handle, const void *data, DWORD size) {
    const auto *cursor = static_cast<const std::uint8_t *>(data);
    while (size != 0) {
        DWORD written = 0;
        if (!WriteFile(handle, cursor, size, &written, nullptr) || written == 0) return false;
        cursor += written;
        size -= written;
    }
    return true;
}

bool read_exact(HANDLE handle, void *data, DWORD size) {
    auto *cursor = static_cast<std::uint8_t *>(data);
    while (size != 0) {
        DWORD received = 0;
        if (!ReadFile(handle, cursor, size, &received, nullptr) || received == 0) return false;
        cursor += received;
        size -= received;
    }
    return true;
}

std::wstring argument(int argc, wchar_t **argv, const wchar_t *name) {
    for (int index = 1; index + 1 < argc; ++index)
        if (std::wstring_view(argv[index]) == name) return argv[index + 1];
    throw std::runtime_error("missing worker argument");
}

std::wstring optional_argument(int argc, wchar_t **argv, const wchar_t *name) {
    for (int index = 1; index + 1 < argc; ++index)
        if (std::wstring_view(argv[index]) == name) return argv[index + 1];
    return {};
}

int run(int argc, wchar_t **argv) {
    const auto pipe_name = argument(argc, argv, L"--pipe");
    const auto mapping_name = argument(argc, argv, L"--mapping");
    const auto model = argument(argc, argv, L"--model");
    const bool directml = argument(argc, argv, L"--provider") == L"directml";
    const auto device = static_cast<std::uint32_t>(std::stoul(argument(argc, argv, L"--device")));
    const auto fault_value = optional_argument(argc, argv, L"--test-exit-request");
    const auto fault_request = fault_value.empty() ? 0 : std::stoull(fault_value);

    HANDLE mapping = OpenFileMappingW(FILE_MAP_ALL_ACCESS, FALSE, mapping_name.c_str());
    if (mapping == nullptr) throw std::runtime_error("could not open shared memory");
    float *shared = static_cast<float *>(MapViewOfFile(
        mapping, FILE_MAP_ALL_ACCESS, 0, 0, neuralpass::worker::k_shared_bytes));
    if (shared == nullptr) {
        CloseHandle(mapping);
        throw std::runtime_error("could not map shared memory");
    }
    HANDLE pipe = CreateNamedPipeW(pipe_name.c_str(), PIPE_ACCESS_DUPLEX,
        PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
        1, sizeof(neuralpass::worker::Message), sizeof(neuralpass::worker::Message),
        0, nullptr);
    if (pipe == INVALID_HANDLE_VALUE) {
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        throw std::runtime_error("could not create control pipe");
    }
    const bool connected = ConnectNamedPipe(pipe, nullptr) != FALSE ||
                           GetLastError() == ERROR_PIPE_CONNECTED;
    if (!connected) {
        CloseHandle(pipe);
        UnmapViewOfFile(shared);
        CloseHandle(mapping);
        throw std::runtime_error("could not connect control pipe");
    }

    auto backend = neuralpass::make_onnx_backend(
        std::filesystem::path(model).string(), directml, device);
    neuralpass::worker::Message message;
    if (!read_exact(pipe, &message, sizeof(message)) ||
        !neuralpass::worker::valid(message) ||
        message.type != neuralpass::worker::MessageType::hello)
        throw std::runtime_error("invalid worker handshake");
    message.type = neuralpass::worker::MessageType::hello_ack;
    if (!write_exact(pipe, &message, sizeof(message)))
        throw std::runtime_error("worker handshake disconnected");

    while (read_exact(pipe, &message, sizeof(message))) {
        if (!neuralpass::worker::valid(message)) break;
        if (message.type == neuralpass::worker::MessageType::shutdown) break;
        if (message.type != neuralpass::worker::MessageType::infer ||
            !neuralpass::worker::valid_image(message)) {
            message.type = neuralpass::worker::MessageType::error;
            message.status = ERROR_INVALID_DATA;
            if (!write_exact(pipe, &message, sizeof(message))) break;
            continue;
        }
        if (fault_request != 0 && message.request == fault_request)
            ExitProcess(86);
        try {
            neuralpass::Image<neuralpass::Color> input(message.width, message.height);
            for (std::size_t index = 0; index < input.size(); ++index)
                input.pixels()[index] = {shared[index * 4 + 0], shared[index * 4 + 1],
                                         shared[index * 4 + 2], shared[index * 4 + 3]};
            auto output = backend->run(input);
            if (output.width() != message.width || output.height() != message.height)
                throw std::runtime_error("worker output dimensions changed");
            for (std::size_t index = 0; index < output.size(); ++index) {
                shared[index * 4 + 0] = output.pixels()[index].r;
                shared[index * 4 + 1] = output.pixels()[index].g;
                shared[index * 4 + 2] = output.pixels()[index].b;
                shared[index * 4 + 3] = output.pixels()[index].a;
            }
            message.type = neuralpass::worker::MessageType::infer_done;
            message.status = 0;
        } catch (...) {
            message.type = neuralpass::worker::MessageType::error;
            message.status = ERROR_PROCESS_ABORTED;
        }
        if (!write_exact(pipe, &message, sizeof(message))) break;
    }
    FlushFileBuffers(pipe);
    DisconnectNamedPipe(pipe);
    CloseHandle(pipe);
    UnmapViewOfFile(shared);
    CloseHandle(mapping);
    return 0;
}

} // namespace

int wmain(int argc, wchar_t **argv) {
    try {
        return run(argc, argv);
    } catch (const std::exception &error) {
        std::cerr << "NeuralPassWorker: " << error.what() << '\n';
        return 1;
    }
}
