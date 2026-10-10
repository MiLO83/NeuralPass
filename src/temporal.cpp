#include "neuralpass/temporal.hpp"

#include <cmath>

namespace neuralpass {
namespace {

float color_distance(const Color &a, const Color &b) {
    // Luma-weighted absolute error is stable in the presence of mild chroma noise.
    return 0.2126f * std::abs(a.r - b.r) +
           0.7152f * std::abs(a.g - b.g) +
           0.0722f * std::abs(a.b - b.b);
}

bool same_size(const Image<Color> &a, const Image<Color> &b) {
    return a.width() == b.width() && a.height() == b.height();
}

} // namespace

TemporalResult reproject_history(const Image<Color> &current,
                                 const Image<float> *current_depth,
                                 const Image<Motion> *motion,
                                 const HistoryFrame &previous,
                                 const TemporalSettings &settings) {
    const auto width = current.width();
    const auto height = current.height();
    TemporalResult result;
    result.reprojected.source = Image<Color>(width, height);
    result.reprojected.styled = Image<Color>(width, height);
    result.reprojected.depth = Image<float>(width, height, 1.0f);
    result.reprojected.valid = Image<std::uint8_t>(width, height, 0);
    result.reprojected.age = Image<std::uint16_t>(width, height, 0);
    result.dirty = Image<std::uint8_t>(width, height, 1);

    if (!same_size(current, previous.source) ||
        !same_size(current, previous.styled) ||
        previous.valid.width() != width || previous.valid.height() != height)
        return result;

    const bool have_depth = current_depth && !previous.depth.empty() &&
        current_depth->width() == width && current_depth->height() == height;
    const bool have_motion = motion && motion->width() == width && motion->height() == height;

    for (std::uint32_t y = 0; y < height; ++y) {
        for (std::uint32_t x = 0; x < width; ++x) {
            result.reprojected.source.at(x, y) = current.at(x, y);
            result.reprojected.depth.at(x, y) = have_depth ? current_depth->at(x, y) : 1.0f;
            const Motion mv = have_motion ? motion->at(x, y) : Motion{};
            const auto px = static_cast<int>(std::lround(static_cast<float>(x) + mv.x));
            const auto py = static_cast<int>(std::lround(static_cast<float>(y) + mv.y));
            bool valid = px >= 0 && py >= 0 && px < static_cast<int>(width) && py < static_cast<int>(height);
            if (valid)
                valid = previous.valid.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py)) != 0;

            if (valid && have_depth) {
                const float now = current_depth->at(x, y);
                const float then = previous.depth.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                const float tolerance = settings.depth_absolute_threshold +
                    settings.depth_relative_threshold * std::max(std::abs(now), std::abs(then));
                valid = std::isfinite(now) && std::isfinite(then) && std::abs(now - then) <= tolerance;
            }
            if (valid) {
                valid = color_distance(current.at(x, y),
                    previous.source.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py))) <=
                    settings.color_threshold;
            }
            if (valid) {
                const auto previous_age = previous.age.empty() ? 0 :
                    previous.age.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                valid = previous_age < settings.maximum_age;
                if (valid) {
                    result.reprojected.styled.at(x, y) =
                        previous.styled.at(static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
                    result.reprojected.valid.at(x, y) = 1;
                    result.reprojected.age.at(x, y) = static_cast<std::uint16_t>(previous_age + 1);
                    result.dirty.at(x, y) = 0;
                    ++result.accepted;
                }
            }
            if (!valid)
                ++result.rejected;
        }
    }
    return result;
}

bool is_camera_cut(const Image<Color> &current, const HistoryFrame &previous,
                   float rejected_fraction) {
    if (!same_size(current, previous.source) || current.empty())
        return true;
    std::size_t rejected = 0;
    for (std::size_t i = 0; i < current.size(); ++i)
        rejected += color_distance(current.pixels()[i], previous.source.pixels()[i]) > 0.18f;
    return static_cast<float>(rejected) / static_cast<float>(current.size()) >= rejected_fraction;
}

bool should_reset_screen_history(const Image<Color> &current,
                                 const HistoryFrame &previous,
                                 bool visual_cut,
                                 bool previous_visual_cut,
                                 bool sticky_history) noexcept {
    return !same_size(current, previous.source) ||
        (visual_cut && (!sticky_history || !previous_visual_cut));
}

} // namespace neuralpass
