#pragma once

#include "neuralpass/types.hpp"

namespace neuralpass {

struct TemporalSettings {
    float color_threshold = 0.10f;
    float depth_relative_threshold = 0.02f;
    float depth_absolute_threshold = 0.002f;
    std::uint16_t maximum_age = 120;
};

struct HistoryFrame {
    Image<Color> source;
    Image<Color> styled;
    Image<float> depth;
    Image<std::uint8_t> valid;
    Image<std::uint16_t> age;
};

struct TemporalResult {
    HistoryFrame reprojected;
    Image<std::uint8_t> dirty;
    std::size_t accepted = 0;
    std::size_t rejected = 0;
};

[[nodiscard]] TemporalResult reproject_history(
    const Image<Color> &current,
    const Image<float> *current_depth,
    const Image<Motion> *current_to_previous,
    const HistoryFrame &previous,
    const TemporalSettings &settings);

[[nodiscard]] bool is_camera_cut(const Image<Color> &current,
                                 const HistoryFrame &previous,
                                 float rejected_fraction = 0.82f);

} // namespace neuralpass
