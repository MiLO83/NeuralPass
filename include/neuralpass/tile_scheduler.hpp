#pragma once

#include "neuralpass/types.hpp"

#include <span>

namespace neuralpass {

struct TileSettings {
    std::uint32_t tile_size = 256;
    std::uint32_t halo = 32;
    std::uint32_t dilation_radius = 4;
    std::uint32_t tile_budget = 2;
    std::uint16_t refresh_age = 120;
};

struct TileJob {
    Rect core;
    Rect padded;
    float dirty_fraction = 0.0f;
    float urgent_fraction = 0.0f;
    float score = 0.0f;
    bool refresh_only = false;
};

[[nodiscard]] Image<std::uint8_t> dilate_mask(const Image<std::uint8_t> &mask,
                                               std::uint32_t radius);

[[nodiscard]] std::vector<TileJob> schedule_tiles(
    const Image<std::uint8_t> &dirty,
    const Image<std::uint16_t> &age,
    const TileSettings &settings,
    const Image<std::uint8_t> *urgent = nullptr);

} // namespace neuralpass
