#pragma once

#include "neuralpass/types.hpp"

#include <memory>
#include <cstdint>
#include <string>
#include <string_view>

namespace neuralpass {

struct InferenceGenerations {
    std::uint64_t scene = 0;
    std::uint64_t style = 0;
    std::uint64_t binding = 0;
};

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    virtual void set_generations(InferenceGenerations) noexcept {}
    virtual Image<Color> run(const Image<Color> &input) = 0;
};

[[nodiscard]] std::unique_ptr<InferenceBackend> make_preview_backend(std::string preset);

#ifdef NEURALPASS_HAS_ONNXRUNTIME
[[nodiscard]] std::unique_ptr<InferenceBackend> make_onnx_backend(const std::string &model_path,
                                                                  bool use_directml,
                                                                  std::uint32_t directml_device_id = 0);
#endif

#ifdef _WIN32
[[nodiscard]] std::unique_ptr<InferenceBackend> make_worker_backend(
    const std::string &worker_path, const std::string &model_path,
    bool use_directml, std::uint32_t directml_device_id = 0);
#endif

} // namespace neuralpass
