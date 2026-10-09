#pragma once

#include "neuralpass/types.hpp"

#include <cstdint>
#include <vector>

namespace neuralpass {

struct DepthPyramidLevel {
    Image<float> minimum;
    Image<float> maximum;
};

// Conservative min/max hierarchy over positive linear view-space depth.
// Invalid samples do not become occluders; a cell is invalid only when none of
// its children contains usable depth.
class DepthPyramid {
public:
    DepthPyramid() = default;
    explicit DepthPyramid(const Image<float> &linear_depth) { rebuild(linear_depth); }

    void rebuild(const Image<float> &linear_depth);
    [[nodiscard]] bool empty() const noexcept { return levels_.empty(); }
    [[nodiscard]] std::size_t level_count() const noexcept { return levels_.size(); }
    [[nodiscard]] const DepthPyramidLevel &level(std::size_t index) const;

private:
    std::vector<DepthPyramidLevel> levels_;
};

struct DepthRaymarchSettings {
    float absolute_tolerance = 0.002f;
    float relative_tolerance = 0.01f;
};

// Tests a projected segment against the depth hierarchy. Coordinates are in
// level-zero pixels and depths are positive linear view-space values. Returns
// false on invalid endpoints or when any conservative leaf test is occluded.
[[nodiscard]] bool depth_segment_visible(
    const DepthPyramid &pyramid,
    float start_x, float start_y, float start_depth,
    float end_x, float end_y, float end_depth,
    const DepthRaymarchSettings &settings = {});

} // namespace neuralpass
