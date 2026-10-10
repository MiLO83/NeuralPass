#include "neuralpass/temporal.hpp"
#include "neuralpass/binding_identity.hpp"
#include "neuralpass/color_pipeline.hpp"
#include "neuralpass/cache_namespace.hpp"
#include "neuralpass/depth_pyramid.hpp"
#include "neuralpass/tile_scheduler.hpp"
#include "neuralpass/inference.hpp"
#include "neuralpass/scene_cache.hpp"
#include "neuralpass/runtime_evidence.hpp"
#include "neuralpass/surface_capture.hpp"
#include "neuralpass/texture_baker.hpp"
#include "neuralpass/visibility.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

using namespace neuralpass;

static void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

static HistoryFrame solid_history(std::uint32_t w, std::uint32_t h, Color c) {
    return {Image<Color>(w, h, c), Image<Color>(w, h, c), Image<float>(w, h, 0.5f),
            Image<std::uint8_t>(w, h, 1), Image<std::uint16_t>(w, h, 0)};
}

static void test_reprojection_accepts_stable_pixels() {
    auto previous = solid_history(8, 8, {0.2f, 0.3f, 0.4f, 1.0f});
    Image<Color> current(8, 8, {0.2f, 0.3f, 0.4f, 1.0f});
    Image<float> depth(8, 8, 0.5f);
    Image<Motion> motion(8, 8, {});
    auto result = reproject_history(current, &depth, &motion, previous, {});
    require(result.accepted == 64 && result.rejected == 0, "stable pixels were rejected");
    require(result.dirty.at(3, 3) == 0 && result.reprojected.age.at(3, 3) == 1,
            "stable history was not propagated");
}

static void test_disocclusion_rejected() {
    auto previous = solid_history(8, 8, {0.2f, 0.3f, 0.4f, 1.0f});
    Image<Color> current(8, 8, {0.2f, 0.3f, 0.4f, 1.0f});
    Image<float> depth(8, 8, 0.5f);
    Image<Motion> motion(8, 8, {});
    depth.at(4, 4) = 0.9f;
    auto result = reproject_history(current, &depth, &motion, previous, {});
    require(result.dirty.at(4, 4) == 1, "depth disocclusion was accepted");
    motion.at(0, 0) = {-20.0f, 0.0f};
    result = reproject_history(current, &depth, &motion, previous, {});
    require(result.dirty.at(0, 0) == 1, "off-screen motion was accepted");
}

static void test_motion_reprojects_previous_pixel() {
    auto previous = solid_history(4, 1, {0.0f, 0.0f, 0.0f, 1.0f});
    for (std::uint32_t x = 0; x < 4; ++x) {
        const float value = static_cast<float>(x) / 4.0f;
        previous.source.at(x, 0) = {value, value, value, 1.0f};
        previous.styled.at(x, 0) = {1.0f-value, 0.0f, 0.0f, 1.0f};
    }
    Image<Color> current(4, 1);
    Image<Motion> motion(4, 1, {-1.0f, 0.0f});
    for (std::uint32_t x = 1; x < 4; ++x) current.at(x, 0) = previous.source.at(x-1, 0);
    auto result = reproject_history(current, nullptr, &motion, previous, {});
    require(result.dirty.at(0, 0) == 1, "newly exposed edge was not dirty");
    require(result.dirty.at(2, 0) == 0, "valid motion sample was rejected");
    require(result.reprojected.styled.at(2, 0).r == previous.styled.at(1, 0).r,
            "styled history came from the wrong motion sample");
}

static void test_dilation_and_priority() {
    Image<std::uint8_t> dirty(600, 300, 0);
    Image<std::uint16_t> age(600, 300, 0);
    dirty.at(300, 150) = 1;
    const auto dilated = dilate_mask(dirty, 2);
    require(dilated.at(298, 148) == 1 && dilated.at(297, 147) == 0, "mask dilation failed");
    const auto jobs = schedule_tiles(dirty, age, {.tile_size=256, .halo=32,
        .dilation_radius=2, .tile_budget=1, .refresh_age=120});
    require(jobs.size() == 1, "tile budget not honored");
    require(jobs[0].core.x == 256 && jobs[0].core.y == 0, "dirty tile was not selected");
    require(jobs[0].padded.x == 224 && jobs[0].padded.width == 320, "tile halo incorrect");
}

static void test_age_refresh() {
    Image<std::uint8_t> dirty(512, 256, 0);
    Image<std::uint16_t> age(512, 256, 0);
    age.at(400, 100) = 121;
    const auto jobs = schedule_tiles(dirty, age, {.tile_size=256, .halo=32,
        .dilation_radius=0, .tile_budget=2, .refresh_age=120});
    require(jobs.size() == 1 && jobs[0].core.x == 256 && jobs[0].refresh_only,
            "aged tile was not refreshed");
}

static void test_newly_visible_tiles_preempt_initial_styling() {
    Image<std::uint8_t> dirty(512, 256, 0);
    Image<std::uint8_t> newly_visible(512, 256, 0);
    Image<std::uint16_t> age(512, 256, 0);
    for (std::uint32_t y = 0; y < 256; ++y)
        for (std::uint32_t x = 0; x < 192; ++x)
            dirty.at(x, y) = 1;
    dirty.at(400, 100) = 1;
    newly_visible.at(400, 100) = 1;
    const auto jobs = schedule_tiles(dirty, age,
        {.tile_size=256, .halo=32, .dilation_radius=0,
         .tile_budget=1, .refresh_age=120}, &newly_visible);
    require(jobs.size() == 1 && jobs[0].core.x == 256 &&
            jobs[0].urgent_fraction > 0.0f,
            "newly visible inpaint did not preempt bulk first-observation styling");
}

static void test_preview_backend_is_bounded() {
    auto backend = make_preview_backend("photo-detail");
    Image<Color> image(3, 3, {0.5f, 0.5f, 0.5f, 1.0f});
    image.at(1, 1) = {1.0f, 0.0f, 0.25f, 1.0f};
    const auto output = backend->run(image);
    require(output.width() == 3 && output.height() == 3, "preview changed image dimensions");
    for (const auto &pixel : output.pixels())
        require(pixel.r >= 0.0f && pixel.r <= 1.0f && pixel.g >= 0.0f && pixel.g <= 1.0f &&
                pixel.b >= 0.0f && pixel.b <= 1.0f, "preview produced an invalid color");
}

