#pragma once

#include "neuralpass/types.hpp"

#include <cstdint>

namespace neuralpass {

// The display encoding is established from both the resource format and the
// swapchain color space. Callers must not infer HDR10 from RGB10A2 alone.
enum class DisplayEncoding : std::uint8_t {
    sdr_srgb,
    scrgb_linear,
    hdr10_pq,
};

enum class CapturePixelLayout : std::uint8_t {
    rgba8,
    bgra8,
    rgba16_float,
    rgb10a2,
};

[[nodiscard]] float half_to_float(std::uint16_t value) noexcept;

// Converts one native backbuffer pixel into the bounded, display-referred sRGB
// input expected by the inference backends.
[[nodiscard]] Color decode_capture_pixel(const std::uint8_t *pixel,
    CapturePixelLayout layout, DisplayEncoding encoding) noexcept;

// Reference implementation of the compositor's color transform. HDR output
// keeps the source scene luminance while taking chroma/contrast from the styled
// SDR result. The shader implements the same contract on the GPU.
[[nodiscard]] Color composite_styled_pixel(const Color &source_encoded,
    const Color &styled_srgb, DisplayEncoding encoding) noexcept;

} // namespace neuralpass
