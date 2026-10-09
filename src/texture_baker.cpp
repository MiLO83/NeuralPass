#include "neuralpass/texture_baker.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <limits>
#include <unordered_set>

namespace neuralpass {
namespace {

float coordinate(float value, bool wrap) {
    if (wrap) return value - std::floor(value);
    return std::clamp(value, 0.0f, 1.0f);
}

std::uint32_t texel(int value, std::uint32_t extent, bool wrap) {
    if (wrap) {
        const int size = static_cast<int>(extent);
        return static_cast<std::uint32_t>((value % size + size) % size);
    }
    return static_cast<std::uint32_t>(std::clamp(value, 0, static_cast<int>(extent) - 1));
}

Color mix(const Color &a, const Color &b, float t) {
    return {a.r + (b.r-a.r)*t, a.g + (b.g-a.g)*t,
            a.b + (b.b-a.b)*t, a.a + (b.a-a.a)*t};
}

bool valid_observation(const SurfaceCorrespondence &sample,
                       const TextureBakeSettings &settings) {
    if (sample.material_id == 0 || !std::isfinite(sample.u) || !std::isfinite(sample.v) ||
        !std::isfinite(sample.confidence) || sample.confidence <= 0.0f ||
        !std::isfinite(settings.observation_weight) || settings.observation_weight <= 0.0f)
        return false;
    const bool has_framebuffer_depth = std::isfinite(sample.framebuffer_depth);
    const bool has_hit_depth = std::isfinite(sample.hit_depth);
    if (has_framebuffer_depth != has_hit_depth) return false;
    if (has_framebuffer_depth) {
        const float tolerance = std::max(0.0f, settings.depth_absolute_threshold) +
            std::max(0.0f, settings.depth_relative_threshold) *
                std::max(std::abs(sample.framebuffer_depth), std::abs(sample.hit_depth));
        if (std::abs(sample.framebuffer_depth - sample.hit_depth) > tolerance) return false;
    }
    return true;
}

} // namespace

MaterialTextureAtlas::MaterialTextureAtlas(std::uint64_t material_id,
                                           std::uint32_t width, std::uint32_t height)
    : material_id_(material_id), color_(width, height, kUnpaintedColor),
      history_weight_(width, height, 0.0f), coverage_(width, height, kUnseen) {}

void MaterialTextureAtlas::splat(
    const Image<Color> &restyled_screen,
    std::span<const SurfaceCorrespondence> correspondence,
    const TextureBakeSettings &settings) {
    if (color_.empty()) return;
    for (const auto &sample : correspondence) {
        if (sample.screen_x < restyled_screen.width() && sample.screen_y < restyled_screen.height())
            observe(restyled_screen.at(sample.screen_x, sample.screen_y), sample, settings);
    }
}

bool MaterialTextureAtlas::observe(const Color &observed,
                                   const SurfaceCorrespondence &sample,
                                   const TextureBakeSettings &settings) {
    if (color_.empty() || sample.material_id != material_id_ ||
        !valid_observation(sample, settings))
        return false;
    const float u = coordinate(sample.u, settings.wrap_u);
    const float v = coordinate(sample.v, settings.wrap_v);
    const float fx = u * static_cast<float>(width() - 1);
    const float fy = v * static_cast<float>(height() - 1);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const std::array<float, 4> bilinear {
        (1.0f-tx)*(1.0f-ty), tx*(1.0f-ty), (1.0f-tx)*ty, tx*ty
    };
    const std::array<int, 4> xs {x0, x0+1, x0, x0+1};
    const std::array<int, 4> ys {y0, y0, y0+1, y0+1};
    bool accepted = false;
    for (std::size_t corner = 0; corner < 4; ++corner) {
        const auto x = texel(xs[corner], width(), settings.wrap_u);
        const auto y = texel(ys[corner], height(), settings.wrap_v);
        const float vote = bilinear[corner] * sample.confidence * settings.observation_weight;
        if (!std::isfinite(vote) || vote <= std::numeric_limits<float>::epsilon()) continue;
        if (settings.fill_only_unobserved && coverage_.at(x, y) == kObserved) continue;
        if (settings.fill_only_unobserved) {
            color_.at(x, y) = observed;
            history_weight_.at(x, y) = vote;
            coverage_.at(x, y) = kObserved;
            accepted = true;
            continue;
        }
        const float old_weight = history_weight_.at(x, y);
        const float retained = std::min(old_weight, std::max(0.0f, settings.maximum_history_weight));
        const float total = retained + vote;
        color_.at(x, y) = mix(color_.at(x, y), observed, vote / total);
        history_weight_.at(x, y) = std::min(total, std::max(vote, settings.maximum_history_weight));
        coverage_.at(x, y) = kObserved;
        accepted = true;
    }
    return accepted;
}

void MaterialTextureAtlas::inpaint_unseen(std::uint32_t maximum_passes) {
    if (color_.empty()) return;
    if (maximum_passes == 0) maximum_passes = width() + height();
    Image<Color> next_color = color_;
    Image<std::uint8_t> next_coverage = coverage_;
    for (std::uint32_t pass = 0; pass < maximum_passes; ++pass) {
        std::size_t filled = 0;
        next_color = color_;
        next_coverage = coverage_;
        for (std::uint32_t y = 0; y < height(); ++y) {
            for (std::uint32_t x = 0; x < width(); ++x) {
                if (coverage_.at(x, y) != kUnseen) continue;
                Color sum {};
                float count = 0.0f;
                for (int oy = -1; oy <= 1; ++oy) {
                    for (int ox = -1; ox <= 1; ++ox) {
                        if (ox == 0 && oy == 0) continue;
                        const auto nx = texel(static_cast<int>(x) + ox, width(), true);
                        const auto ny = texel(static_cast<int>(y) + oy, height(), true);
                        if (coverage_.at(nx, ny) == kUnseen) continue;
                        const auto neighbor = color_.at(nx, ny);
                        sum.r += neighbor.r; sum.g += neighbor.g; sum.b += neighbor.b; sum.a += neighbor.a;
                        count += 1.0f;
                    }
                }
                if (count > 0.0f) {
                    next_color.at(x, y) = {sum.r/count, sum.g/count, sum.b/count, sum.a/count};
                    next_coverage.at(x, y) = kInpainted;
                    ++filled;
                }
            }
        }
        color_ = std::move(next_color);
        coverage_ = std::move(next_coverage);
        if (filled == 0) break;
    }
}

Color MaterialTextureAtlas::sample(float u, float v, bool wrap_u, bool wrap_v) const {
    if (color_.empty() || !std::isfinite(u) || !std::isfinite(v)) return {};
    u = coordinate(u, wrap_u);
    v = coordinate(v, wrap_v);
    const float fx = u * static_cast<float>(width() - 1);
    const float fy = v * static_cast<float>(height() - 1);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const std::array<float, 4> weights {
        (1.0f-tx)*(1.0f-ty), tx*(1.0f-ty), (1.0f-tx)*ty, tx*ty
    };
    const std::array<int, 4> xs {x0, x0+1, x0, x0+1};
    const std::array<int, 4> ys {y0, y0, y0+1, y0+1};
    Color result {};
    result.a = 0.0f;
    float total = 0.0f;
    for (std::size_t corner = 0; corner < weights.size(); ++corner) {
        const auto x = texel(xs[corner], width(), wrap_u);
        const auto y = texel(ys[corner], height(), wrap_v);
        if (coverage_.at(x, y) == kUnseen || weights[corner] <= 0.0f) continue;
        const auto value = color_.at(x, y);
        result.r += value.r * weights[corner];
        result.g += value.g * weights[corner];
        result.b += value.b * weights[corner];
        result.a += value.a * weights[corner];
        total += weights[corner];
    }
    if (total <= std::numeric_limits<float>::epsilon()) return {};
    result.r /= total; result.g /= total; result.b /= total; result.a /= total;
    return result;
}

bool MaterialTextureAtlas::has_coverage(float u, float v, bool wrap_u, bool wrap_v) const {
    if (coverage_.empty() || !std::isfinite(u) || !std::isfinite(v)) return false;
    u = coordinate(u, wrap_u);
    v = coordinate(v, wrap_v);
    const float fx = u * static_cast<float>(width() - 1);
    const float fy = v * static_cast<float>(height() - 1);
    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const std::array<float, 4> weights {
        (1.0f-tx)*(1.0f-ty), tx*(1.0f-ty), (1.0f-tx)*ty, tx*ty
    };
    const std::array<int, 4> xs {x0, x0+1, x0, x0+1};
    const std::array<int, 4> ys {y0, y0, y0+1, y0+1};
    for (std::size_t corner = 0; corner < weights.size(); ++corner) {
        const auto x = texel(xs[corner], width(), wrap_u);
        const auto y = texel(ys[corner], height(), wrap_v);
        if (weights[corner] > 0.0f && coverage_.at(x, y) != kUnseen) return true;
    }
    return false;
}

Image<Color> reconstruct_from_atlas(
    const Image<Color> &fallback, const MaterialTextureAtlas &atlas,
    std::span<const SurfaceCorrespondence> correspondence) {
    Image<Color> result = fallback;
    for (const auto &sample : correspondence) {
        if (sample.material_id != atlas.material_id() || sample.confidence <= 0.0f ||
            sample.screen_x >= result.width() || sample.screen_y >= result.height() ||
            !atlas.has_coverage(sample.u, sample.v))
            continue;
        result.at(sample.screen_x, sample.screen_y) = atlas.sample(sample.u, sample.v);
    }
    return result;
}

MaterialTextureBaker::MaterialTextureBaker(MaterialTextureBakerSettings settings)
    : settings_(settings) {
    if (settings_.atlas_width == 0 || settings_.atlas_height == 0)
        throw std::invalid_argument("material atlas dimensions must be nonzero");
}

MaterialTextureBakeStats MaterialTextureBaker::update(
    const Image<Color> &restyled_screen,
    std::span<const SurfaceCorrespondence> correspondence) {
    MaterialTextureBakeStats stats;
    stats.submitted = correspondence.size();
    std::unordered_set<std::uint64_t> touched;
    for (const auto &sample : correspondence) {
        if (sample.screen_x >= restyled_screen.width() || sample.screen_y >= restyled_screen.height())
            continue;
        auto [entry, inserted] = atlases_.try_emplace(
            sample.material_id, sample.material_id, settings_.atlas_width, settings_.atlas_height);
        if (entry->second.observe(restyled_screen.at(sample.screen_x, sample.screen_y),
                                  sample, settings_.texture)) {
            ++stats.accepted;
            touched.insert(sample.material_id);
        } else if (inserted) {
            // Invalid observations must not create empty, unbounded material entries.
            atlases_.erase(entry);
        }
    }
    stats.materials_touched = touched.size();
    return stats;
}

MaterialTextureBakePlan MaterialTextureBaker::plan(
    const Image<Color> &live_frame,
    std::span<const SurfaceCorrespondence> correspondence) const {
    MaterialTextureBakePlan result;
    result.composite = live_frame;
    result.reveal_mask = Image<std::uint8_t>(live_frame.width(), live_frame.height(), 0);
    result.epoch = epoch_;
    for (const auto &sample : correspondence) {
        if (sample.screen_x >= live_frame.width() || sample.screen_y >= live_frame.height() ||
            !valid_observation(sample, settings_.texture))
            continue;
        const auto found = atlases_.find(sample.material_id);
        if (found != atlases_.end() && found->second.has_coverage(
                sample.u, sample.v, settings_.texture.wrap_u, settings_.texture.wrap_v)) {
            result.composite.at(sample.screen_x, sample.screen_y) = found->second.sample(
                sample.u, sample.v, settings_.texture.wrap_u, settings_.texture.wrap_v);
            ++result.known_pixels;
        } else if (result.reveal_mask.at(sample.screen_x, sample.screen_y) == 0) {
            result.reveal_mask.at(sample.screen_x, sample.screen_y) = 255;
            ++result.revealed_pixels;
        }
    }
    return result;
}

MaterialTextureBakeStats MaterialTextureBaker::commit(
    const MaterialTextureBakePlan &plan,
    const Image<Color> &inpainted_frame,
    std::span<const SurfaceCorrespondence> correspondence) {
    MaterialTextureBakeStats stats;
    stats.submitted = correspondence.size();
    if (plan.epoch != epoch_ || plan.reveal_mask.width() != inpainted_frame.width() ||
        plan.reveal_mask.height() != inpainted_frame.height()) {
        stats.stale = true;
        return stats;
    }
    std::unordered_set<std::uint64_t> touched;
    for (const auto &sample : correspondence) {
        if (sample.screen_x >= inpainted_frame.width() || sample.screen_y >= inpainted_frame.height() ||
            plan.reveal_mask.at(sample.screen_x, sample.screen_y) == 0 ||
            !valid_observation(sample, settings_.texture))
            continue;
        auto [entry, inserted] = atlases_.try_emplace(
            sample.material_id, sample.material_id, settings_.atlas_width, settings_.atlas_height);
        if (entry->second.observe(inpainted_frame.at(sample.screen_x, sample.screen_y),
                                  sample, settings_.texture)) {
            ++stats.accepted;
            touched.insert(sample.material_id);
        } else if (inserted) {
            atlases_.erase(entry);
        }
    }
    stats.materials_touched = touched.size();
    return stats;
}

Image<Color> MaterialTextureBaker::reconstruct(
    const Image<Color> &fallback,
    std::span<const SurfaceCorrespondence> correspondence) const {
    Image<Color> result = fallback;
    for (const auto &sample : correspondence) {
        if (sample.screen_x >= result.width() || sample.screen_y >= result.height() ||
            !std::isfinite(sample.confidence) || sample.confidence <= 0.0f)
            continue;
        const auto found = atlases_.find(sample.material_id);
        if (found == atlases_.end() || !found->second.has_coverage(sample.u, sample.v,
                settings_.texture.wrap_u, settings_.texture.wrap_v))
            continue;
        result.at(sample.screen_x, sample.screen_y) = found->second.sample(
            sample.u, sample.v, settings_.texture.wrap_u, settings_.texture.wrap_v);
    }
    return result;
}

void MaterialTextureBaker::inpaint(std::uint32_t maximum_passes) {
    for (auto &[material_id, atlas] : atlases_) {
        (void)material_id;
        atlas.inpaint_unseen(maximum_passes);
    }
}

MaterialTextureAtlas *MaterialTextureBaker::find(std::uint64_t material_id) noexcept {
    const auto found = atlases_.find(material_id);
    return found == atlases_.end() ? nullptr : &found->second;
}

const MaterialTextureAtlas *MaterialTextureBaker::find(std::uint64_t material_id) const noexcept {
    const auto found = atlases_.find(material_id);
    return found == atlases_.end() ? nullptr : &found->second;
}

} // namespace neuralpass