static void test_hdr_color_contract_is_bounded_and_luminance_stable() {
    require(half_to_float(0x0000u) == 0.0f && half_to_float(0x3c00u) == 1.0f &&
            std::abs(half_to_float(0x4400u) - 4.0f) < 0.001f,
            "FP16 decoding changed canonical values");

    const std::array<std::uint16_t, 4> scrgb_half {0x4400u, 0x4400u, 0x4400u, 0x3c00u};
    std::array<std::uint8_t, 8> scrgb_bytes {};
    std::memcpy(scrgb_bytes.data(), scrgb_half.data(), scrgb_bytes.size());
    const auto inference = decode_capture_pixel(scrgb_bytes.data(),
        CapturePixelLayout::rgba16_float, DisplayEncoding::scrgb_linear);
    require(inference.r > 0.9f && inference.r <= 1.0f &&
            inference.r == inference.g && inference.g == inference.b,
            "scRGB capture was not tone-mapped into bounded neutral sRGB");
    const auto restored = composite_styled_pixel({4.0f, 4.0f, 4.0f, 1.0f},
        inference, DisplayEncoding::scrgb_linear);
    require(std::abs(restored.r - 4.0f) < 0.01f &&
            std::abs(restored.g - 4.0f) < 0.01f &&
            std::abs(restored.b - 4.0f) < 0.01f,
            "identity-styled scRGB did not restore source luminance");

    // 0.508 is approximately 100 nits in ST.2084. An identity style should
    // survive the BT.2020/PQ round trip despite RGB10 quantization.
    const std::uint32_t pq_code = 520u;
    const std::uint32_t packed = pq_code | pq_code << 10 | pq_code << 20 | 3u << 30;
    std::array<std::uint8_t, 4> pq_bytes {};
    std::memcpy(pq_bytes.data(), &packed, sizeof(packed));
    const auto pq_inference = decode_capture_pixel(pq_bytes.data(),
        CapturePixelLayout::rgb10a2, DisplayEncoding::hdr10_pq);
    const float encoded = static_cast<float>(pq_code) / 1023.0f;
    const auto pq_restored = composite_styled_pixel(
        {encoded, encoded, encoded, 1.0f}, pq_inference, DisplayEncoding::hdr10_pq);
    require(std::abs(pq_restored.r - encoded) < 0.002f &&
            std::abs(pq_restored.g - encoded) < 0.002f &&
            std::abs(pq_restored.b - encoded) < 0.002f,
            "identity-styled HDR10 did not preserve PQ luminance");

    const std::array<std::uint16_t, 4> invalid_half {0x7e00u, 0xfc00u, 0x0000u, 0x3c00u};
    std::memcpy(scrgb_bytes.data(), invalid_half.data(), scrgb_bytes.size());
    const auto sanitized = decode_capture_pixel(scrgb_bytes.data(),
        CapturePixelLayout::rgba16_float, DisplayEncoding::scrgb_linear);
    require(std::isfinite(sanitized.r) && std::isfinite(sanitized.g) &&
            sanitized.r == 0.0f && sanitized.g == 0.0f,
            "non-finite HDR capture values reached inference");
}

static void test_runtime_display_evidence_json_is_exact_and_escaped() {
    const auto json = runtime_display_evidence_json({
        .generated_utc = "2026-01-02T03:04:05.006Z",
        .process_architecture = "X64",
        .graphics_api = "d3d12\"probe",
        .graphics_api_value = 49152,
        .width = 3840,
        .height = 2160,
        .backbuffer_format = "r10g10b10a2_unorm",
        .backbuffer_format_value = 24,
        .swapchain_color_space = "hdr10_pq",
        .swapchain_color_space_value = 3,
        .display_capture_supported = true,
        .display_encoding = "hdr10_pq",
        .hdr_path = true,
        .classification = "compatible_format_color_space",
    });
    const std::string expected =
        "{\n"
        "  \"schema_version\": 1,\n"
        "  \"generated_utc\": \"2026-01-02T03:04:05.006Z\",\n"
        "  \"process_architecture\": \"X64\",\n"
        "  \"graphics_api\": \"d3d12\\\"probe\",\n"
        "  \"graphics_api_value\": 49152,\n"
        "  \"width\": 3840,\n"
        "  \"height\": 2160,\n"
        "  \"backbuffer_format\": \"r10g10b10a2_unorm\",\n"
        "  \"backbuffer_format_value\": 24,\n"
        "  \"swapchain_color_space\": \"hdr10_pq\",\n"
        "  \"swapchain_color_space_value\": 3,\n"
        "  \"display_capture_supported\": true,\n"
        "  \"display_encoding\": \"hdr10_pq\",\n"
        "  \"hdr_path\": true,\n"
        "  \"classification\": \"compatible_format_color_space\"\n"
        "}\n";
    require(json == expected, "runtime display evidence JSON contract changed");
}

static void test_binding_identity_is_pipeline_and_slot_specific() {
    const std::array<std::uint64_t, 2> pipeline {100, 200};
    const std::array<DescriptorIdentity, 2> descriptors {{
        {3, 1001, true}, {7, 1002, true},
    }};
    const std::array<DescriptorIdentity, 2> reordered {{
        {7, 1002, true}, {3, 1001, true},
    }};
    const std::array<DescriptorIdentity, 2> swapped_slots {{
        {3, 1002, true}, {7, 1001, true},
    }};
    const std::array<std::uint64_t, 2> other_pipeline {100, 201};
    const auto key = make_binding_instance_key(pipeline, descriptors, true);
    require(key.valid() && key.restart_stable,
            "complete content-backed binding did not produce a stable key");
    require(make_binding_instance_key(pipeline, reordered, true).value == key.value,
            "descriptor enumeration order changed binding identity");
    require(make_binding_instance_key(pipeline, swapped_slots, true).value != key.value,
            "different descriptor placement shared a binding identity");
    require(make_binding_instance_key(other_pipeline, descriptors, true).value != key.value,
            "different pipelines shared a binding identity");
    require(!make_binding_instance_key(pipeline, descriptors, false).restart_stable,
            "incomplete pipeline identity was marked restart-stable");
    auto transient = descriptors;
    transient[0].restart_stable = false;
    require(!make_binding_instance_key(pipeline, transient, true).restart_stable,
            "transient descriptor identity was marked restart-stable");
}

