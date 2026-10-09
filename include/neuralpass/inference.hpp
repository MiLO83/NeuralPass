#pragma once

#include "neuralpass/types.hpp"

#include <memory>
#include <string>
#include <string_view>

namespace neuralpass {

class InferenceBackend {
public:
    virtual ~InferenceBackend() = default;
    [[nodiscard]] virtual std::string_view name() const noexcept = 0;
    [[nodiscard]] virtual bool ready() const noexcept = 0;
    virtual Image<Color> run(const Image<Color> &input) = 0;
};

[[nodiscard]] std::unique_ptr<InferenceBackend> make_preview_backend(std::string preset);

#ifdef NEURALPASS_HAS_ONNXRUNTIME
[[nodiscard]] std::unique_ptr<InferenceBackend> make_onnx_backend(const std::string &model_path,
                                                                  bool use_directml);
#endif

} // namespace neuralpass
