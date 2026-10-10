#include "neuralpass/inference.hpp"
#include "neuralpass/worker_protocol.hpp"

#include <Windows.h>

#include <algorithm>
#include <chrono>
#include <filesystem>
#include <mutex>
#include <stdexcept>
#include <string>
#include <vector>

namespace neuralpass {
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

std::wstring quote_argument(const std::wstring &value) {
    std::wstring result = L"\"";
    std::size_t slashes = 0;
    for (const wchar_t ch : value) {
        if (ch == L'\\') {
            ++slashes;
        } else if (ch == L'\"') {
            result.append(slashes * 2 + 1, L'\\');
            result.push_back(ch);
            slashes = 0;
        } else {
            result.append(slashes, L'\\');
            slashes = 0;
            result.push_back(ch);
        }
    }
    result.append(slashes * 2, L'\\');
    result.push_back(L'\"');
    return result;
}

class WorkerBackend final : public InferenceBackend {
public:
    WorkerBackend(std::string worker_path, std::string model_path,
                  bool directml, std::uint32_t device_id)
        : worker_path_(std::filesystem::path(std::move(worker_path)).wstring()),
          model_path_(std::filesystem::path(std::move(model_path)).wstring()),
          directml_(directml), device_id_(device_id),
          display_(directml ? "worker/directml" : "worker/cpu") {
        start();
    }

    ~WorkerBackend() override { stop(); }
    std::string_view name() const noexcept override { return display_; }
    bool ready() const noexcept override { return pipe_ != INVALID_HANDLE_VALUE; }
    void set_generations(InferenceGenerations generations) noexcept override {
        generations_ = generations;
    }

    Image<Color> run(const Image<Color> &input) override {
        std::lock_guard lock(mutex_);
        try {
            return exchange(input);
        } catch (...) {
            // A worker is disposable. Restart once so a provider/device reset does
            // not permanently disable inference in the host process.
            stop();
            start();
            return exchange(input);
        }
    }

private:
    void start() {
        if (!std::filesystem::is_regular_file(worker_path_))
            throw std::runtime_error("NeuralPassWorker.exe is missing");
        const auto nonce = std::to_wstring(GetCurrentProcessId()) + L"-" +
            std::to_wstring(GetTickCount64()) + L"-" +
            std::to_wstring(reinterpret_cast<std::uintptr_t>(this));
        pipe_name_ = L"\\\\.\\pipe\\NeuralPass-" + nonce;
        mapping_name_ = L"Local\\NeuralPass-" + nonce;
        mapping_ = CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0,
                                      static_cast<DWORD>(worker::k_shared_bytes),
                                      mapping_name_.c_str());
        if (mapping_ == nullptr) throw std::runtime_error("could not create worker shared memory");
        shared_ = static_cast<float *>(MapViewOfFile(
            mapping_, FILE_MAP_ALL_ACCESS, 0, 0, worker::k_shared_bytes));
        if (shared_ == nullptr) {
            stop();
            throw std::runtime_error("could not map worker shared memory");
        }
        std::wstring command = quote_argument(worker_path_) + L" --pipe " +
            quote_argument(pipe_name_) + L" --mapping " + quote_argument(mapping_name_) +
            L" --model " + quote_argument(model_path_) + L" --provider " +
            (directml_ ? L"directml" : L"cpu") + L" --device " +
            std::to_wstring(device_id_);
        // Test-only fault injection is inherited by the first child. Tests clear
        // it before the retry, proving that a real broken pipe is recoverable.
        wchar_t fault_request[32] = {};
        if (GetEnvironmentVariableW(L"NEURALPASS_WORKER_TEST_EXIT_REQUEST",
                                    fault_request, 32) != 0)
            command += L" --test-exit-request " + quote_argument(fault_request);
        std::vector<wchar_t> mutable_command(command.begin(), command.end());
        mutable_command.push_back(L'\0');
        STARTUPINFOW startup {};
        startup.cb = sizeof(startup);
        PROCESS_INFORMATION process {};
        const auto directory = std::filesystem::path(worker_path_).parent_path().wstring();
        if (!CreateProcessW(worker_path_.c_str(), mutable_command.data(), nullptr, nullptr,
                            FALSE, CREATE_NO_WINDOW, nullptr,
                            directory.empty() ? nullptr : directory.c_str(),
                            &startup, &process)) {
            stop();
            throw std::runtime_error("could not start NeuralPassWorker.exe");
        }
        process_ = process.hProcess;
        CloseHandle(process.hThread);
        const auto deadline = GetTickCount64() + 10000;
        while (GetTickCount64() < deadline) {
            pipe_ = CreateFileW(pipe_name_.c_str(), GENERIC_READ | GENERIC_WRITE,
                                0, nullptr, OPEN_EXISTING, 0, nullptr);
            if (pipe_ != INVALID_HANDLE_VALUE) break;
            if (WaitForSingleObject(process_, 0) == WAIT_OBJECT_0) break;
            WaitNamedPipeW(pipe_name_.c_str(), 100);
        }
        if (pipe_ == INVALID_HANDLE_VALUE) {
            stop();
            throw std::runtime_error("NeuralPassWorker.exe did not open its control pipe");
        }
        worker::Message hello;
        hello.type = worker::MessageType::hello;
        worker::Message reply;
        if (!write_exact(pipe_, &hello, sizeof(hello)) ||
            !read_exact(pipe_, &reply, sizeof(reply)) || !worker::valid(reply) ||
            reply.type != worker::MessageType::hello_ack) {
            stop();
            throw std::runtime_error("NeuralPass worker protocol handshake failed");
        }
    }