static void test_depth_pyramid_and_conservative_raymarch() {
    Image<float> depth(8, 4, 10.0f);
    depth.at(4, 2) = 2.0f;
    depth.at(7, 3) = std::numeric_limits<float>::quiet_NaN();
    DepthPyramid pyramid(depth);
    require(pyramid.level_count() == 4 &&
            pyramid.level(1).minimum.width() == 4 &&
            pyramid.level(1).minimum.height() == 2 &&
            pyramid.level(3).minimum.width() == 1,
            "depth pyramid dimensions are incorrect");
    require(pyramid.level(3).minimum.at(0, 0) == 2.0f &&
            pyramid.level(3).maximum.at(0, 0) == 10.0f,
            "depth pyramid did not preserve conservative depth bounds");
    require(!depth_segment_visible(pyramid, 0.0f, 2.0f, 5.0f,
                                   7.0f, 2.0f, 5.0f),
            "hierarchical raymarch missed a foreground occluder");
    require(depth_segment_visible(pyramid, 0.0f, 0.0f, 5.0f,
                                  7.0f, 0.0f, 5.0f),
            "hierarchical raymarch rejected an unobstructed segment");
    require(!depth_segment_visible(pyramid, -1.0f, 0.0f, 5.0f,
                                   7.0f, 0.0f, 5.0f),
            "out-of-bounds raymarch endpoint was accepted");
}

static void test_visibility_classifies_newly_revealed_causes() {
    SurfaceCaptureFrame previous(6, 1, 1);
    SurfaceCaptureFrame current(6, 1, 2);
    previous.pixels().at(0, 0) = {1, 0.1f, 0.2f, 4.0f, 4.0f, 1.0f};
    current.pixels().at(0, 0) = {1, 0.1f, 0.2f, 4.0f, 4.0f, 1.0f};
    previous.pixels().at(1, 0) = {9, 0.4f, 0.4f, 2.0f, 2.0f, 1.0f};
    current.pixels().at(1, 0) = {2, 0.3f, 0.3f, 5.0f, 5.0f, 1.0f};
    current.pixels().at(2, 0) = {3, 0.8f, 0.8f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::quiet_NaN(), 1.0f};
    current.pixels().at(3, 0) = {4, 0.2f, 0.2f, 3.0f, 3.0f, 1.0f};
    current.pixels().at(4, 0) = {5, 0.2f, 0.2f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::quiet_NaN(), 1.0f};
    previous.pixels().at(5, 0) = {3, 0.1f, 0.1f, 3.0f, 3.0f, 1.0f};
    Image<Motion> motion(6, 1, {});
    motion.at(3, 0) = {10.0f, 0.0f};
    const auto classes = classify_visibility(current, &previous, &motion);
    require(classes.pixels.at(0, 0) == VisibilityClass::known_visible &&
            classes.pixels.at(1, 0) == VisibilityClass::disoccluded &&
            classes.pixels.at(2, 0) == VisibilityClass::newly_front_facing &&
            classes.pixels.at(3, 0) == VisibilityClass::offscreen_entry &&
            classes.pixels.at(4, 0) == VisibilityClass::first_observation &&
            classes.pixels.at(5, 0) == VisibilityClass::unsupported,
            "visibility causes were not classified independently");
    require(classes.count(VisibilityClass::known_visible) == 1 &&
            classes.count(VisibilityClass::unsupported) == 1,
            "visibility classification counts are incorrect");

    const auto after_cut = classify_visibility(current, nullptr, nullptr);
    require(after_cut.count(VisibilityClass::first_observation) == 5 &&
            after_cut.count(VisibilityClass::unsupported) == 1,
            "camera-cut classification reused previous-frame visibility");
}

static void test_texture_baker_splats_inpaints_and_reconstructs() {
    Image<Color> restyled(2, 1);
    restyled.at(0, 0) = {1.0f, 0.1f, 0.0f, 1.0f};
    restyled.at(1, 0) = {0.0f, 0.2f, 1.0f, 1.0f};
    const std::array<SurfaceCorrespondence, 2> mapping {{
        {0, 0, 7, 0.25f, 0.5f, 1.0f},
        {1, 0, 7, 0.75f, 0.5f, 1.0f},
    }};
    MaterialTextureAtlas atlas(7, 16, 8);
    require(atlas.color().at(0, 0).r == 1.0f && atlas.color().at(0, 0).g == 0.0f &&
            atlas.color().at(0, 0).b == 1.0f,
            "new atlas was not initialized to the magenta sentinel");
    atlas.splat(restyled, mapping);
    const auto observed_before = static_cast<std::size_t>(std::count(
        atlas.coverage().pixels().begin(), atlas.coverage().pixels().end(), std::uint8_t {2}));
    require(observed_before > 0 && observed_before < atlas.coverage().size(),
            "UV splat did not produce a partial observed mask");
    atlas.inpaint_unseen();
    require(std::count(atlas.coverage().pixels().begin(), atlas.coverage().pixels().end(),
                       std::uint8_t {0}) == 0,
            "UV inpaint left unseen texture pixels");
    Image<Color> fallback(2, 1, {0.0f, 1.0f, 0.0f, 1.0f});
    const auto reconstructed = reconstruct_from_atlas(fallback, atlas, mapping);
    require(reconstructed.at(0, 0).r > reconstructed.at(0, 0).b &&
            reconstructed.at(1, 0).b > reconstructed.at(1, 0).r,
            "wrapped atlas did not reproduce the visible restyled colors");
}

static void test_texture_baker_locks_observed_texels_and_rejects_bad_depth() {
    Image<Color> restyled(3, 1);
    restyled.at(0, 0) = {1.0f, 0.0f, 0.0f, 1.0f};
    restyled.at(1, 0) = {0.0f, 0.0f, 1.0f, 1.0f};
    restyled.at(2, 0) = {0.0f, 1.0f, 0.0f, 1.0f};
    MaterialTextureAtlas atlas(9, 8, 8);
    const std::array<SurfaceCorrespondence, 3> mapping {{
        {0, 0, 9, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f},
        {1, 0, 9, 0.5f, 0.5f, 1.0f, 0.5f, 0.5f},
        {2, 0, 9, 0.1f, 0.1f, 1.0f, 0.2f, 0.9f},
    }};
    atlas.splat(restyled, mapping);
    const auto locked = atlas.sample(0.5f, 0.5f);
    require(locked.r > locked.b, "later framebuffer sample overwrote an observed texel");
    require(atlas.coverage().at(1, 1) == MaterialTextureAtlas::kUnseen,
            "depth-mismatched sample painted an atlas texel");
}

