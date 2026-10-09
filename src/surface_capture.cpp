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
                              pixel.du_dx, pixel.du_dy, pixel.dv_dx, pixel.dv_dy});
        }
    }
    result.shrink_to_fit();
    return result;
}

SceneTransitionTracker::SceneTransitionTracker(SceneTransitionSettings settings)
    : settings_(settings) {
    if (!std::isfinite(settings_.material_overlap_threshold) ||
        settings_.material_overlap_threshold < 0.0f ||
        settings_.material_overlap_threshold > 1.0f ||
        settings_.scene_change_confirmation_frames == 0)
        throw std::invalid_argument("invalid scene transition settings");
}

float SceneTransitionTracker::overlap(
    const std::unordered_set<std::uint64_t> &visible) const {
    if (visible.empty() || scene_materials_.empty()) return 0.0f;
    std::size_t shared = 0;
    for (const auto material_id : visible)
        if (scene_materials_.contains(material_id)) ++shared;
    return static_cast<float>(shared) /
        static_cast<float>(std::min(visible.size(), scene_materials_.size()));
}

SceneTransition SceneTransitionTracker::observe(
    bool visual_cut,
    std::span<const SurfaceCorrespondence> correspondence) {
    std::unordered_set<std::uint64_t> visible;
    for (const auto &sample : correspondence)
        if (sample.material_id != 0 && std::isfinite(sample.confidence) && sample.confidence > 0.0f)
            visible.insert(sample.material_id);

    if (scene_materials_.empty()) {
        scene_materials_ = visible;
        return visual_cut ? SceneTransition::camera_cut : SceneTransition::stable;
    }

    const bool related = !visible.empty() && overlap(visible) >= settings_.material_overlap_threshold;
    if (visual_cut) {
        foreign_frames_ = related || visible.empty() ? 0u : 1u;
        candidate_materials_ = related ? std::unordered_set<std::uint64_t> {} : visible;
        if (related) scene_materials_.insert(visible.begin(), visible.end());
        return foreign_frames_ == 0
            ? SceneTransition::camera_cut
            : SceneTransition::pending_scene_change;
    }

    if (foreign_frames_ != 0) {
        if (related) {
            foreign_frames_ = 0;
            candidate_materials_.clear();
            scene_materials_.insert(visible.begin(), visible.end());
            return SceneTransition::camera_cut;
        }
        if (visible.empty()) return SceneTransition::pending_scene_change;
        candidate_materials_.insert(visible.begin(), visible.end());
        if (++foreign_frames_ >= settings_.scene_change_confirmation_frames) {
            scene_materials_ = std::move(candidate_materials_);
            candidate_materials_.clear();
            foreign_frames_ = 0;
            advance(scene_key_);
            return SceneTransition::scene_change;
        }
        return SceneTransition::pending_scene_change;
    }

    if (related) scene_materials_.insert(visible.begin(), visible.end());
    return SceneTransition::stable;
}

void SceneTransitionTracker::reset() {
    scene_materials_.clear();
    candidate_materials_.clear();
    foreign_frames_ = 0;
    scene_key_.identity = 0;
    advance(scene_key_);
}

} // namespace neuralpass
