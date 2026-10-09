#include "neuralpass/depth_pyramid.hpp"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace neuralpass {
namespace {

bool valid_depth(float value) {
    return std::isfinite(value) && value > 0.0f;
}

} // namespace

void DepthPyramid::rebuild(const Image<float> &linear_depth) {
    levels_.clear();
    if (linear_depth.empty()) return;
    DepthPyramidLevel base {
        Image<float>(linear_depth.width(), linear_depth.height(),
                     std::numeric_limits<float>::infinity()),
        Image<float>(linear_depth.width(), linear_depth.height(),
                     -std::numeric_limits<float>::infinity())};
    for (std::size_t index = 0; index < linear_depth.size(); ++index) {
        const auto value = linear_depth.pixels()[index];
        if (!valid_depth(value)) continue;
        base.minimum.pixels()[index] = value;
        base.maximum.pixels()[index] = value;
    }
    levels_.push_back(std::move(base));
    while (levels_.back().minimum.width() > 1 || levels_.back().minimum.height() > 1) {
        const auto &source = levels_.back();
        const auto width = (source.minimum.width() + 1) / 2;
        const auto height = (source.minimum.height() + 1) / 2;
        DepthPyramidLevel next {
            Image<float>(width, height, std::numeric_limits<float>::infinity()),
            Image<float>(width, height, -std::numeric_limits<float>::infinity())};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                for (std::uint32_t oy = 0; oy < 2; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2; ++ox) {
                        const auto sx = x * 2 + ox;
                        const auto sy = y * 2 + oy;
                        if (sx >= source.minimum.width() || sy >= source.minimum.height()) continue;
                        next.minimum.at(x, y) = std::min(next.minimum.at(x, y),
                                                        source.minimum.at(sx, sy));
                        next.maximum.at(x, y) = std::max(next.maximum.at(x, y),
                                                        source.maximum.at(sx, sy));
                    }
                }
            }
        }
        levels_.push_back(std::move(next));
    }
}

const DepthPyramidLevel &DepthPyramid::level(std::size_t index) const {
    if (index >= levels_.size()) throw std::out_of_range("depth pyramid level");
    return levels_[index];
}

bool depth_segment_visible(
    const DepthPyramid &pyramid,
    float start_x, float start_y, float start_depth,
    float end_x, float end_y, float end_depth,
    const DepthRaymarchSettings &settings) {
    if (pyramid.empty() || !std::isfinite(start_x) || !std::isfinite(start_y) ||
        !std::isfinite(end_x) || !std::isfinite(end_y) || !valid_depth(start_depth) ||
        !valid_depth(end_depth)) return false;
    const auto &base = pyramid.level(0).minimum;
    const float dx = end_x - start_x;
    const float dy = end_y - start_y;
    const float extent = std::max(std::abs(dx), std::abs(dy));
    if (extent <= std::numeric_limits<float>::epsilon()) return true;

    float distance = 0.0f;
    while (distance <= extent) {
        const float t = distance / extent;
        const float x = start_x + dx * t;
        const float y = start_y + dy * t;
        const float ray_depth = start_depth + (end_depth - start_depth) * t;
        if (x < 0.0f || y < 0.0f || x >= static_cast<float>(base.width()) ||
            y >= static_cast<float>(base.height())) return false;

        std::size_t candidate_level = pyramid.level_count() - 1;
        float advance = 1.0f;
        for (;;) {
            const auto &minimum = pyramid.level(candidate_level).minimum;
            const auto scale = static_cast<float>(std::uint64_t {1} << candidate_level);
            const auto sx = std::min(static_cast<std::uint32_t>(x / scale), minimum.width() - 1);
            const auto sy = std::min(static_cast<std::uint32_t>(y / scale), minimum.height() - 1);
            const float scene_depth = minimum.at(sx, sy);
            const float tolerance = std::max(0.0f, settings.absolute_tolerance) +
                std::max(0.0f, settings.relative_tolerance) * ray_depth;
            if (valid_depth(scene_depth) && scene_depth + tolerance < ray_depth) {
                if (candidate_level == 0) return false;
                --candidate_level;
                continue;
            }
            advance = scale;
            break;
        }
        if (distance >= extent) break;
        distance = std::min(extent, distance + std::max(1.0f, advance));
    }
    return true;
}

} // namespace neuralpass