static void test_elliptical_uv_splat_uses_gradients_and_confidence() {
    MaterialTextureAtlas atlas(10, 64, 64);
    SurfaceCorrespondence sample {0, 0, 10, 0.5f, 0.5f, 0.25f};
    sample.du_dx = 0.08f;
    sample.du_dy = 0.0f;
    sample.dv_dx = 0.0f;
    sample.dv_dy = 0.015f;
    TextureBakeSettings settings;
    settings.fill_only_unobserved = false;
    require(atlas.observe({1.0f, 0.0f, 0.0f, 1.0f}, sample, settings),
            "gradient-driven elliptical splat rejected a valid sample");
    const auto footprint = static_cast<std::size_t>(std::count_if(
        atlas.coverage().pixels().begin(), atlas.coverage().pixels().end(),
        [](std::uint8_t value) { return value != MaterialTextureAtlas::kUnseen; }));
    require(footprint > 4 && footprint < 256,
            "elliptical splat did not create a bounded anisotropic footprint");

    sample.confidence = 1.0f;
    require(atlas.observe({0.0f, 0.0f, 1.0f, 1.0f}, sample, settings),
            "second confidence-weighted elliptical sample was rejected");
    const auto center = atlas.sample(0.5f, 0.5f);
    require(center.b > center.r,
            "higher-confidence UV observation did not dominate accumulated color");
}

static void test_mips_do_not_expand_texture_coverage() {
    MaterialTextureAtlas atlas(12, 4, 4);
    TextureBakeSettings settings;
    settings.wrap_u = false;
    settings.wrap_v = false;
    for (std::uint32_t y = 0; y < 2; ++y) {
        for (std::uint32_t x = 0; x < 2; ++x) {
            SurfaceCorrespondence sample {0, 0, 12,
                static_cast<float>(x) / 3.0f,
                static_cast<float>(y) / 3.0f, 1.0f};
            const auto kind = x == 1 && y == 1
                ? TextureSampleKind::generated_inpaint
                : TextureSampleKind::direct_observation;
            require(atlas.observe({0.8f, 0.2f, 0.1f, 0.5f}, sample, settings, kind),
                    "mip fixture did not paint its source footprint");
        }
    }
    const auto mips = atlas.generate_mips();
    require(mips.size() == 3 && mips[1].color.width() == 2 &&
            mips[2].color.width() == 1,
            "atlas generated an invalid mip chain");
    require(mips[1].coverage.at(0, 0) == MaterialTextureAtlas::kInpainted &&
            mips[1].coverage.at(1, 0) == MaterialTextureAtlas::kUnseen &&
            mips[1].coverage.at(0, 1) == MaterialTextureAtlas::kUnseen &&
            mips[1].coverage.at(1, 1) == MaterialTextureAtlas::kUnseen,
            "first mip expanded or lost conservative provenance coverage");
    const auto coarse = mips[2].color.at(0, 0);
    require(mips[2].coverage.at(0, 0) == MaterialTextureAtlas::kUnseen &&
            coarse.r == MaterialTextureAtlas::kUnpaintedColor.r &&
            coarse.g == MaterialTextureAtlas::kUnpaintedColor.g &&
            coarse.b == MaterialTextureAtlas::kUnpaintedColor.b,
            "coarse mip leaked style across an unseen texture footprint");
}

static void test_linear_source_texture_transfer_and_alpha() {
    TextureBakeSettings settings;
    const Color source {0.2f, 0.4f, 0.6f, 0.25f};
    const auto neutral = transfer_to_source_texture(
        {0.5f, 0.5f, 0.5f, 1.0f}, {0.5f, 0.5f, 0.5f, 0.1f}, source, settings);
    require(std::abs(neutral.r - source.r) < 0.001f &&
            std::abs(neutral.g - source.g) < 0.001f &&
            std::abs(neutral.b - source.b) < 0.001f && neutral.a == source.a,
            "neutral framebuffer transfer changed the source texture");
    const auto styled = transfer_to_source_texture(
        {0.5f, 0.5f, 0.5f, 1.0f}, {0.8f, 0.5f, 0.2f, 1.0f}, source, settings);
    require(styled.r > source.r && std::abs(styled.g - source.g) < 0.001f &&
            styled.b < source.b && styled.a == source.a,
            "linear framebuffer transfer did not apply bounded style ratios");

    MaterialTextureBaker baker({.atlas_width=16, .atlas_height=8});
    Image<Color> live(1, 1, {0.5f, 0.5f, 0.5f, 0.9f});
    Image<Color> generated(1, 1, {0.8f, 0.5f, 0.2f, 1.0f});
    SurfaceCorrespondence sample {0, 0, 13, 0.5f, 0.5f, 1.0f};
    sample.source_r = source.r;
    sample.source_g = source.g;
    sample.source_b = source.b;
    sample.source_a = source.a;
    require(has_source_texture_sample(sample),
            "complete source texture sample was not recognized");
    const auto plan = baker.plan(live, std::span(&sample, 1));
    require(baker.commit(plan, generated, std::span(&sample, 1)).accepted == 1,
            "source-aware generated texel was not committed");
    const auto baked = baker.find(13)->sample(0.5f, 0.5f);
    require(baked.r > source.r && baked.b < source.b &&
            std::abs(baked.a - source.a) < 0.001f,
            "baker stored shaded framebuffer color instead of source-texture transfer");
}

static void test_material_baker_handles_sparse_multiple_materials() {
    Image<Color> restyled(4, 1);
    restyled.at(0, 0) = {1.0f, 0.0f, 0.0f, 1.0f};
    restyled.at(1, 0) = {0.0f, 0.0f, 1.0f, 1.0f};
    restyled.at(2, 0) = {0.0f, 1.0f, 0.0f, 1.0f};
    restyled.at(3, 0) = {1.0f, 1.0f, 0.0f, 1.0f};
    const std::array<SurfaceCorrespondence, 4> mapping {{
        {0, 0, 11, 0.25f, 0.5f, 1.0f},
        {1, 0, 22, 0.75f, 0.5f, 1.0f},
        {2, 0, 33, 0.50f, 0.5f, 0.0f},
        {3, 0, 44, std::numeric_limits<float>::quiet_NaN(), 0.5f, 1.0f},
    }};
    MaterialTextureBaker baker({.atlas_width=16, .atlas_height=8});
    const auto stats = baker.update(restyled, mapping);
    require(stats.submitted == 4 && stats.accepted == 2 && stats.materials_touched == 2,
            "multi-material baker reported incorrect update statistics");
    require(baker.material_count() == 2 && baker.find(11) != nullptr && baker.find(22) != nullptr,
            "valid materials were not retained independently");
    require(baker.find(33) == nullptr && baker.find(44) == nullptr,
            "invalid observations created empty material atlases");

    Image<Color> fallback(4, 1, {0.2f, 0.3f, 0.4f, 1.0f});
    const auto reconstructed = baker.reconstruct(fallback, mapping);
    require(reconstructed.at(0, 0).r > reconstructed.at(0, 0).b,
            "first material was not reconstructed");
    require(reconstructed.at(1, 0).b > reconstructed.at(1, 0).r,
            "second material was not reconstructed");
    require(reconstructed.at(2, 0).g == fallback.at(2, 0).g &&
            reconstructed.at(3, 0).g == fallback.at(3, 0).g,
            "uncovered pixels did not retain the live fallback");
}

