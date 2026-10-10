#include "neuralpass/surface_capture.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace neuralpass {

namespace {

void advance(SceneKey &key) noexcept {
    ++key.generation;
    if (key.generation == 0) key.generation = 1;
}

constexpr std::uint64_t k_fnv_offset = 14695981039346656037ull;
constexpr std::uint64_t k_fnv_prime = 1099511628211ull;
constexpr std::size_t k_max_profile_samples = 4096;
constexpr std::size_t k_max_evidence_entries = 16384;

void hash_u64(std::uint64_t &hash, std::uint64_t value) noexcept {
    for (unsigned shift = 0; shift < 64; shift += 8) {
        hash ^= (value >> shift) & 0xffu;
        hash *= k_fnv_prime;
    }
}

std::uint32_t unit_bin(float value, std::uint32_t bins) noexcept {
    if (!std::isfinite(value)) return 0;
    value -= std::floor(value);
    return std::min(bins - 1, static_cast<std::uint32_t>(value * bins));
}

std::uint32_t depth_bin(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    const auto magnitude = std::log2(1.0f + std::abs(value));
    return std::min(255u, static_cast<std::uint32_t>(magnitude * 32.0f));
}

std::uint32_t color_bin(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    return std::min(15u, static_cast<std::uint32_t>(
        std::clamp(value, 0.0f, 1.0f) * 16.0f));
}

template <typename Set>
float set_overlap(const Set &left, const Set &right) noexcept {
    if (left.empty() || right.empty()) return 0.0f;
    const auto &small = left.size() <= right.size() ? left : right;
    const auto &large = left.size() <= right.size() ? right : left;
    std::size_t shared = 0;
    for (const auto value : small)
        if (large.contains(value)) ++shared;
    return static_cast<float>(shared) / static_cast<float>(small.size());
}

template <typename Set>
std::size_t shared_count(const Set &left, const Set &right) noexcept {
    if (left.empty() || right.empty()) return 0;
    const auto &small = left.size() <= right.size() ? left : right;
    const auto &large = left.size() <= right.size() ? right : left;
    return static_cast<std::size_t>(std::count_if(
        small.begin(), small.end(), [&](auto value) { return large.contains(value); }));
}

template <typename Set>
void merge_set(Set &destination, const Set &source) {
    destination.insert(source.begin(), source.end());
}

} // namespace

std::vector<SurfaceCorrespondence> SurfaceCaptureFrame::correspondences(
    float minimum_confidence) const {
    std::vector<SurfaceCorrespondence> result;
    if (pixels_.empty()) return result;
    minimum_confidence = std::clamp(minimum_confidence, 0.0f, 1.0f);
    result.reserve(pixels_.size() / 2);
    for (std::uint32_t y = 0; y < height(); ++y) {
        for (std::uint32_t x = 0; x < width(); ++x) {
            const auto &pixel = pixels_.at(x, y);
            if (pixel.material_id == 0 || !std::isfinite(pixel.u) || !std::isfinite(pixel.v) ||
                !std::isfinite(pixel.confidence) || pixel.confidence < minimum_confidence)
                continue;
            // Depth remains optional, but a backend must supply both sides or
            // neither so that a half-populated comparison cannot be trusted.
            const bool has_framebuffer_depth = std::isfinite(pixel.framebuffer_depth);
            const bool has_hit_depth = std::isfinite(pixel.hit_depth);
            if (has_framebuffer_depth != has_hit_depth) continue;
            result.push_back({x, y, pixel.material_id, pixel.u, pixel.v, pixel.confidence,
                              pixel.framebuffer_depth, pixel.hit_depth,
                              pixel.du_dx, pixel.du_dy, pixel.dv_dx, pixel.dv_dy,
                              pixel.source_r, pixel.source_g, pixel.source_b, pixel.source_a});
        }
    }
    result.shrink_to_fit();
    return result;
}

SceneTransitionTracker::SceneTransitionTracker(SceneTransitionSettings settings)
    : settings_(settings) {
    if (!std::isfinite(settings_.material_overlap_threshold) ||
        !std::isfinite(settings_.strong_material_overlap_threshold) ||
        !std::isfinite(settings_.composite_evidence_threshold) ||
        settings_.material_overlap_threshold < 0.0f ||
        settings_.material_overlap_threshold > 1.0f ||
        settings_.strong_material_overlap_threshold < settings_.material_overlap_threshold ||
        settings_.strong_material_overlap_threshold > 1.0f ||
        settings_.composite_evidence_threshold < 0.0f ||
        settings_.composite_evidence_threshold > 1.0f ||
        settings_.scene_change_confirmation_frames == 0)
        throw std::invalid_argument("invalid scene transition settings");
}

void SceneTransitionTracker::EvidenceProfile::clear() noexcept {
    materials.clear(); uv.clear(); depth.clear(); color.clear();
}

