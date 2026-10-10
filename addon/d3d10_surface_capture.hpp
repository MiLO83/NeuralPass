#pragma once

#include "surface_capture_backend.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d10_1.h>

#include <cstdint>
#include <optional>
#include <vector>

namespace neuralpass::d3d10_capture {

using UvSemantic = capture::UvInput;
using ReplacementMip = capture::ReplacementMip;

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
    [[nodiscard]] bool initialize(ID3D10Device *device, std::uint32_t width,
                                  std::uint32_t height);
    void reset() override;
    [[nodiscard]] bool replay(void *native_command_list, const UvSemantic &uv,
                              std::uint64_t material_id,
                              const capture::DrawCommand &draw,
                              int source_texture_override = -1) override;
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(
        void *native_command_list, bool schedule_next = true) override;
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(bool schedule_next = true);
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

} // namespace neuralpass::d3d10_capture