static void test_uncovered_atlas_never_reconstructs_debug_sentinel() {
    MaterialTextureAtlas atlas(5, 8, 8);
    Image<Color> fallback(1, 1, {0.1f, 0.2f, 0.3f, 1.0f});
    const std::array<SurfaceCorrespondence, 1> mapping {{{0, 0, 5, 0.5f, 0.5f, 1.0f}}};
    const auto reconstructed = reconstruct_from_atlas(fallback, atlas, mapping);
    require(reconstructed.at(0, 0).r == fallback.at(0, 0).r &&
            reconstructed.at(0, 0).b == fallback.at(0, 0).b,
            "unseen atlas texel leaked the magenta debug sentinel");
}

static void test_partial_atlas_sampling_ignores_unseen_neighbors() {
    MaterialTextureAtlas atlas(6, 8, 8);
    const SurfaceCorrespondence observed {0, 0, 6, 0.0f, 0.0f, 1.0f};
    require(atlas.observe({0.0f, 1.0f, 0.0f, 1.0f}, observed),
            "corner atlas observation was rejected");
    const auto edge = atlas.sample(0.05f, 0.0f);
    require(edge.r == 0.0f && edge.g > 0.99f && edge.b == 0.0f,
            "bilinear atlas sampling blended the magenta unseen sentinel");
}

static void test_baker_plans_only_newly_revealed_texels_and_rejects_cut_results() {
    MaterialTextureBaker baker({.atlas_width=16, .atlas_height=8});
    Image<Color> first(2, 1);
    first.at(0, 0) = {1.0f, 0.0f, 0.0f, 1.0f};
    first.at(1, 0) = {0.0f, 0.0f, 1.0f, 1.0f};
    const std::array<SurfaceCorrespondence, 2> mapping {{
        {0, 0, 71, 0.25f, 0.5f, 1.0f},
        {1, 0, 71, 0.75f, 0.5f, 1.0f},
    }};

    const auto initial_plan = baker.plan(first, mapping);
    require(initial_plan.known_pixels == 0 && initial_plan.revealed_pixels == 2 &&
            initial_plan.first_observation_pixels == 2 && initial_plan.inpaint_pixels == 0 &&
            initial_plan.reveal_mask.at(0, 0) == 255 &&
            initial_plan.reveal_mask.at(1, 0) == 255 &&
            initial_plan.first_observation_mask.at(0, 0) == 255 &&
            initial_plan.first_observation_mask.at(1, 0) == 255 &&
            initial_plan.inpaint_mask.at(0, 0) == 0,
            "initial bake plan did not expose only missing material texels");

    MaterialTextureBaker classified_baker({.atlas_width=16, .atlas_height=8});
    Image<TextureRevealClass> causes(2, 1, TextureRevealClass::unknown);
    causes.at(0, 0) = TextureRevealClass::first_observation;
    causes.at(1, 0) = TextureRevealClass::newly_visible;
    const auto classified_plan = classified_baker.plan(first, mapping, &causes);
    require(classified_plan.first_observation_pixels == 1 &&
            classified_plan.inpaint_pixels == 1 &&
            classified_plan.first_observation_mask.at(0, 0) == 255 &&
            classified_plan.first_observation_mask.at(1, 0) == 0 &&
            classified_plan.inpaint_mask.at(0, 0) == 0 &&
            classified_plan.inpaint_mask.at(1, 0) == 255,
            "visibility causes were not separated into styling and inpaint masks");
    auto invalid_plan = classified_plan;
    invalid_plan.inpaint_mask.at(0, 0) = 255;
    require(classified_baker.commit(invalid_plan, first, mapping).stale &&
            classified_baker.material_count() == 0,
            "overlapping reveal masks were accepted as a valid inference result");
    bool rejected_cause_dimensions = false;
    try {
        Image<TextureRevealClass> wrong_size(1, 1, TextureRevealClass::first_observation);
        (void)classified_baker.plan(first, mapping, &wrong_size);
    } catch (const std::invalid_argument &) {
        rejected_cause_dimensions = true;
    }
    require(rejected_cause_dimensions,
            "mismatched reveal classification dimensions were silently accepted");
    const auto initial_commit = baker.commit(initial_plan, first, mapping);
    require(initial_commit.accepted == 2 && !initial_commit.stale,
            "initial revealed texels were not committed");

    const auto stable_plan = baker.plan(Image<Color>(2, 1, {0.0f, 1.0f, 0.0f, 1.0f}), mapping);
    require(stable_plan.known_pixels == 2 && stable_plan.revealed_pixels == 0 &&
            stable_plan.reveal_mask.at(0, 0) == 0 && stable_plan.reveal_mask.at(1, 0) == 0,
            "established atlas texels were scheduled for regeneration");
    require(stable_plan.composite.at(0, 0).r > stable_plan.composite.at(0, 0).g &&
            stable_plan.composite.at(1, 0).b > stable_plan.composite.at(1, 0).g,
            "bake plan did not reproject persistent material colors");

    SurfaceCorrespondence newly_visible {0, 0, 72, 0.5f, 0.5f, 1.0f};
    const auto before_cut = baker.plan(first, std::span(&newly_visible, 1));
    baker.invalidate_in_flight();
    const auto rejected = baker.commit(before_cut, first, std::span(&newly_visible, 1));
    require(rejected.stale && rejected.accepted == 0 && baker.find(72) == nullptr,
            "a pre-cut inpaint result leaked into the persistent texture cache");
    require(baker.find(71) != nullptr,
            "camera-cut invalidation discarded established material atlases");
}

static void test_inpaint_provenance_alpha_and_direct_observation_supersession() {
    MaterialTextureBaker baker({.atlas_width=16, .atlas_height=8});
    Image<Color> live(1, 1, {0.2f, 0.3f, 0.4f, 0.35f});
    const SurfaceCorrespondence sample {0, 0, 81, 0.5f, 0.5f, 1.0f};
    const auto plan = baker.plan(live, std::span(&sample, 1));
    Image<Color> generated(1, 1, {0.9f, 0.1f, 0.2f, 1.0f});
    require(baker.commit(plan, generated, std::span(&sample, 1)).accepted == 1,
            "generated reveal was not committed");
    const auto *atlas = baker.find(81);
    require(atlas != nullptr && atlas->coverage().at(7, 3) == MaterialTextureAtlas::kInpainted,
            "generated reveal was incorrectly marked as a direct observation");
    require(std::abs(atlas->sample(0.5f, 0.5f).a - 0.35f) < 0.001f,
            "generated reveal changed source alpha");

    Image<Color> directly_observed(1, 1, {0.1f, 0.8f, 0.2f, 0.35f});
    require(baker.update(directly_observed, std::span(&sample, 1)).accepted == 1,
            "direct observation did not supersede generated coverage");
    atlas = baker.find(81);
    require(atlas->coverage().at(7, 3) == MaterialTextureAtlas::kObserved &&
            atlas->sample(0.5f, 0.5f).g > atlas->sample(0.5f, 0.5f).r,
            "direct observation did not replace prior inpainted texels");

    const auto second_plan = baker.plan(live, std::span(&sample, 1));
    require(second_plan.revealed_pixels == 0 && second_plan.known_pixels == 1,
            "covered direct observation was scheduled for regeneration");
}

