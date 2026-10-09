#include "neuralpass/inference.hpp"

#include <algorithm>
#include <cmath>

namespace neuralpass {
namespace {

float clamp01(float v) { return std::clamp(v, 0.0f, 1.0f); }

class PreviewBackend final : public InferenceBackend {
public:
    explicit PreviewBackend(std::string preset)
        : preset_(std::move(preset)), display_("preview/" + preset_) {}
    std::string_view name() const noexcept override { return display_; }
    bool ready() const noexcept override { return true; }

    Image<Color> run(const Image<Color> &input) override {
        Image<Color> output(input.width(), input.height());
        for (std::uint32_t y = 0; y < input.height(); ++y) {
            for (std::uint32_t x = 0; x < input.width(); ++x) {
                const Color c = input.at(x, y);
                Color out = c;
                if (preset_ == "photo-detail") {
                    const auto xl = x == 0 ? x : x - 1;
                    const auto xr = std::min(input.width() - 1, x + 1);
                    const auto yt = y == 0 ? y : y - 1;
                    const auto yb = std::min(input.height() - 1, y + 1);
                    const Color blur {
                        (input.at(xl,y).r + input.at(xr,y).r + input.at(x,yt).r + input.at(x,yb).r) * 0.25f,
                        (input.at(xl,y).g + input.at(xr,y).g + input.at(x,yt).g + input.at(x,yb).g) * 0.25f,
                        (input.at(xl,y).b + input.at(xr,y).b + input.at(x,yt).b + input.at(x,yb).b) * 0.25f, 1.0f};
                    out = {clamp01(c.r + (c.r - blur.r) * 0.35f),
                           clamp01(c.g + (c.g - blur.g) * 0.35f),
                           clamp01(c.b + (c.b - blur.b) * 0.35f), c.a};
                } else if (preset_ == "mosaic") {
                    const float l = 0.2126f*c.r + 0.7152f*c.g + 0.0722f*c.b;
                    out = {clamp01(std::round((0.75f*c.r + 0.25f*l)*7.0f)/7.0f),
                           clamp01(std::round((0.65f*c.g + 0.20f*l)*6.0f)/6.0f),
                           clamp01(std::round((0.55f*c.b + 0.15f*l)*5.0f)/5.0f), c.a};
                } else {
                    const float l = 0.2126f*c.r + 0.7152f*c.g + 0.0722f*c.b;
                    out = {clamp01(c.r * 1.15f + (1.0f-l)*0.06f),
                           clamp01(c.g * 0.88f + l*0.05f),
                           clamp01(c.b * 1.08f + (0.5f-l)*0.08f), c.a};
                }
                output.at(x, y) = out;
            }
        }
        return output;
    }

private:
    std::string preset_;
    std::string display_;
};

} // namespace

std::unique_ptr<InferenceBackend> make_preview_backend(std::string preset) {
    return std::make_unique<PreviewBackend>(std::move(preset));
}

} // namespace neuralpass
