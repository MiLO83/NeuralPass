#pragma once

#include "neuralpass/types.hpp"

#include <cstdint>
#include <filesystem>
#include <limits>
#include <optional>
#include <span>
#include <unordered_map>

namespace neuralpass {

// One visible framebuffer pixel mapped back to a material's texture space.
// A capture adapter supplies these records; the baker itself is graphics-API agnostic.
struct SurfaceCorrespondence {
    std::uint32_t screen_x = 0;
    std::uint32_t screen_y = 0;
    std::uint64_t material_id = 0;
    float u = 0.0f;
    float v = 0.0f;
    float confidence = 1.0f;
    // Linear depth from the game framebuffer and from the correspondence ray.
    // NaN keeps depth validation optional for adapters that have not exposed it yet.
    float framebuffer_depth = std::numeric_limits<float>::quiet_NaN();
    float hit_depth = std::numeric_limits<float>::quiet_NaN();
    // Screen-space UV derivatives. When all four are finite the baker uses an
    // elliptical pixel footprint; otherwise it falls back to bilinear splatting.
    float du_dx = std::numeric_limits<float>::quiet_NaN();
    float du_dy = std::numeric_limits<float>::quiet_NaN();
    float dv_dx = std::numeric_limits<float>::quiet_NaN();
    float dv_dy = std::numeric_limits<float>::quiet_NaN();
};

struct TextureBakeSettings {
    float observation_weight = 1.0f;
    float maximum_history_weight = 32.0f;
    bool wrap_u = true;
    bool wrap_v = true;
    // Observed texels are immutable. Inpainted texels remain replaceable by a
    // later real screen observation.
    bool fill_only_unobserved = true;
    float depth_absolute_threshold = 0.002f;
    float depth_relative_threshold = 0.02f;
    std::uint32_t maximum_splat_radius = 16;
};

enum class TextureSampleKind : std::uint8_t {
    generated_inpaint,
    direct_observation,
};

struct MaterialTextureMip {
    Image<Color> color;
    Image<std::uint8_t> coverage;
};

class MaterialTextureAtlas {
public:
    static constexpr Color kUnpaintedColor {1.0f, 0.0f, 1.0f, 1.0f};
    static constexpr std::uint8_t kUnseen = 0;
    static constexpr std::uint8_t kInpainted = 1;
    static constexpr std::uint8_t kObserved = 2;

    MaterialTextureAtlas() = default;
    MaterialTextureAtlas(std::uint64_t material_id, std::uint32_t width, std::uint32_t height);

    [[nodiscard]] std::uint64_t material_id() const noexcept { return material_id_; }
    [[nodiscard]] std::uint32_t width() const noexcept { return color_.width(); }
    [[nodiscard]] std::uint32_t height() const noexcept { return color_.height(); }
    [[nodiscard]] const Image<Color> &color() const noexcept { return color_; }
    // kUnseen = magenta sentinel, kInpainted = synthetic preview, kObserved =
    // immutable color projected from a restyled screen pixel.
    [[nodiscard]] const Image<std::uint8_t> &coverage() const noexcept { return coverage_; }

    // Records one verified screen observation. Returns false when the sample
    // does not belong to this material or fails bounds/depth validation.
    bool observe(const Color &restyled, const SurfaceCorrespondence &correspondence,
                 const TextureBakeSettings &settings = {},
                 TextureSampleKind kind = TextureSampleKind::direct_observation);
    void splat(const Image<Color> &restyled_screen,
               std::span<const SurfaceCorrespondence> correspondence,
               const TextureBakeSettings &settings = {});
    void inpaint_unseen(std::uint32_t maximum_passes = 0);
    [[nodiscard]] Color sample(float u, float v, bool wrap_u = true, bool wrap_v = true) const;
    [[nodiscard]] bool has_coverage(float u, float v, bool wrap_u = true,
                                    bool wrap_v = true) const;
    // Level zero is a copy of the atlas. A coarser texel remains unseen unless
    // every source texel in its footprint is covered, preventing mip filtering
    // from growing style into never-observed texture regions.
    [[nodiscard]] std::vector<MaterialTextureMip> generate_mips() const;