static void test_surface_capture_compacts_backend_neutral_pixels() {
    SurfaceCaptureFrame capture(3, 2);
    capture.pixels().at(0, 0) = {101, 0.25f, 0.75f, 0.4f, 0.4f, 0.9f};
    capture.pixels().at(1, 0) = {0, 0.5f, 0.5f, 0.4f, 0.4f, 1.0f};
    capture.pixels().at(2, 0) = {202, 0.5f, 0.5f, 0.4f,
        std::numeric_limits<float>::quiet_NaN(), 1.0f};
    capture.pixels().at(1, 1) = {303, 0.1f, 0.2f,
        std::numeric_limits<float>::quiet_NaN(),
        std::numeric_limits<float>::quiet_NaN(), 0.75f};
    capture.pixels().at(2, 1) = {404, 0.1f, 0.2f, 0.2f, 0.2f, 0.001f};
    const auto mapping = capture.correspondences(0.01f);
    require(mapping.size() == 2, "surface capture did not reject invalid backend samples");
    require(mapping[0].screen_x == 0 && mapping[0].screen_y == 0 &&
            mapping[0].material_id == 101,
            "surface capture changed the first screen-space correspondence");
    require(mapping[1].screen_x == 1 && mapping[1].screen_y == 1 &&
            mapping[1].material_id == 303,
            "surface capture did not preserve an adapter without optional depth");
}

static void test_scene_transition_preserves_camera_cuts_and_confirms_new_scenes() {
    SceneTransitionTracker tracker({.material_overlap_threshold=0.25f,
                                    .scene_change_confirmation_frames=3});
    const std::array<SurfaceCorrespondence, 2> room {{
        {0, 0, 1001, 0.1f, 0.1f, 1.0f},
        {1, 0, 1002, 0.2f, 0.2f, 1.0f},
    }};
    require(tracker.observe(false, room) == SceneTransition::stable,
            "first scene was not initialized as stable");
    tracker.set_scene_identity(0x1234);
    const auto room_key = tracker.scene_key();
    require(room_key.identity == 0x1234,
            "persistent scene identity was not attached to the generation token");
    require(tracker.observe(true, room) == SceneTransition::camera_cut,
            "visual cut with shared materials was not classified as a camera cut");
    require(tracker.scene_key() == room_key,
            "same-scene camera cut changed the scene generation");

    const std::array<SurfaceCorrespondence, 2> other {{
        {0, 0, 9001, 0.1f, 0.1f, 1.0f},
        {1, 0, 9002, 0.2f, 0.2f, 1.0f},
    }};
    require(tracker.observe(true, other) == SceneTransition::pending_scene_change,
            "foreign cut was not quarantined before confirmation");
    require(tracker.observe(false, other) == SceneTransition::pending_scene_change,
            "foreign scene left quarantine before confirmation");
    require(tracker.observe(false, other) == SceneTransition::scene_change,
            "sustained foreign materials did not confirm a scene change");
    require(tracker.scene_key() != room_key,
            "confirmed new scene retained the old scene generation");

    SceneTransitionTracker recovered({.material_overlap_threshold=0.25f,
                                      .scene_change_confirmation_frames=3});
    require(recovered.observe(false, room) == SceneTransition::stable,
            "recovery fixture did not establish its scene");
    require(recovered.observe(true, other) == SceneTransition::pending_scene_change,
            "recovery fixture did not enter quarantine");
    require(recovered.observe(false, room) == SceneTransition::camera_cut,
            "returning same-scene materials did not resolve as a camera cut");

    SceneTransitionTracker manual({.material_overlap_threshold=0.25f,
                                   .scene_change_confirmation_frames=3});
    require(manual.observe(false, room) == SceneTransition::stable,
            "manual-control fixture did not establish its scene");
    const auto manual_room_key = manual.scene_key();
    require(manual.observe(true, other) == SceneTransition::pending_scene_change,
            "manual-control fixture did not enter quarantine");
    manual.keep_current_scene(other);
    require(manual.scene_key() != manual_room_key &&
            manual.observe(false, other) == SceneTransition::stable,
            "keep-current control did not accept quarantined evidence");
    const auto kept_key = manual.scene_key();
    manual.start_new_scene(room);
    require(manual.scene_key() != kept_key && manual.scene_key().identity == 0 &&
            manual.observe(false, room) == SceneTransition::stable,
            "start-new control did not force a fresh scene generation");

    SceneTransitionTracker no_geometry;
    const auto before_reset = no_geometry.scene_key();
    require(no_geometry.observe(true, {}) == SceneTransition::camera_cut,
            "capture-less visual cut was not handled conservatively");
    no_geometry.reset();
    require(no_geometry.scene_key() != before_reset,
            "explicit scene reset did not advance the scene generation");
}

