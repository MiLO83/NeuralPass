#include "neuralpass/binding_style.hpp"

#include <algorithm>
#include <stdexcept>
#include <utility>
#include <vector>

namespace neuralpass {
namespace {

constexpr std::uint64_t kFnvOffset = 1469598103934665603ull;
constexpr std::uint64_t kFnvPrime = 1099511628211ull;

template <typename T>
void hash_value(std::uint64_t &hash, const T &value) noexcept {
    const auto *bytes = reinterpret_cast<const std::uint8_t *>(&value);
    for (std::size_t index = 0; index < sizeof(T); ++index) {
        hash ^= bytes[index];
        hash *= kFnvPrime;
    }
}

void require_equal_dimensions(const Image<Color> &styled,
                              const Image<std::uint8_t> &valid,
                              const Image<Color> &live,
                              const Image<std::uint8_t> &strength) {
    if (styled.width() != valid.width() || styled.height() != valid.height() ||
        styled.width() != live.width() || styled.height() != live.height() ||
        styled.width() != strength.width() || styled.height() != strength.height())
        throw std::invalid_argument("binding style images must have equal dimensions");
}

} // namespace

std::uint64_t binding_style_control_identity(
    const BindingStyleControls &controls) noexcept {
    std::vector<std::pair<std::uint64_t, std::uint8_t>> ordered;
    ordered.reserve(controls.size());
    for (const auto &[binding, strength] : controls)
        if (binding != 0 && strength < 100)
            ordered.emplace_back(binding, strength);
    std::sort(ordered.begin(), ordered.end());
    std::uint64_t hash = kFnvOffset;
    constexpr std::uint64_t version = 1;
    hash_value(hash, version);
    for (const auto &[binding, strength] : ordered) {
        hash_value(hash, binding);
        hash_value(hash, strength);
    }
    return hash == 0 ? 1 : hash;
}

BindingStylePlan plan_binding_styles(
    std::uint32_t width, std::uint32_t height,
    std::span<const SurfaceCorrespondence> correspondence,
    const BindingStyleControls &controls) {
    BindingStylePlan result;
    result.strength_percent = Image<std::uint8_t>(width, height, 100);
    result.active_correspondence.reserve(correspondence.size());
    for (const auto &sample : correspondence) {
        const auto found = controls.find(sample.material_id);
        const auto strength = found == controls.end()
            ? std::uint8_t {100} : std::min<std::uint8_t>(found->second, 100);
        if (sample.screen_x < width && sample.screen_y < height) {
            auto &pixel = result.strength_percent.at(sample.screen_x, sample.screen_y);
            pixel = std::min(pixel, strength);
        }
        if (strength != 0) result.active_correspondence.push_back(sample);
    }
    return result;
}

void invalidate_bypassed_bindings(
    Image<std::uint8_t> &valid,
    const Image<std::uint8_t> &strength_percent) {
    if (valid.width() != strength_percent.width() ||
        valid.height() != strength_percent.height())
        throw std::invalid_argument("binding style images must have equal dimensions");
    for (std::size_t index = 0; index < valid.size(); ++index)
        if (strength_percent.pixels()[index] == 0) valid.pixels()[index] = 0;
}

void apply_binding_styles(
    Image<Color> &styled,
    Image<std::uint8_t> &valid,
    const Image<Color> &live,
    const Image<std::uint8_t> &strength_percent,
    Rect region) {
    require_equal_dimensions(styled, valid, live, strength_percent);
    if (region.x > styled.width() || region.y > styled.height() ||
        region.width > styled.width() - region.x ||
        region.height > styled.height() - region.y)
        throw std::out_of_range("binding style region exceeds image bounds");
    for (std::uint32_t y = region.y; y < region.y + region.height; ++y) {
        for (std::uint32_t x = region.x; x < region.x + region.width; ++x) {
            const auto strength = strength_percent.at(x, y);
            if (strength >= 100) continue;
            if (strength == 0) {
                styled.at(x, y) = live.at(x, y);
                valid.at(x, y) = 0;
                continue;
            }
            if (valid.at(x, y) == 0) continue;
            const float amount = static_cast<float>(strength) / 100.0f;
            const auto source = live.at(x, y);
            const auto generated = styled.at(x, y);
            styled.at(x, y) = {
                source.r + (generated.r - source.r) * amount,
                source.g + (generated.g - source.g) * amount,
                source.b + (generated.b - source.b) * amount,
                source.a};
        }
    }
}

} // namespace neuralpass