    // Versioned binary snapshots include color, observation weights, coverage,
    // and a payload checksum. The caller chooses the final path.
    [[nodiscard]] bool save(const std::filesystem::path &path) const;
    [[nodiscard]] static std::optional<MaterialTextureAtlas> load(
        const std::filesystem::path &path);

private:
    std::uint64_t material_id_ = 0;
    Image<Color> color_;
    Image<float> history_weight_;
    Image<std::uint8_t> coverage_;
};

// Samples a baked material back into screen space. Pixels without a matching,
// valid correspondence retain the supplied fallback framebuffer.
Image<Color> reconstruct_from_atlas(
    const Image<Color> &fallback,
    const MaterialTextureAtlas &atlas,
    std::span<const SurfaceCorrespondence> correspondence);

struct MaterialTextureBakerSettings {
    std::uint32_t atlas_width = 1024;
    std::uint32_t atlas_height = 1024;
    TextureBakeSettings texture {};
};

struct MaterialTextureBakeStats {
    std::size_t submitted = 0;
    std::size_t accepted = 0;
    std::size_t materials_touched = 0;
    bool stale = false;
};

struct MaterialTextureBakePlan {
    // Known material texels projected over the live framebuffer. Newly exposed
    // pixels intentionally retain the live color until an inpaint result lands.
    Image<Color> composite;
    Image<std::uint8_t> reveal_mask;
    // Inference output alpha is not authoritative. Keep the live framebuffer
    // alpha alongside the request so generated texels cannot alter cutouts.
    Image<float> source_alpha;
    std::uint64_t epoch = 0;
    std::size_t known_pixels = 0;
    std::size_t revealed_pixels = 0;
};

struct AtlasCacheStats {
    std::size_t saved = 0;
    std::size_t loaded = 0;
    std::size_t rejected = 0;
};

// Owns the persistent atlas set for a scene. The capture adapter may submit a
// sparse list (only pixels with trustworthy material/UV/depth data); all other
// screen pixels remain on the normal temporal fallback path.
class MaterialTextureBaker {
public:
    explicit MaterialTextureBaker(MaterialTextureBakerSettings settings = {});

    [[nodiscard]] MaterialTextureBakeStats update(
        const Image<Color> &restyled_screen,
        std::span<const SurfaceCorrespondence> correspondence);
    [[nodiscard]] MaterialTextureBakePlan plan(
        const Image<Color> &live_frame,
        std::span<const SurfaceCorrespondence> correspondence) const;
    [[nodiscard]] MaterialTextureBakeStats commit(
        const MaterialTextureBakePlan &plan,
        const Image<Color> &inpainted_frame,
        std::span<const SurfaceCorrespondence> correspondence);
    [[nodiscard]] Image<Color> reconstruct(
        const Image<Color> &fallback,
        std::span<const SurfaceCorrespondence> correspondence) const;

    // Offline/export helper only. The live path should use plan/commit so it
    // generates only visible newly revealed texels instead of flood-filling an
    // entire atlas without framebuffer evidence.
    void inpaint(std::uint32_t maximum_passes = 0);
    // Camera cuts invalidate asynchronous results but retain material atlases.
    void invalidate_in_flight() noexcept { ++epoch_; }
    // True scene changes start a new cache and invalidate asynchronous results.
    void reset_scene() noexcept { atlases_.clear(); ++epoch_; }
    void clear() noexcept { reset_scene(); }
    [[nodiscard]] std::uint64_t epoch() const noexcept { return epoch_; }
    [[nodiscard]] std::size_t material_count() const noexcept { return atlases_.size(); }
    [[nodiscard]] MaterialTextureAtlas *find(std::uint64_t material_id) noexcept;
    [[nodiscard]] const MaterialTextureAtlas *find(std::uint64_t material_id) const noexcept;

    // Only IDs proven restart-stable by the graphics adapter may be saved.
    // Snapshots use unique generation names, so a crash cannot corrupt the
    // previously completed generation. Loading picks the newest valid snapshot
    // for each material and ignores temporary/corrupt files.
    [[nodiscard]] AtlasCacheStats save_cache(
        const std::filesystem::path &directory,
        std::span<const std::uint64_t> restart_stable_material_ids) const;
    [[nodiscard]] AtlasCacheStats load_cache(
        const std::filesystem::path &directory, bool replace = true);

private:
    MaterialTextureBakerSettings settings_;
    std::unordered_map<std::uint64_t, MaterialTextureAtlas> atlases_;
    std::uint64_t epoch_ = 1;
};

} // namespace neuralpass
