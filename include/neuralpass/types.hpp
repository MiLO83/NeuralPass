#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <stdexcept>
#include <vector>

namespace neuralpass {

struct Color {
    float r = 0.0f;
    float g = 0.0f;
    float b = 0.0f;
    float a = 1.0f;
};

struct Motion {
    // Current-pixel to previous-frame displacement in pixels.
    float x = 0.0f;
    float y = 0.0f;
};

template <typename T> class Image {
public:
    Image() = default;
    Image(std::uint32_t width, std::uint32_t height, const T &value = {})
        : width_(width), height_(height), pixels_(checked_size(width, height), value) {}

    [[nodiscard]] std::uint32_t width() const noexcept { return width_; }
    [[nodiscard]] std::uint32_t height() const noexcept { return height_; }
    [[nodiscard]] bool empty() const noexcept { return pixels_.empty(); }
    [[nodiscard]] std::size_t size() const noexcept { return pixels_.size(); }

    T &at(std::uint32_t x, std::uint32_t y) { return pixels_.at(index(x, y)); }
    const T &at(std::uint32_t x, std::uint32_t y) const { return pixels_.at(index(x, y)); }
    T *data() noexcept { return pixels_.data(); }
    const T *data() const noexcept { return pixels_.data(); }
    std::vector<T> &pixels() noexcept { return pixels_; }
    const std::vector<T> &pixels() const noexcept { return pixels_; }

private:
    static std::size_t checked_size(std::uint32_t width, std::uint32_t height) {
        if (width == 0 || height == 0)
            return 0;
        const auto n = static_cast<std::uint64_t>(width) * height;
        if (n > static_cast<std::uint64_t>(SIZE_MAX / sizeof(T)))
            throw std::overflow_error("image is too large");
        return static_cast<std::size_t>(n);
    }

    [[nodiscard]] std::size_t index(std::uint32_t x, std::uint32_t y) const {
        if (x >= width_ || y >= height_)
            throw std::out_of_range("image coordinate");
        return static_cast<std::size_t>(y) * width_ + x;
    }

    std::uint32_t width_ = 0;
    std::uint32_t height_ = 0;
    std::vector<T> pixels_;
};

struct Rect {
    std::uint32_t x = 0;
    std::uint32_t y = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};

} // namespace neuralpass