static void test_scene_transition_combines_material_uv_depth_and_color_evidence() {
    const auto sample = [](std::uint64_t material, float u, float depth,
                           Color source) {
        SurfaceCorrespondence result;
        result.material_id = material;
        result.u = u;
        result.v = u;
        result.confidence = 1.0f;
        result.framebuffer_depth = depth;
        result.hit_depth = depth;
        result.source_r = source.r;
        result.source_g = source.g;
        result.source_b = source.b;
        result.source_a = source.a;
        return result;
    };

    const auto original = sample(7001, 0.1f, 0.2f, {0.8f, 0.1f, 0.1f, 1.0f});
    const auto corroborated = sample(7001, 0.8f, 0.8f, {0.8f, 0.1f, 0.1f, 1.0f});
    SceneTransitionTracker same_binding;
    require(same_binding.observe(false, std::span(&original, 1)) == SceneTransition::stable &&
            same_binding.observe(true, std::span(&corroborated, 1)) ==
                SceneTransition::camera_cut,
            "source-color evidence did not preserve a one-material camera cut");

    const auto conflicting = sample(7001, 0.8f, 0.8f, {0.1f, 0.1f, 0.8f, 1.0f});
    SceneTransitionTracker reused_binding;
    require(reused_binding.observe(false, std::span(&original, 1)) == SceneTransition::stable &&
            reused_binding.observe(true, std::span(&conflicting, 1)) ==
                SceneTransition::pending_scene_change,
            "a reused material identity overrode conflicting UV/depth/color evidence");

    const std::array<SurfaceCorrespondence, 2> original_pair {{
        sample(7101, 0.1f, 0.2f, {0.8f, 0.1f, 0.1f, 1.0f}),
        sample(7102, 0.2f, 0.3f, {0.1f, 0.8f, 0.1f, 1.0f}),
    }};
    const std::array<SurfaceCorrespondence, 2> moved_pair {{
        sample(7101, 0.7f, 0.8f, {0.1f, 0.1f, 0.8f, 1.0f}),
        sample(7102, 0.8f, 0.9f, {0.7f, 0.7f, 0.1f, 1.0f}),
    }};
    SceneTransitionTracker strong_geometry;
    require(strong_geometry.observe(false, original_pair) == SceneTransition::stable &&
            strong_geometry.observe(true, moved_pair) == SceneTransition::camera_cut,
            "multiple stable binding/material/geometry identities did not preserve the scene");

    const SurfaceCorrespondence legacy {0, 0, 7201, 0.2f, 0.2f, 1.0f};
    SceneTransitionTracker optional_channels;
    require(optional_channels.observe(false, std::span(&legacy, 1)) ==
                SceneTransition::stable &&
            optional_channels.observe(true, std::span(&legacy, 1)) ==
                SceneTransition::camera_cut,
            "missing optional scene evidence did not retain conservative compatibility");
}

static void test_same_scene_camera_cut_preserves_atlas_and_reveals_new_uvs() {
    MaterialTextureBaker baker({.atlas_width=32, .atlas_height=16});
    SceneTransitionTracker tracker({.material_overlap_threshold=0.25f,
                                    .scene_change_confirmation_frames=3});
    Image<Color> styled(2, 1);
    styled.at(0, 0) = {0.9f, 0.1f, 0.1f, 1.0f};
    styled.at(1, 0) = {0.1f, 0.2f, 0.9f, 1.0f};
    const std::array<SurfaceCorrespondence, 1> first {{
        {0, 0, 5001, 0.15f, 0.5f, 1.0f},
    }};
    require(tracker.observe(false, first) == SceneTransition::stable,
            "camera-cut fixture did not establish its scene");
    const auto first_plan = baker.plan(styled, first);
    require(baker.commit(first_plan, styled, first).accepted == 1,
            "camera-cut fixture did not establish atlas coverage");

    const std::array<SurfaceCorrespondence, 2> changed_angle {{
        {0, 0, 5001, 0.15f, 0.5f, 1.0f},
        {1, 0, 5001, 0.85f, 0.5f, 1.0f},
    }};
    require(tracker.observe(true, changed_angle) == SceneTransition::camera_cut,
            "shared material identity was not retained across a camera cut");
    baker.invalidate_in_flight();
    const auto changed_plan = baker.plan(styled, changed_angle);
    require(changed_plan.known_pixels == 1 && changed_plan.revealed_pixels == 1 &&
            changed_plan.first_observation_pixels == 0 &&
            changed_plan.inpaint_pixels == 1 &&
            changed_plan.reveal_mask.at(0, 0) == 0 &&
            changed_plan.reveal_mask.at(1, 0) == 255 &&
            changed_plan.inpaint_mask.at(1, 0) == 255,
            "camera cut did not preserve known UVs and reveal only the new angle");
    require(baker.commit(changed_plan, styled, changed_angle).accepted == 1,
            "newly visible same-scene UV was not accepted after a camera cut");
}

static void test_pending_scene_quarantine_cannot_commit_texture_data() {
    MaterialTextureBaker baker({.atlas_width=16, .atlas_height=8});
    SceneTransitionTracker tracker({.material_overlap_threshold=0.5f,
                                    .scene_change_confirmation_frames=3});
    Image<Color> frame(1, 1, {0.7f, 0.2f, 0.1f, 1.0f});
    const SurfaceCorrespondence original {0, 0, 6101, 0.25f, 0.5f, 1.0f};
    require(tracker.observe(false, std::span(&original, 1)) == SceneTransition::stable,
            "quarantine fixture did not establish its scene");
    const auto old_request = baker.plan(frame, std::span(&original, 1));

    const SurfaceCorrespondence foreign {0, 0, 9901, 0.75f, 0.5f, 1.0f};
    require(tracker.observe(true, std::span(&foreign, 1)) ==
                SceneTransition::pending_scene_change,
            "foreign scene did not enter quarantine");
    baker.invalidate_in_flight();
    const auto old_commit = baker.commit(old_request, frame, std::span(&original, 1));
    require(old_commit.stale && old_commit.accepted == 0 && baker.find(6101) == nullptr,
            "pre-quarantine inference committed after an ambiguous cut");

    const std::span<const SurfaceCorrespondence> quarantined {};
    const auto pending_plan = baker.plan(frame, quarantined);
    require(pending_plan.revealed_pixels == 0 &&
            baker.commit(pending_plan, frame, quarantined).accepted == 0 &&
            baker.find(9901) == nullptr,
            "quarantined foreign observation mutated texture coverage");
}

static void test_atlas_cache_round_trip_and_rejects_corruption() {
    MaterialTextureBaker source({.atlas_width=16, .atlas_height=8});
    Image<Color> styled(2, 1);
    styled.at(0, 0) = {0.9f, 0.1f, 0.2f, 1.0f};
    styled.at(1, 0) = {0.1f, 0.2f, 0.9f, 1.0f};
    const std::array<SurfaceCorrespondence, 2> mapping {{
        {0, 0, 0x1234, 0.25f, 0.5f, 1.0f},
        {1, 0, 0x1234, 0.75f, 0.5f, 1.0f},
    }};
    require(source.update(styled, mapping).accepted == 2,
            "cache fixture did not paint its atlas");

    const auto unique = std::to_string(std::chrono::steady_clock::now()
        .time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("neuralpass-atlas-test-" + unique);
    const std::array<std::uint64_t, 1> stable {0x1234};
    const auto saved = source.save_cache(directory, stable);
    require(saved.saved == 1 && saved.rejected == 0,
            "restart-stable atlas was not saved");

    MaterialTextureBaker restored({.atlas_width=16, .atlas_height=8});
    const auto loaded = restored.load_cache(directory);
    require(loaded.loaded == 1 && restored.find(0x1234) != nullptr,
            "saved atlas did not survive a cache round trip");
    const auto reconstructed = restored.reconstruct(
        Image<Color>(2, 1, {0.0f, 1.0f, 0.0f, 1.0f}), mapping);
    require(reconstructed.at(0, 0).r > reconstructed.at(0, 0).g &&
            reconstructed.at(1, 0).b > reconstructed.at(1, 0).g,
            "loaded atlas changed its observed colors");

    const auto corrupt = directory / "corrupt.npatlas";
    {
        std::ofstream output(corrupt, std::ios::binary);
        output << "NPATL01";
    }
    MaterialTextureBaker verifier({.atlas_width=16, .atlas_height=8});
    const auto checked = verifier.load_cache(directory);
    require(checked.loaded == 1 && checked.rejected == 1,
            "cache loader did not isolate a corrupt snapshot");

    std::error_code error;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error))
        std::filesystem::remove(entry.path(), error);
    std::filesystem::remove(directory, error);
}

