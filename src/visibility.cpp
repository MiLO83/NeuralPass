#include "neuralpass/visibility.hpp"

#include <algorithm>
#include <cmath>
#include <unordered_set>

namespace neuralpass {
namespace {

bool supported(const SurfaceCapturePixel &pixel, float minimum_confidence) {
    return pixel.material_id != 0 && std::isfinite(pixel.u) && std::isfinite(pixel.v) &&
        std::isfinite(pixel.confidence) && pixel.confidence >= minimum_confidence;
}

float wrapped_distance(float left, float right) {
    const float difference = std::abs(left - right);
    return std::min({difference, std::abs(difference - 1.0f),
                     std::abs(difference + 1.0f)});
}

} // namespace

VisibilityClassification classify_visibility(
    const SurfaceCaptureFrame &current,
    const SurfaceCaptureFrame *previous,
    const Image<Motion> *motion,
    const VisibilitySettings &settings) {
    VisibilityClassification result;
    result.pixels = Image<VisibilityClass>(current.width(), current.height(),
                                           VisibilityClass::unsupported);
    const bool have_previous = previous != nullptr && previous->width() == current.width() &&
        previous->height() == current.height();
    const bool have_motion = motion != nullptr && motion->width() == current.width() &&
        motion->height() == current.height();
    std::unordered_set<std::uint64_t> previous_bindings;
    if (have_previous)
        for (const auto &pixel : previous->pixels().pixels())
            if (supported(pixel, settings.minimum_confidence))
                previous_bindings.insert(pixel.material_id);

    auto assign = [&](std::uint32_t x, std::uint32_t y, VisibilityClass value) {
        result.pixels.at(x, y) = value;
        ++result.counts[static_cast<std::size_t>(value)];
    };
    for (std::uint32_t y = 0; y < current.height(); ++y) {
        for (std::uint32_t x = 0; x < current.width(); ++x) {
            const auto &now = current.pixels().at(x, y);
            if (!supported(now, settings.minimum_confidence)) {
                assign(x, y, VisibilityClass::unsupported);
                continue;
            }
            if (!have_previous) {
                assign(x, y, VisibilityClass::first_observation);
                continue;
            }
            const auto movement = have_motion ? motion->at(x, y) : Motion {};
            const int px = static_cast<int>(std::lround(static_cast<float>(x) + movement.x));
            const int py = static_cast<int>(std::lround(static_cast<float>(y) + movement.y));
            if (px < 0 || py < 0 || px >= static_cast<int>(current.width()) ||
                py >= static_cast<int>(current.height())) {
                assign(x, y, VisibilityClass::offscreen_entry);
                continue;
            }
            const auto &before = previous->pixels().at(
                static_cast<std::uint32_t>(px), static_cast<std::uint32_t>(py));
            if (supported(before, settings.minimum_confidence) &&
                before.material_id == now.material_id &&
                wrapped_distance(before.u, now.u) <= settings.uv_tolerance &&
                wrapped_distance(before.v, now.v) <= settings.uv_tolerance) {
                assign(x, y, VisibilityClass::known_visible);
                continue;
            }
            if (supported(before, settings.minimum_confidence) &&
                std::isfinite(before.framebuffer_depth) &&
                std::isfinite(now.framebuffer_depth)) {
                const float tolerance = std::max(0.0f, settings.depth_absolute_threshold) +
                    std::max(0.0f, settings.depth_relative_threshold) *
                        std::max(std::abs(before.framebuffer_depth),
                                 std::abs(now.framebuffer_depth));
                if (before.framebuffer_depth + tolerance < now.framebuffer_depth) {
                    assign(x, y, VisibilityClass::disoccluded);
                    continue;
                }
            }
            if (previous_bindings.contains(now.material_id))
                assign(x, y, VisibilityClass::newly_front_facing);
            else
                assign(x, y, VisibilityClass::first_observation);
        }
    }
    return result;
}

Image<TextureRevealClass> texture_reveal_classes(
    const VisibilityClassification &visibility) {
    Image<TextureRevealClass> result(visibility.pixels.width(),
        visibility.pixels.height(), TextureRevealClass::unknown);
    for (std::size_t index = 0; index < visibility.pixels.size(); ++index) {
        switch (visibility.pixels.pixels()[index]) {
        case VisibilityClass::first_observation:
            result.pixels()[index] = TextureRevealClass::first_observation;
            break;
        case VisibilityClass::disoccluded:
        case VisibilityClass::newly_front_facing:
        case VisibilityClass::offscreen_entry:
            result.pixels()[index] = TextureRevealClass::newly_visible;
            break;
        case VisibilityClass::unsupported:
        case VisibilityClass::known_visible:
            break;
        }
    }
    return result;
}

} // namespace neuralpass
