#pragma once

#include <cstdint>

namespace neuralpass {

struct AdaptiveBudgetSettings {
    std::uint32_t minimum_budget = 0;
    std::uint32_t maximum_budget = 2;
    float target_frame_time_ms = 1000.0f / 60.0f;
    std::uint32_t recovery_samples = 30;
};

struct AdaptiveBudgetSample {
    float frame_time_ms = 0.0f;
    float worker_time_ms = 0.0f;
    std::uint32_t attempted_tiles = 0;
    bool capture_backlog = false;
};

struct AdaptiveBudgetDecision {
    std::uint32_t tile_budget = 0;
    float smoothed_frame_time_ms = 0.0f;
    float smoothed_tile_time_ms = 0.0f;
    float frame_headroom_ms = 0.0f;
    bool under_pressure = false;
    bool changed = false;
};

// A deliberately asymmetric controller: overload removes work immediately,
// while recovery requires sustained frame-time headroom. This controls the
// asynchronous inference workload without ever waiting in the render thread.
class AdaptiveTileBudgetController {
public:
    explicit AdaptiveTileBudgetController(AdaptiveBudgetSettings settings = {});

    void configure(AdaptiveBudgetSettings settings);
    void reset(std::uint32_t initial_budget = 1);
    [[nodiscard]] AdaptiveBudgetDecision observe(const AdaptiveBudgetSample &sample);
    [[nodiscard]] std::uint32_t budget() const noexcept { return budget_; }

private:
    AdaptiveBudgetSettings settings_;
    std::uint32_t budget_ = 1;
    std::uint32_t healthy_samples_ = 0;
    float frame_time_ema_ms_ = 0.0f;
    float tile_time_ema_ms_ = 0.0f;
};

} // namespace neuralpass
