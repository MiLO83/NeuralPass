#pragma once

#include "neuralpass/surface_capture.hpp"
#include "neuralpass/types.hpp"

#include <array>
#include <cstddef>

namespace neuralpass {

enum class VisibilityClass : std::uint8_t {
    unsupported,
    known_visible,
    disoccluded,
    newly_front_facing,
    offscreen_entry,
    first_observation,
};

struct VisibilitySettings {
    float uv_tolerance = 0.02f;
    float depth_absolute_threshold = 0.002f;
    float depth_relative_threshold = 0.02f;
    float minimum_confidence = 0.01f;
};

struct VisibilityClassification {
    Image<VisibilityClass> pixels;
    std::array<std::size_t, 6> counts {};

    [[nodiscard]] std::size_t count(VisibilityClass value) const noexcept {
        return counts[static_cast<std::size_t>(value)];
    }
};

// Motion maps a current pixel to its previous-frame location. Without motion,
// identity reprojection is used. Camera/scene cuts should pass no previous
// frame so every supported sample becomes a direct first observation.
[[nodiscard]] VisibilityClassification classify_visibility(
    const SurfaceCaptureFrame &current,
    const SurfaceCaptureFrame *previous,
    const Image<Motion> *current_to_previous,
    const VisibilitySettings &settings = {});

// Converts screen-space causes into the two disjoint texture-generation lanes.
// Keeping this mapping in the shared core prevents live adapters and acceptance
// tests from silently disagreeing about what may enter persistent coverage.
[[nodiscard]] Image<TextureRevealClass> texture_reveal_classes(
    const VisibilityClassification &visibility);

} // namespace neuralpass
