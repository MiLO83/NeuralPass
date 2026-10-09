#pragma once

#include "neuralpass/surface_capture.hpp"

#include <cstdint>
#include <array>
#include <limits>
#include <optional>
#include <string>
#include <vector>

namespace neuralpass::capture {

enum class GraphicsBackend : std::uint8_t {
    d3d9,
    d3d10,
    d3d11,
    d3d12,
    vulkan,
};

enum class DrawKind : std::uint8_t {
    direct,
    indexed,
    indirect,
    indexed_indirect,
};

struct UvInput {
    std::string name;
    std::uint32_t index = 0;
    std::uint32_t register_index = 0;
    std::uint32_t location = std::numeric_limits<std::uint32_t>::max();

    [[nodiscard]] bool valid() const noexcept {
        return !name.empty() || location != std::numeric_limits<std::uint32_t>::max();
    }
};

struct DrawCommand {
    DrawKind kind = DrawKind::direct;
    std::uint32_t vertex_or_index_count = 0;
    std::uint32_t instance_count = 1;
    std::uint32_t first_vertex_or_index = 0;
    std::int32_t vertex_offset = 0;
    std::uint32_t first_instance = 0;
    void *argument_buffer = nullptr;
    std::uint64_t argument_offset = 0;
    std::uint32_t draw_count = 1;
    std::uint32_t argument_stride = 0;
    // API-neutral snapshot of the graphics state required by explicit backends.
    // Legacy immediate-context adapters may ignore these opaque handles.
    std::uint64_t pipeline = 0;
    std::uint64_t pipeline_layout = 0;
    std::array<std::uint64_t, 8> render_target_views {};
    std::uint32_t render_target_count = 0;
    std::uint64_t depth_stencil_view = 0;
    bool target_compatible = false;
    bool inside_render_pass = false;
    // Pixel-shader source selected by the add-on's descriptor tracker. Explicit
    // APIs need the register space as well as the register index when compiling
    // a companion capture shader. A zero view means source color is unavailable.
    std::uint64_t source_view = 0;
    std::uint32_t source_register = 0;
    std::uint32_t source_space = 0;
    std::uint32_t sampler_register = 0;
    std::uint32_t sampler_space = 0;
    bool source_sampleable = false;
};

struct ReplacementMip {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
    std::vector<std::uint8_t> coverage;
};

struct CaptureCapabilities {
    bool direct_draws = false;
    bool indexed_draws = false;
    bool indirect_draws = false;
    bool replacement_textures = false;
    bool asynchronous_readback = false;
    bool shader_coverage_preserved = false;
};

struct CaptureStatistics {
    std::uint64_t replayed_draws = 0;
    std::uint64_t dropped_frames = 0;
    std::uint64_t replacement_draws = 0;
    std::uint64_t rejected_replacements = 0;
};

class SurfaceCaptureBackend {
public:
    virtual ~SurfaceCaptureBackend() = default;

    [[nodiscard]] virtual GraphicsBackend backend() const noexcept = 0;
    [[nodiscard]] virtual CaptureCapabilities capabilities() const noexcept = 0;
    [[nodiscard]] virtual bool initialize(void *native_device, std::uint32_t width,
                                          std::uint32_t height) = 0;
    virtual void reset() = 0;
    [[nodiscard]] virtual bool replay(void *native_command_list, const UvInput &uv,
                                      std::uint64_t material_id,
                                      const DrawCommand &draw,
                                      int source_texture_override = -1) = 0;
    [[nodiscard]] virtual std::optional<SurfaceCaptureFrame> finish_frame(
        void *native_command_list) = 0;
    [[nodiscard]] virtual CaptureStatistics statistics() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t width() const noexcept = 0;
    [[nodiscard]] virtual std::uint32_t height() const noexcept = 0;
    virtual void queue_replacement(std::uint64_t material_id,
                                   std::vector<ReplacementMip> mips) = 0;
    virtual void clear_replacements() = 0;
};

} // namespace neuralpass::capture
