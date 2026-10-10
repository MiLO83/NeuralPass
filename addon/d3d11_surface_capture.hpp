#pragma once

#include "surface_capture_backend.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d11.h>

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace neuralpass::d3d11_capture {

using UvSemantic = capture::UvInput;
using ReplacementMip = capture::ReplacementMip;

// Reads the vertex shader output signature and selects the first two-component
// (or wider) TEXCOORD interpolant. This is the value rasterization actually
// feeds to the pixel shader, rather than an untransformed vertex-buffer guess.
[[nodiscard]] UvSemantic inspect_uv_output(const void *bytecode, std::size_t size);

class SurfaceCapture final : public capture::SurfaceCaptureBackend {
public:
    SurfaceCapture() = default;
    ~SurfaceCapture();
    SurfaceCapture(const SurfaceCapture &) = delete;
    SurfaceCapture &operator=(const SurfaceCapture &) = delete;

    [[nodiscard]] capture::GraphicsBackend backend() const noexcept override;
    [[nodiscard]] capture::CaptureCapabilities capabilities() const noexcept override;
    [[nodiscard]] bool initialize(void *native_device, std::uint32_t width,
                                  std::uint32_t height) override;
    [[nodiscard]] bool initialize(ID3D11Device *device, std::uint32_t width,
                                  std::uint32_t height);
    void reset() override;
    [[nodiscard]] bool replay(void *native_command_list, const UvSemantic &uv,
                              std::uint64_t material_id,
                              const capture::DrawCommand &draw,
                              int source_texture_override = -1) override;

    // Called from ReShade's pre-draw event. On success this executes the game
    // draw exactly once, replays it into the UV surface, restores all touched
    // D3D11 state, and returns true so ReShade suppresses the wrapper's draw.
    [[nodiscard]] bool draw(ID3D11DeviceContext *context, const UvSemantic &uv,
                            std::uint64_t material_id, std::uint32_t vertex_count,
                            std::uint32_t instance_count, std::uint32_t first_vertex,
                            std::uint32_t first_instance,
                            int source_texture_override = -1);
    [[nodiscard]] bool draw_indexed(ID3D11DeviceContext *context, const UvSemantic &uv,
                                    std::uint64_t material_id, std::uint32_t index_count,
                                    std::uint32_t instance_count, std::uint32_t first_index,
                                    std::int32_t vertex_offset,
                                    std::uint32_t first_instance,
                                    int source_texture_override = -1);
    [[nodiscard]] bool draw_indirect(ID3D11DeviceContext *context, const UvSemantic &uv,
                                     std::uint64_t material_id, ID3D11Buffer *arguments,
                                     std::uint32_t argument_offset,
                                     int source_texture_override = -1);
    [[nodiscard]] bool draw_indexed_indirect(
        ID3D11DeviceContext *context, const UvSemantic &uv,
        std::uint64_t material_id, ID3D11Buffer *arguments,
        std::uint32_t argument_offset, int source_texture_override = -1);

    // Polls completed staging copies without flushing or waiting, then queues
    // the current surface into a free ring slot and clears it for the next frame.
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(
        void *native_command_list, bool schedule_next = true) override;
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(
        ID3D11DeviceContext *context, bool schedule_next = true);

    [[nodiscard]] std::uint64_t replayed_draws() const noexcept;
    [[nodiscard]] std::uint64_t dropped_frames() const noexcept;
    [[nodiscard]] std::uint64_t replacement_draws() const noexcept;
    [[nodiscard]] std::uint64_t rejected_replacements() const noexcept;
    [[nodiscard]] capture::CaptureStatistics statistics() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    void queue_replacement(std::uint64_t material_id,
                           std::vector<ReplacementMip> mips) override;
    void clear_replacements() override;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace neuralpass::d3d11_capture