    void stop() noexcept {
        if (pipe_ != INVALID_HANDLE_VALUE) {
            worker::Message message;
            message.type = worker::MessageType::shutdown;
            (void)write_exact(pipe_, &message, sizeof(message));
            CloseHandle(pipe_);
            pipe_ = INVALID_HANDLE_VALUE;
        }
        if (process_ != nullptr) {
            if (WaitForSingleObject(process_, 2000) == WAIT_TIMEOUT)
                TerminateProcess(process_, 1);
            CloseHandle(process_);
            process_ = nullptr;
        }
        if (shared_ != nullptr) {
            UnmapViewOfFile(shared_);
            shared_ = nullptr;
        }
        if (mapping_ != nullptr) {
            CloseHandle(mapping_);
            mapping_ = nullptr;
        }
    }

    Image<Color> exchange(const Image<Color> &input) {
        if (pipe_ == INVALID_HANDLE_VALUE || shared_ == nullptr)
            throw std::runtime_error("NeuralPass worker is unavailable");
        worker::Message request;
        request.type = worker::MessageType::infer;
        request.request = ++request_id_;
        request.scene_generation = generations_.scene;
        request.style_generation = generations_.style;
        request.binding_generation = generations_.binding;
        request.width = input.width();
        request.height = input.height();
        request.payload_bytes = static_cast<std::uint32_t>(
            input.size() * 4 * sizeof(float));
        if (!worker::valid_image(request))
            throw std::runtime_error("inference tile exceeds bounded worker mailbox");
        for (std::size_t index = 0; index < input.size(); ++index) {
            shared_[index * 4 + 0] = input.pixels()[index].r;
            shared_[index * 4 + 1] = input.pixels()[index].g;
            shared_[index * 4 + 2] = input.pixels()[index].b;
            shared_[index * 4 + 3] = input.pixels()[index].a;
        }
        worker::Message reply;
        if (!write_exact(pipe_, &request, sizeof(request)) ||
            !read_exact(pipe_, &reply, sizeof(reply)))
            throw std::runtime_error("NeuralPass worker disconnected");
        if (!worker::matches_inference(request, reply))
            throw std::runtime_error("NeuralPass rejected a stale or invalid worker result");
        Image<Color> output(input.width(), input.height());
        for (std::size_t index = 0; index < output.size(); ++index)
            output.pixels()[index] = {shared_[index * 4 + 0], shared_[index * 4 + 1],
                                      shared_[index * 4 + 2], shared_[index * 4 + 3]};
        return output;
    }

    std::wstring worker_path_;
    std::wstring model_path_;
    bool directml_ = false;
    std::uint32_t device_id_ = 0;
    std::string display_;
    InferenceGenerations generations_;
    std::uint64_t request_id_ = 0;
    std::wstring pipe_name_;
    std::wstring mapping_name_;
    HANDLE mapping_ = nullptr;
    float *shared_ = nullptr;
    HANDLE pipe_ = INVALID_HANDLE_VALUE;
    HANDLE process_ = nullptr;
    std::mutex mutex_;
};

} // namespace

std::unique_ptr<InferenceBackend> make_worker_backend(
        const std::string &worker_path, const std::string &model_path,
        bool use_directml, std::uint32_t directml_device_id) {
    return std::make_unique<WorkerBackend>(worker_path, model_path,
                                           use_directml, directml_device_id);
}

} // namespace neuralpass
