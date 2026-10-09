#include "neuralpass/color_pipeline.hpp"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstring>

namespace neuralpass {
namespace {

float finite_nonnegative(float value) noexcept {
    return std::isfinite(value) ? std::max(value, 0.0f) : 0.0f;
}

float srgb_to_linear(float value) noexcept {
    value = std::clamp(finite_nonnegative(value), 0.0f, 1.0f);
    return value <= 0.04045f ? value / 12.92f
                            : std::pow((value + 0.055f) / 1.055f, 2.4f);
}

float linear_to_srgb(float value) noexcept {
    value = std::clamp(finite_nonnegative(value), 0.0f, 1.0f);
    return value <= 0.0031308f ? value * 12.92f
                              : 1.055f * std::pow(value, 1.0f / 2.4f) - 0.055f;
}

float aces_fitted(float value) noexcept {
    value = finite_nonnegative(value);
    return std::clamp((value * (2.51f * value + 0.03f)) /
        (value * (2.43f * value + 0.59f) + 0.14f), 0.0f, 1.0f);
}

Color bt2020_to_bt709(Color c) noexcept {
    return {
        1.660491f*c.r - 0.587641f*c.g - 0.072850f*c.b,
       -0.124550f*c.r + 1.132900f*c.g - 0.008349f*c.b,
       -0.018151f*c.r - 0.100579f*c.g + 1.118730f*c.b, c.a};
}

Color bt709_to_bt2020(Color c) noexcept {
    return {
        0.627404f*c.r + 0.329283f*c.g + 0.043313f*c.b,
        0.069097f*c.r + 0.919540f*c.g + 0.011362f*c.b,
        0.016391f*c.r + 0.088013f*c.g + 0.895595f*c.b, c.a};
}

float pq_to_linear(float value) noexcept {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float p = std::pow(std::clamp(finite_nonnegative(value), 0.0f, 1.0f), 1.0f / m2);
    return std::pow(std::max(p - c1, 0.0f) / std::max(c2 - c3*p, 1.0e-6f), 1.0f / m1);
}

float linear_to_pq(float value) noexcept {
    constexpr float m1 = 2610.0f / 16384.0f;
    constexpr float m2 = 2523.0f / 32.0f;
    constexpr float c1 = 3424.0f / 4096.0f;
    constexpr float c2 = 2413.0f / 128.0f;
    constexpr float c3 = 2392.0f / 128.0f;
    const float p = std::pow(std::clamp(finite_nonnegative(value), 0.0f, 1.0f), m1);
    return std::pow((c1 + c2*p) / (1.0f + c3*p), m2);
}

Color scene_linear_709(const Color &source, DisplayEncoding encoding) noexcept {
    if (encoding == DisplayEncoding::scrgb_linear)
        return {finite_nonnegative(source.r), finite_nonnegative(source.g),
                finite_nonnegative(source.b), source.a};
    if (encoding == DisplayEncoding::hdr10_pq) {
        // PQ is normalized to 10,000 nits; scRGB scene units use 80 nits.
        return bt2020_to_bt709({pq_to_linear(source.r) * 125.0f,
                                pq_to_linear(source.g) * 125.0f,
                                pq_to_linear(source.b) * 125.0f, source.a});
    }
    return {srgb_to_linear(source.r), srgb_to_linear(source.g),
            srgb_to_linear(source.b), source.a};
}

Color scene_to_inference(Color scene) noexcept {
    return {linear_to_srgb(aces_fitted(scene.r)),
            linear_to_srgb(aces_fitted(scene.g)),
            linear_to_srgb(aces_fitted(scene.b)), scene.a};
}

float luma709(const Color &c) noexcept {
    return std::max(0.0f, 0.2126f*c.r + 0.7152f*c.g + 0.0722f*c.b);
}

} // namespace

float half_to_float(std::uint16_t value) noexcept {
    const std::uint32_t sign = static_cast<std::uint32_t>(value & 0x8000u) << 16;
    const std::uint32_t exponent = (value >> 10) & 0x1fu;
    const std::uint32_t mantissa = value & 0x03ffu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            std::uint32_t m = mantissa;
            int shift = 0;
            while ((m & 0x0400u) == 0) { m <<= 1; ++shift; }
            bits = sign | static_cast<std::uint32_t>(127 - 14 - shift) << 23 |
                   (m & 0x03ffu) << 13;
        }
    } else if (exponent == 0x1fu) {
        bits = sign | 0x7f800000u | mantissa << 13;
    } else {
        bits = sign | (exponent + 112u) << 23 | mantissa << 13;
    }
    return std::bit_cast<float>(bits);
}