static void test_scene_cache_catalog_matches_returning_views_and_isolates_scenes() {
    const auto unique = std::to_string(std::chrono::steady_clock::now()
        .time_since_epoch().count());
    const auto directory = std::filesystem::temp_directory_path() /
        ("neuralpass-scene-test-" + unique);
    SceneCacheCatalog catalog(directory, 0.5f);
    const std::array<std::uint64_t, 3> room {101, 102, 103};
    const auto created = catalog.resolve(room);
    require(created.valid() && !created.matched_existing && catalog.record(created.identity, room),
            "scene cache did not create its first persistent namespace");

    const std::array<std::uint64_t, 2> another_room_view {102, 104};
    const auto returned = catalog.resolve(another_room_view);
    require(returned.matched_existing && returned.identity == created.identity,
            "returning scene view did not match its persistent namespace");
    require(catalog.record(returned.identity, another_room_view),
            "scene cache did not accumulate a returning view");
    const std::array<std::uint64_t, 1> accumulated_view {104};
    require(catalog.resolve(accumulated_view).identity == created.identity,
            "accumulated scene binding was not available for later matching");

    const std::array<std::uint64_t, 2> foreign {9001, 9002};
    const auto isolated = catalog.resolve(foreign);
    require(isolated.valid() && !isolated.matched_existing && isolated.identity != created.identity,
            "foreign scene reused an unrelated cache namespace");
    require(catalog.record(isolated.identity, foreign),
            "foreign scene namespace was not recorded");
    const auto forced = catalog.create_new(room, 0xfeedbeef);
    require(forced.valid() && !forced.matched_existing &&
            forced.identity != created.identity,
            "manual start-new did not force a distinct scene namespace");
    require(catalog.create_new(room, 0xfeedbeef).identity == forced.identity &&
            catalog.create_new(room, 0xfeedbef0).identity != forced.identity,
            "forced scene namespace discriminator is not deterministic");

    {
        std::ofstream corrupt(isolated.directory / "materials-corrupt.npscene", std::ios::binary);
        corrupt << "NPSCNO1";
    }
    require(catalog.resolve(foreign).identity == isolated.identity,
            "corrupt scene manifest hid the newest valid generation");
    require(!catalog.resolve({}).valid(),
            "empty/session-only evidence created a persistent scene namespace");
    std::error_code error;
    std::filesystem::remove_all(directory, error);
}

static void test_cache_namespace_isolates_game_style_and_model() {
    const std::filesystem::path root = "cache-root";
    const std::filesystem::path scene = "scene-root";
    const CacheNamespace baseline {0x11, 0x22, 0x33};
    require(baseline.scene_catalog_root(root) ==
                root / "game-0000000000000011" / "scenes",
            "game build was not encoded into the scene catalog namespace");
    require(baseline.atlas_root(scene) ==
                scene / "styles" / "style-0000000000000022" /
                "model-0000000000000033" / "atlases",
            "style/model atlas namespace is not deterministic");
    require(CacheNamespace{0x12, 0x22, 0x33}.scene_catalog_root(root) !=
                baseline.scene_catalog_root(root),
            "different game builds shared a scene catalog");
    require(CacheNamespace{0x11, 0x23, 0x33}.atlas_root(scene) !=
                baseline.atlas_root(scene),
            "different styles shared an atlas namespace");
    require(CacheNamespace{0x11, 0x22, 0x34}.atlas_root(scene) !=
                baseline.atlas_root(scene),
            "different models shared an atlas namespace");
    bool rejected = false;
    try {
        (void)CacheNamespace{}.atlas_root(scene);
    } catch (const std::invalid_argument &) {
        rejected = true;
    }
    require(rejected, "zero cache namespace identity was accepted");
}

int main() {
    try {
        test_reprojection_accepts_stable_pixels();
        test_disocclusion_rejected();
        test_motion_reprojects_previous_pixel();
        test_dilation_and_priority();
        test_age_refresh();
        test_newly_visible_tiles_preempt_initial_styling();
        test_preview_backend_is_bounded();
        test_hdr_color_contract_is_bounded_and_luminance_stable();
        test_runtime_display_evidence_json_is_exact_and_escaped();
        test_binding_identity_is_pipeline_and_slot_specific();
        test_depth_pyramid_and_conservative_raymarch();
        test_visibility_classifies_newly_revealed_causes();
        test_texture_baker_splats_inpaints_and_reconstructs();
        test_texture_baker_locks_observed_texels_and_rejects_bad_depth();
        test_elliptical_uv_splat_uses_gradients_and_confidence();
        test_mips_do_not_expand_texture_coverage();
        test_linear_source_texture_transfer_and_alpha();
        test_material_baker_handles_sparse_multiple_materials();
        test_uncovered_atlas_never_reconstructs_debug_sentinel();
        test_partial_atlas_sampling_ignores_unseen_neighbors();
        test_baker_plans_only_newly_revealed_texels_and_rejects_cut_results();
        test_inpaint_provenance_alpha_and_direct_observation_supersession();
        test_surface_capture_compacts_backend_neutral_pixels();
        test_scene_transition_preserves_camera_cuts_and_confirms_new_scenes();
        test_scene_transition_combines_material_uv_depth_and_color_evidence();
        test_same_scene_camera_cut_preserves_atlas_and_reveals_new_uvs();
        test_pending_scene_quarantine_cannot_commit_texture_data();
        test_atlas_cache_round_trip_and_rejects_corruption();
        test_scene_cache_catalog_matches_returning_views_and_isolates_scenes();
        test_cache_namespace_isolates_game_style_and_model();
        std::cout << "NeuralPass core tests passed\n";
        return EXIT_SUCCESS;
    } catch (const std::exception &error) {
        std::cerr << "FAIL: " << error.what() << '\n';
        return EXIT_FAILURE;
    }
}