SceneTransitionTracker::EvidenceProfile SceneTransitionTracker::profile(
    std::span<const SurfaceCorrespondence> correspondence) {
    SceneTransitionTracker::EvidenceProfile result;
    const auto stride = std::max<std::size_t>(
        1, (correspondence.size() + k_max_profile_samples - 1) / k_max_profile_samples);
    for (std::size_t index = 0; index < correspondence.size(); ++index) {
        const auto &sample = correspondence[index];
        if (sample.material_id == 0 || !std::isfinite(sample.confidence) ||
            sample.confidence <= 0.0f)
            continue;
        result.materials.insert(sample.material_id);
        if (index % stride != 0) continue;
        if (std::isfinite(sample.u) && std::isfinite(sample.v)) {
            auto hash = k_fnv_offset;
            hash_u64(hash, sample.material_id);
            hash_u64(hash, unit_bin(sample.u, 16));
            hash_u64(hash, unit_bin(sample.v, 16));
            if (result.uv.size() < k_max_evidence_entries) result.uv.insert(hash);
        }
        if (std::isfinite(sample.framebuffer_depth) && std::isfinite(sample.hit_depth)) {
            auto hash = k_fnv_offset;
            hash_u64(hash, sample.material_id);
            hash_u64(hash, depth_bin(sample.framebuffer_depth));
            hash_u64(hash, depth_bin(sample.hit_depth));
            if (result.depth.size() < k_max_evidence_entries) result.depth.insert(hash);
        }
        if (has_source_texture_sample(sample)) {
            auto hash = k_fnv_offset;
            hash_u64(hash, sample.material_id);
            hash_u64(hash, color_bin(sample.source_r));
            hash_u64(hash, color_bin(sample.source_g));
            hash_u64(hash, color_bin(sample.source_b));
            hash_u64(hash, color_bin(sample.source_a));
            if (result.color.size() < k_max_evidence_entries) result.color.insert(hash);
        }
    }
    return result;
}

void SceneTransitionTracker::merge(EvidenceProfile &destination,
                                   const EvidenceProfile &source) {
    merge_set(destination.materials, source.materials);
    const auto merge_bounded = [](auto &target, const auto &values) {
        for (const auto value : values) {
            if (target.size() >= k_max_evidence_entries) break;
            target.insert(value);
        }
    };
    merge_bounded(destination.uv, source.uv);
    merge_bounded(destination.depth, source.depth);
    merge_bounded(destination.color, source.color);
}

bool SceneTransitionTracker::related(const EvidenceProfile &visible) const {
    const auto material = set_overlap(scene_evidence_.materials, visible.materials);
    if (material < settings_.material_overlap_threshold) return false;
    // Multiple agreeing canonical material identities already combine pipeline,
    // descriptor binding, resource content, and immutable geometry topology.
    if (shared_count(scene_evidence_.materials, visible.materials) >= 2 &&
        material >= settings_.strong_material_overlap_threshold)
        return true;

    float weighted = material * 0.50f;
    float weight = 0.50f;
    const auto add = [&](const auto &known, const auto &current, float channel_weight) {
        if (known.empty() || current.empty()) return;
        weighted += set_overlap(known, current) * channel_weight;
        weight += channel_weight;
    };
    add(scene_evidence_.uv, visible.uv, 0.20f);
    add(scene_evidence_.depth, visible.depth, 0.15f);
    add(scene_evidence_.color, visible.color, 0.15f);
    return weighted / weight >= settings_.composite_evidence_threshold;
}

SceneTransition SceneTransitionTracker::observe(
    bool visual_cut,
    std::span<const SurfaceCorrespondence> correspondence) {
    auto visible = profile(correspondence);

    if (scene_evidence_.empty()) {
        scene_evidence_ = visible;
        return visual_cut ? SceneTransition::camera_cut : SceneTransition::stable;
    }

    const bool is_related = !visible.empty() && related(visible);
    if (visual_cut) {
        foreign_frames_ = is_related || visible.empty() ? 0u : 1u;
        candidate_evidence_ = is_related ? EvidenceProfile {} : visible;
        if (is_related) merge(scene_evidence_, visible);
        return foreign_frames_ == 0
            ? SceneTransition::camera_cut
            : SceneTransition::pending_scene_change;
    }

    if (foreign_frames_ != 0) {
        if (is_related) {
            foreign_frames_ = 0;
            candidate_evidence_.clear();
            merge(scene_evidence_, visible);
            return SceneTransition::camera_cut;
        }
        if (visible.empty()) return SceneTransition::pending_scene_change;
        merge(candidate_evidence_, visible);
        if (++foreign_frames_ >= settings_.scene_change_confirmation_frames) {
            scene_evidence_ = std::move(candidate_evidence_);
            candidate_evidence_.clear();
            foreign_frames_ = 0;
            advance(scene_key_);
            return SceneTransition::scene_change;
        }
        return SceneTransition::pending_scene_change;
    }

    if (is_related) merge(scene_evidence_, visible);
    return SceneTransition::stable;
}

void SceneTransitionTracker::keep_current_scene(
    std::span<const SurfaceCorrespondence> correspondence) {
    merge(scene_evidence_, profile(correspondence));
    candidate_evidence_.clear();
    foreign_frames_ = 0;
    advance(scene_key_);
}

void SceneTransitionTracker::start_new_scene(
    std::span<const SurfaceCorrespondence> correspondence) {
    scene_evidence_ = profile(correspondence);
    candidate_evidence_.clear();
    foreign_frames_ = 0;
    scene_key_.identity = 0;
    advance(scene_key_);
}

void SceneTransitionTracker::reset() {
    scene_evidence_.clear();
    candidate_evidence_.clear();
    foreign_frames_ = 0;
    scene_key_.identity = 0;
    advance(scene_key_);
}

} // namespace neuralpass