Color decode_capture_pixel(const std::uint8_t *pixel, CapturePixelLayout layout,
                           DisplayEncoding encoding) noexcept {
    Color encoded {};
    if (layout == CapturePixelLayout::rgba16_float) {
        std::uint16_t words[4] {};
        std::memcpy(words, pixel, sizeof(words));
        encoded = {half_to_float(words[0]), half_to_float(words[1]),
                   half_to_float(words[2]), half_to_float(words[3])};
    } else if (layout == CapturePixelLayout::rgb10a2) {
        std::uint32_t packed = 0;
        std::memcpy(&packed, pixel, sizeof(packed));
        encoded = {static_cast<float>( packed        & 1023u) / 1023.0f,
                   static_cast<float>((packed >> 10) & 1023u) / 1023.0f,
                   static_cast<float>((packed >> 20) & 1023u) / 1023.0f,
                   static_cast<float>((packed >> 30) &    3u) / 3.0f};
    } else {
        const bool bgra = layout == CapturePixelLayout::bgra8;
        encoded = {pixel[bgra ? 2 : 0] / 255.0f, pixel[1] / 255.0f,
                   pixel[bgra ? 0 : 2] / 255.0f, pixel[3] / 255.0f};
    }
    if (encoding == DisplayEncoding::sdr_srgb) return encoded;
    return scene_to_inference(scene_linear_709(encoded, encoding));
}

Color composite_styled_pixel(const Color &source_encoded, const Color &styled_srgb,
                             DisplayEncoding encoding) noexcept {
    if (encoding == DisplayEncoding::sdr_srgb)
        return {std::clamp(styled_srgb.r, 0.0f, 1.0f),
                std::clamp(styled_srgb.g, 0.0f, 1.0f),
                std::clamp(styled_srgb.b, 0.0f, 1.0f), source_encoded.a};

    const Color scene = scene_linear_709(source_encoded, encoding);
    const Color inference_source = scene_to_inference(scene);
    const Color inference_linear {srgb_to_linear(inference_source.r),
                                  srgb_to_linear(inference_source.g),
                                  srgb_to_linear(inference_source.b), 1.0f};
    const Color styled_linear {srgb_to_linear(styled_srgb.r), srgb_to_linear(styled_srgb.g),
                               srgb_to_linear(styled_srgb.b), 1.0f};
    const float styled_luma = luma709(styled_linear);
    const float ratio = std::clamp(styled_luma / std::max(luma709(inference_linear), 1.0e-4f),
                                   0.0f, 4.0f);
    const float target_luma = luma709(scene) * ratio;
    Color output_scene {};
    if (styled_luma > 1.0e-5f) {
        const float scale = target_luma / styled_luma;
        output_scene = {styled_linear.r*scale, styled_linear.g*scale,
                        styled_linear.b*scale, source_encoded.a};
    }
    if (encoding == DisplayEncoding::scrgb_linear) return output_scene;
    const Color bt2020 = bt709_to_bt2020(output_scene);
    return {linear_to_pq(bt2020.r / 125.0f), linear_to_pq(bt2020.g / 125.0f),
            linear_to_pq(bt2020.b / 125.0f), source_encoded.a};
}

} // namespace neuralpass
