#include "neuralpass/inference.hpp"

#include <onnxruntime_cxx_api.h>
#ifdef NEURALPASS_USE_DIRECTML
#include <dml_provider_factory.h>
#endif

#include <algorithm>
#include <array>
#include <stdexcept>
#include <vector>

namespace neuralpass {
namespace {

Color sample_bilinear(const Image<Color> &image, float x, float y) {
    x = std::clamp(x, 0.0f, static_cast<float>(image.width() - 1));
    y = std::clamp(y, 0.0f, static_cast<float>(image.height() - 1));
    const auto x0 = static_cast<std::uint32_t>(x);
    const auto y0 = static_cast<std::uint32_t>(y);
    const auto x1 = std::min(x0 + 1, image.width() - 1);
    const auto y1 = std::min(y0 + 1, image.height() - 1);
    const float tx = x - static_cast<float>(x0);
    const float ty = y - static_cast<float>(y0);
    const auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const Color top {
        lerp(image.at(x0, y0).r, image.at(x1, y0).r, tx),
        lerp(image.at(x0, y0).g, image.at(x1, y0).g, tx),
        lerp(image.at(x0, y0).b, image.at(x1, y0).b, tx), 1.0f};
    const Color bottom {
        lerp(image.at(x0, y1).r, image.at(x1, y1).r, tx),
        lerp(image.at(x0, y1).g, image.at(x1, y1).g, tx),
        lerp(image.at(x0, y1).b, image.at(x1, y1).b, tx), 1.0f};
    return {lerp(top.r, bottom.r, ty), lerp(top.g, bottom.g, ty),
            lerp(top.b, bottom.b, ty), 1.0f};
}

Image<Color> resize_image(const Image<Color> &input, std::uint32_t width, std::uint32_t height) {
    if (input.width() == width && input.height() == height) return input;
    Image<Color> output(width, height);
    const float scale_x = static_cast<float>(input.width()) / static_cast<float>(width);
    const float scale_y = static_cast<float>(input.height()) / static_cast<float>(height);
    for (std::uint32_t y = 0; y < height; ++y)
        for (std::uint32_t x = 0; x < width; ++x)
            output.at(x, y) = sample_bilinear(
                input, (static_cast<float>(x) + 0.5f) * scale_x - 0.5f,
                (static_cast<float>(y) + 0.5f) * scale_y - 0.5f);
    return output;
}

class OnnxBackend final : public InferenceBackend {
public:
    OnnxBackend(const std::string &path, bool use_directml)
        : env_(ORT_LOGGING_LEVEL_WARNING, "NeuralPass") {
        Ort::SessionOptions options;
        options.SetGraphOptimizationLevel(GraphOptimizationLevel::ORT_ENABLE_ALL);
#ifdef NEURALPASS_USE_DIRECTML
        if (use_directml) {
            options.DisableMemPattern();
            options.SetExecutionMode(ExecutionMode::ORT_SEQUENTIAL);
            Ort::ThrowOnError(OrtSessionOptionsAppendExecutionProvider_DML(options, 0));
        }
#else
        (void)use_directml;
#endif
#ifdef _WIN32
        const std::wstring wide(path.begin(), path.end());
        session_ = std::make_unique<Ort::Session>(env_, wide.c_str(), options);
#else
        session_ = std::make_unique<Ort::Session>(env_, path.c_str(), options);
#endif
        Ort::AllocatorWithDefaultOptions allocator;
        input_name_ = session_->GetInputNameAllocated(0, allocator).get();
        output_name_ = session_->GetOutputNameAllocated(0, allocator).get();
        const auto input_shape = session_->GetInputTypeInfo(0).GetTensorTypeAndShapeInfo().GetShape();
        if (input_shape.size() == 4) {
            if (input_shape[2] > 0) input_height_ = static_cast<std::uint32_t>(input_shape[2]);
            if (input_shape[3] > 0) input_width_ = static_cast<std::uint32_t>(input_shape[3]);
        }
    }

    std::string_view name() const noexcept override { return "onnx"; }
    bool ready() const noexcept override { return session_ != nullptr; }

    Image<Color> run(const Image<Color> &input) override {
        const auto prepared = resize_image(input,
            input_width_ != 0 ? input_width_ : input.width(),
            input_height_ != 0 ? input_height_ : input.height());
        const auto h = static_cast<std::int64_t>(prepared.height());
        const auto w = static_cast<std::int64_t>(prepared.width());
        std::vector<float> chw(static_cast<std::size_t>(3*h*w));
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) {
                const auto &c = prepared.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y));
                const auto i = static_cast<std::size_t>(y*w+x);
                chw[i] = c.r * 255.0f;
                chw[static_cast<std::size_t>(h*w)+i] = c.g * 255.0f;
                chw[static_cast<std::size_t>(2*h*w)+i] = c.b * 255.0f;
            }
        const std::array<std::int64_t,4> shape {1,3,h,w};
        auto memory = Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);
        auto tensor = Ort::Value::CreateTensor<float>(memory, chw.data(), chw.size(), shape.data(), shape.size());
        const char *inputs[] = {input_name_.c_str()};
        const char *outputs[] = {output_name_.c_str()};
        auto result = session_->Run(Ort::RunOptions{nullptr}, inputs, &tensor, 1, outputs, 1);
        const float *data = result.front().GetTensorData<float>();
        const auto shape_out = result.front().GetTensorTypeAndShapeInfo().GetShape();
        if (shape_out.size() != 4 || shape_out[2] != h || shape_out[3] != w)
            throw std::runtime_error("model output must be NCHW and match the input tile size");
        Image<Color> image(static_cast<std::uint32_t>(w), static_cast<std::uint32_t>(h));
        for (std::int64_t y = 0; y < h; ++y)
            for (std::int64_t x = 0; x < w; ++x) {
                const auto i = static_cast<std::size_t>(y*w+x);
                image.at(static_cast<std::uint32_t>(x), static_cast<std::uint32_t>(y)) = {
                    std::clamp(data[i] / 255.0f, 0.0f, 1.0f),
                    std::clamp(data[static_cast<std::size_t>(h*w)+i] / 255.0f, 0.0f, 1.0f),
                    std::clamp(data[static_cast<std::size_t>(2*h*w)+i] / 255.0f, 0.0f, 1.0f), 1.0f};
            }
        return resize_image(image, input.width(), input.height());
    }

private:
    Ort::Env env_;
    std::unique_ptr<Ort::Session> session_;
    std::string input_name_;
    std::string output_name_;
    std::uint32_t input_width_ = 0;
    std::uint32_t input_height_ = 0;
};

} // namespace

std::unique_ptr<InferenceBackend> make_onnx_backend(const std::string &path, bool use_directml) {
    return std::make_unique<OnnxBackend>(path, use_directml);
}

} // namespace neuralpass
