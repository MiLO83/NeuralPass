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
                                   const TextureBakeSettings &settings,
                                   TextureSampleKind kind) {
    if (color_.empty() || sample.material_id != material_id_ ||
        !valid_observation(sample, settings))
        return false;
    const float u = coordinate(sample.u, settings.wrap_u);
    const float v = coordinate(sample.v, settings.wrap_v);
    const float fx = u * static_cast<float>(width() - 1);
    const float fy = v * static_cast<float>(height() - 1);
    bool accepted = false;
    auto apply_vote = [&](int sample_x, int sample_y, float footprint_weight) {
        const auto x = texel(sample_x, width(), settings.wrap_u);
        const auto y = texel(sample_y, height(), settings.wrap_v);
        const float vote = footprint_weight * sample.confidence * settings.observation_weight;
        if (!std::isfinite(vote) || vote <= std::numeric_limits<float>::epsilon()) return;
        const auto existing = coverage_.at(x, y);
        if (kind == TextureSampleKind::generated_inpaint && existing != kUnseen) return;
        if (settings.fill_only_unobserved && existing == kObserved) return;
        if (settings.fill_only_unobserved) {
            color_.at(x, y) = observed;
            history_weight_.at(x, y) = vote;
            coverage_.at(x, y) = kind == TextureSampleKind::direct_observation
                ? kObserved : kInpainted;
            accepted = true;
            return;
        }
        const float old_weight = history_weight_.at(x, y);
        const float retained = std::min(old_weight, std::max(0.0f, settings.maximum_history_weight));
        const float total = retained + vote;
        color_.at(x, y) = mix(color_.at(x, y), observed, vote / total);
        history_weight_.at(x, y) = std::min(total, std::max(vote, settings.maximum_history_weight));
        coverage_.at(x, y) = kind == TextureSampleKind::direct_observation
            ? kObserved : kInpainted;
        accepted = true;
    };

    const bool has_gradients = std::isfinite(sample.du_dx) && std::isfinite(sample.du_dy) &&
        std::isfinite(sample.dv_dx) && std::isfinite(sample.dv_dy) &&
        settings.maximum_splat_radius != 0;
    if (has_gradients) {
        const float j00 = sample.du_dx * static_cast<float>(width() - 1);
        const float j01 = sample.du_dy * static_cast<float>(width() - 1);
        const float j10 = sample.dv_dx * static_cast<float>(height() - 1);
        const float j11 = sample.dv_dy * static_cast<float>(height() - 1);
        // J*J^T is the UV-space covariance of a screen pixel. A half-texel
        // floor keeps magnified samples stable and invertible.
        const float covariance_xx = j00*j00 + j01*j01 + 0.25f;
        const float covariance_xy = j00*j10 + j01*j11;
        const float covariance_yy = j10*j10 + j11*j11 + 0.25f;
        const float determinant = covariance_xx*covariance_yy - covariance_xy*covariance_xy;
        if (std::isfinite(determinant) && determinant > 1.0e-8f) {
            const float inverse_xx = covariance_yy / determinant;
            const float inverse_xy = -covariance_xy / determinant;
            const float inverse_yy = covariance_xx / determinant;
            const auto limit = static_cast<int>(settings.maximum_splat_radius);
            const int radius_x = std::min(limit, std::max(1,
                static_cast<int>(std::ceil(2.0f * std::sqrt(covariance_xx)))));
            const int radius_y = std::min(limit, std::max(1,
                static_cast<int>(std::ceil(2.0f * std::sqrt(covariance_yy)))));
            const int center_x = static_cast<int>(std::floor(fx));
            const int center_y = static_cast<int>(std::floor(fy));
            for (int y = center_y - radius_y; y <= center_y + radius_y; ++y) {
                for (int x = center_x - radius_x; x <= center_x + radius_x; ++x) {
                    const float dx = (static_cast<float>(x) + 0.5f) - fx;
                    const float dy = (static_cast<float>(y) + 0.5f) - fy;
                    const float distance = inverse_xx*dx*dx +
                        2.0f*inverse_xy*dx*dy + inverse_yy*dy*dy;
                    if (std::isfinite(distance) && distance <= 4.0f)
                        apply_vote(x, y, std::exp(-0.5f * distance));
                }
            }
            return accepted;
        }
    }

    const int x0 = static_cast<int>(std::floor(fx));
    const int y0 = static_cast<int>(std::floor(fy));
    const float tx = fx - x0;
    const float ty = fy - y0;
    const std::array<float, 4> bilinear {
        (1.0f-tx)*(1.0f-ty), tx*(1.0f-ty), (1.0f-tx)*ty, tx*ty
    };
    const std::array<int, 4> xs {x0, x0+1, x0, x0+1};
    const std::array<int, 4> ys {y0, y0, y0+1, y0+1};
    for (std::size_t corner = 0; corner < 4; ++corner) {
        apply_vote(xs[corner], ys[corner], bilinear[corner]);
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

std::vector<MaterialTextureMip> MaterialTextureAtlas::generate_mips() const {
    std::vector<MaterialTextureMip> result;
    if (color_.empty()) return result;
    result.push_back({color_, coverage_});
    while (result.back().color.width() > 1 || result.back().color.height() > 1) {
        const auto &source = result.back();
        const auto width = (source.color.width() + 1) / 2;
        const auto height = (source.color.height() + 1) / 2;
        MaterialTextureMip next {
            Image<Color>(width, height, kUnpaintedColor),
            Image<std::uint8_t>(width, height, kUnseen)};
        for (std::uint32_t y = 0; y < height; ++y) {
            for (std::uint32_t x = 0; x < width; ++x) {
                Color sum {};
                std::uint32_t count = 0;
                std::uint8_t provenance = kObserved;
                bool complete = true;
                for (std::uint32_t oy = 0; oy < 2; ++oy) {
                    for (std::uint32_t ox = 0; ox < 2; ++ox) {
                        const auto sx = x * 2 + ox;
                        const auto sy = y * 2 + oy;
                        // Odd edge texels represent a smaller true footprint;
                        // absent children do not make that footprint incomplete.
                        if (sx >= source.color.width() || sy >= source.color.height()) continue;
                        const auto coverage = source.coverage.at(sx, sy);
                        if (coverage == kUnseen) { complete = false; continue; }
                        const auto value = source.color.at(sx, sy);
                        sum.r += value.r; sum.g += value.g; sum.b += value.b; sum.a += value.a;
                        provenance = std::min(provenance, coverage);
                        ++count;
                    }
                }
                if (!complete || count == 0) continue;
                const float divisor = static_cast<float>(count);
                next.color.at(x, y) = {sum.r/divisor, sum.g/divisor,
                                       sum.b/divisor, sum.a/divisor};
                next.coverage.at(x, y) = provenance;
            }
        }
        result.push_back(std::move(next));
    }
    return result;
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
    result.source_alpha = Image<float>(live_frame.width(), live_frame.height(), 1.0f);
    for (std::size_t index = 0; index < live_frame.size(); ++index)
        result.source_alpha.pixels()[index] = live_frame.pixels()[index].a;
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
        plan.reveal_mask.height() != inpainted_frame.height() ||
        plan.source_alpha.width() != inpainted_frame.width() ||
        plan.source_alpha.height() != inpainted_frame.height()) {
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
        auto generated = inpainted_frame.at(sample.screen_x, sample.screen_y);
        generated.a = plan.source_alpha.at(sample.screen_x, sample.screen_y);
        if (entry->second.observe(generated, sample, settings_.texture,
                                  TextureSampleKind::generated_inpaint)) {
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
