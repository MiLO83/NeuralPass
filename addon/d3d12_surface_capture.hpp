#pragma once

#include "surface_capture_backend.hpp"

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <d3d12.h>
#include <reshade.hpp>

#include <array>

namespace neuralpass::d3d12_capture {

class SurfaceCapture final : public capture::SurfaceCaptureBackend {
public:
    SurfaceCapture();
    ~SurfaceCapture();
    SurfaceCapture(const SurfaceCapture &) = delete;
    SurfaceCapture &operator=(const SurfaceCapture &) = delete;

    void attach(reshade::api::device *device, reshade::api::command_queue *queue);
    void attach_native_queue(ID3D12CommandQueue *queue);
    [[nodiscard]] bool signal_submitted();
    void register_pipeline(reshade::api::pipeline_layout layout,
                           std::uint32_t subobject_count,
                           const reshade::api::pipeline_subobject *subobjects,
                           reshade::api::pipeline pipeline);
    void unregister_pipeline(reshade::api::pipeline pipeline);
    [[nodiscard]] capture::GraphicsBackend backend() const noexcept override;
    [[nodiscard]] capture::CaptureCapabilities capabilities() const noexcept override;
    [[nodiscard]] bool initialize(void *native_device, std::uint32_t width,
                                  std::uint32_t height) override;
    void reset() override;
    [[nodiscard]] bool replay(void *native_command_list, const capture::UvInput &uv,
                              std::uint64_t material_id,
                              const capture::DrawCommand &draw,
                              int source_texture_override = -1) override;
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(
        void *native_command_list) override;
    [[nodiscard]] capture::CaptureStatistics statistics() const noexcept override;
    [[nodiscard]] std::uint32_t width() const noexcept override;
    [[nodiscard]] std::uint32_t height() const noexcept override;
    void queue_replacement(std::uint64_t material_id,
                           std::vector<capture::ReplacementMip> mips) override;
    void clear_replacements() override;

private:
    struct Impl;
    Impl *impl_ = nullptr;
};

} // namespace neuralpass::d3d12_capture
