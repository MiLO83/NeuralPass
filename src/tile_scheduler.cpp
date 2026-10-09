#include "neuralpass/tile_scheduler.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace neuralpass {

Image<std::uint8_t> dilate_mask(const Image<std::uint8_t> &mask, std::uint32_t radius) {
    if (radius == 0)
        return mask;
    Image<std::uint8_t> horizontal(mask.width(), mask.height(), 0);
    Image<std::uint8_t> output(mask.width(), mask.height(), 0);
    for (std::uint32_t y = 0; y < mask.height(); ++y) {
        for (std::uint32_t x = 0; x < mask.width(); ++x) {
            const auto lo = x > radius ? x - radius : 0;
            const auto hi = std::min(mask.width() - 1, x + radius);
            for (std::uint32_t sx = lo; sx <= hi; ++sx)
                if (mask.at(sx, y)) { horizontal.at(x, y) = 1; break; }
        }
    }
    for (std::uint32_t y = 0; y < mask.height(); ++y) {
        const auto lo = y > radius ? y - radius : 0;
        const auto hi = std::min(mask.height() - 1, y + radius);
        for (std::uint32_t x = 0; x < mask.width(); ++x)
            for (std::uint32_t sy = lo; sy <= hi; ++sy)
                if (horizontal.at(x, sy)) { output.at(x, y) = 1; break; }
    }
    return output;
}

std::vector<TileJob> schedule_tiles(const Image<std::uint8_t> &input_dirty,
                                    const Image<std::uint16_t> &age,
                                    const TileSettings &settings) {
    if (settings.tile_size == 0)
        throw std::invalid_argument("tile_size must be non-zero");
    if (age.width() != input_dirty.width() || age.height() != input_dirty.height())
        throw std::invalid_argument("age and dirty images must have equal dimensions");
    if (input_dirty.empty() || settings.tile_budget == 0)
        return {};

    const auto dirty = dilate_mask(input_dirty, settings.dilation_radius);
    std::vector<TileJob> jobs;
    for (std::uint32_t y = 0; y < dirty.height(); y += settings.tile_size) {
        for (std::uint32_t x = 0; x < dirty.width(); x += settings.tile_size) {
            const auto w = std::min(settings.tile_size, dirty.width() - x);
            const auto h = std::min(settings.tile_size, dirty.height() - y);
            std::uint64_t dirty_count = 0;
            std::uint16_t oldest = 0;
            for (std::uint32_t py = y; py < y + h; ++py)
                for (std::uint32_t px = x; px < x + w; ++px) {
                    dirty_count += dirty.at(px, py) != 0;
                    oldest = std::max(oldest, age.at(px, py));
                }
            const bool refresh = dirty_count == 0 && oldest >= settings.refresh_age;
            if (dirty_count == 0 && !refresh)
                continue;

            const float fraction = static_cast<float>(dirty_count) / static_cast<float>(w * h);
            const float cx = static_cast<float>(x) + static_cast<float>(w) * 0.5f;
            const float cy = static_cast<float>(y) + static_cast<float>(h) * 0.5f;
            const float nx = (cx / static_cast<float>(dirty.width())) - 0.5f;
            const float ny = (cy / static_cast<float>(dirty.height())) - 0.5f;
            const float center = 1.0f - std::min(1.0f, std::sqrt(nx * nx + ny * ny) * 1.41421356f);
            const float age_score = settings.refresh_age == 0 ? 1.0f :
                std::min(2.0f, static_cast<float>(oldest) / static_cast<float>(settings.refresh_age));
            const auto left = x > settings.halo ? x - settings.halo : 0;
            const auto top = y > settings.halo ? y - settings.halo : 0;
            const auto right = std::min(dirty.width(), x + w + settings.halo);
            const auto bottom = std::min(dirty.height(), y + h + settings.halo);
            jobs.push_back({
                {x, y, w, h},
                {left, top, right - left, bottom - top},
                fraction,
                fraction * 10000.0f + center * 100.0f + age_score * 10.0f,
                refresh});
        }
    }
    std::stable_sort(jobs.begin(), jobs.end(), [](const TileJob &a, const TileJob &b) {
        if (a.score != b.score) return a.score > b.score;
        if (a.core.y != b.core.y) return a.core.y < b.core.y;
        return a.core.x < b.core.x;
    });
    if (jobs.size() > settings.tile_budget)
        jobs.resize(settings.tile_budget);
    return jobs;
}

} // namespace neuralpass
