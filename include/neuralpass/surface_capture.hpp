#pragma once

#include "neuralpass/texture_baker.hpp"
#include "neuralpass/types.hpp"

#include <cstdint>
#include <limits>
#include <span>
#include <unordered_set>
#include <vector>

namespace neuralpass {

// Canonical CPU representation produced by every graphics backend after its
// G-buffer/readback step. Material ID zero and confidence zero mean that the
// pixel is unsupported and must remain on the screen-space fallback path.
struct SurfaceCapturePixel {
    std::uint64_t material_id = 0;
    float u = 0.0f;
    float v = 0.0f;
    float framebuffer_depth = std::numeric_limits<float>::quiet_NaN();
    float hit_depth = std::numeric_limits<float>::quiet_NaN();
    float confidence = 0.0f;
    float du_dx = std::numeric_limits<float>::quiet_NaN();
    float du_dy = std::numeric_limits<float>::quiet_NaN();
    float dv_dx = std::numeric_limits<float>::quiet_NaN();
    float dv_dy = std::numeric_limits<float>::quiet_NaN();
};

class SurfaceCaptureFrame {
public:
    SurfaceCaptureFrame() = default;
    SurfaceCaptureFrame(std::uint32_t width, std::uint32_t height,
                        std::uint64_t frame_index = 0)
        : pixels_(width, height), frame_index_(frame_index) {}

    [[nodiscard]] std::uint32_t width() const noexcept { return pixels_.width(); }
    [[nodiscard]] std::uint32_t height() const noexcept { return pixels_.height(); }
    [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::uint64_t frame_index() const noexcept { return frame_index_; }
    [[nodiscard]] Image<SurfaceCapturePixel> &pixels() noexcept { return pixels_; }
    [[nodiscard]] const Image<SurfaceCapturePixel> &pixels() const noexcept { return pixels_; }

    // Compacts the dense GPU-friendly planes into sparse records consumed by
    // MaterialTextureBaker. Invalid IDs, UVs, depths, and weak samples are
    // rejected here once, identically for every graphics API.
    [[nodiscard]] std::vector<SurfaceCorrespondence> correspondences(
        float minimum_confidence = 0.01f) const;

private:
    Image<SurfaceCapturePixel> pixels_;
    std::uint64_t frame_index_ = 0;
};

enum class SceneTransition {
    stable,
    camera_cut,
    pending_scene_change,
    scene_change,
};

struct SceneKey {
    // Persistent catalog identity is zero until enough restart-stable evidence
    // exists. Generation remains process-local and rejects stale async work.
    std::uint64_t identity = 0;
    std::uint64_t generation = 1;

    [[nodiscard]] bool valid() const noexcept { return generation != 0; }
    bool operator==(const SceneKey &) const = default;
};

struct SceneTransitionSettings {
    float material_overlap_threshold = 0.10f;
    std::uint32_t scene_change_confirmation_frames = 3;
};

// Combines an immediate image-space cut signal with stable material identity.
// Every visual cut invalidates temporal reprojection. Foreign cuts enter a
// quarantined pending state so observations cannot contaminate either scene;
// the cache is replaced only after multiple frames confirm a new identity.
// With no material capture, it conservatively reports a camera cut.
class SceneTransitionTracker {
public:
    explicit SceneTransitionTracker(SceneTransitionSettings settings = {});

    [[nodiscard]] SceneTransition observe(
        bool visual_cut,
        std::span<const SurfaceCorrespondence> correspondence);
    [[nodiscard]] SceneKey scene_key() const noexcept { return scene_key_; }
    void set_scene_identity(std::uint64_t identity) noexcept { scene_key_.identity = identity; }
    void reset();

private:
    [[nodiscard]] float overlap(const std::unordered_set<std::uint64_t> &visible) const;

    SceneTransitionSettings settings_;
    std::unordered_set<std::uint64_t> scene_materials_;
    std::unordered_set<std::uint64_t> candidate_materials_;
    std::uint32_t foreign_frames_ = 0;
    SceneKey scene_key_;
};

} // namespace neuralpass
