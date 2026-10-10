#pragma once

#include "surface_capture_backend.hpp"

#include <reshade.hpp>

namespace neuralpass::vulkan_capture {

class SurfaceCapture final : public capture::SurfaceCaptureBackend {
public:
    SurfaceCapture();
    ~SurfaceCapture() override;
    SurfaceCapture(const SurfaceCapture &) = delete;
    SurfaceCapture &operator=(const SurfaceCapture &) = delete;

    void attach(reshade::api::device *device, reshade::api::command_queue *queue);
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
    [[nodiscard]] bool replay(void *command_list, const capture::UvInput &uv,
                              std::uint64_t material_id,
                              const capture::DrawCommand &draw,
                              int source_texture_override = -1) override;
    [[nodiscard]] std::optional<SurfaceCaptureFrame> finish_frame(
        void *command_list, bool schedule_next = true) override;
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

} // namespace neuralpass::vulkan_capture
