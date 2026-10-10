#pragma once

#include "neuralpass/texture_baker.hpp"

#include <cstdint>
#include <span>
#include <unordered_map>

namespace neuralpass {

using BindingStyleControls = std::unordered_map<std::uint64_t, std::uint8_t>;

struct BindingStylePlan {
    std::vector<SurfaceCorrespondence> active_correspondence;
    // Percentage of generated appearance admitted at each known material pixel.
    // Pixels without an explicit control remain at 100.
    Image<std::uint8_t> strength_percent;
};

[[nodiscard]] std::uint64_t binding_style_control_identity(
    const BindingStyleControls &controls) noexcept;

[[nodiscard]] BindingStylePlan plan_binding_styles(
    std::uint32_t width, std::uint32_t height,
    std::span<const SurfaceCorrespondence> correspondence,
    const BindingStyleControls &controls);

// Clears validity for explicitly bypassed material pixels without modifying
// partially styled history, which may already contain a correctly blended atlas.
void invalidate_bypassed_bindings(
    Image<std::uint8_t> &valid,
    const Image<std::uint8_t> &strength_percent);

// Applies a control exactly once to newly generated pixels in the supplied region.
// A zero-strength material becomes invalid so the compositor preserves the game.
void apply_binding_styles(
    Image<Color> &styled,
    Image<std::uint8_t> &valid,
    const Image<Color> &live,
    const Image<std::uint8_t> &strength_percent,
    Rect region);

} // namespace neuralpass
