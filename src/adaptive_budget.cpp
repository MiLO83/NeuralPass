#include "neuralpass/adaptive_budget.hpp"

#include <algorithm>
#include <cmath>
#include <stdexcept>

namespace neuralpass {

namespace {

bool valid_time(float value) {
    return std::isfinite(value) && value > 0.0f;
}

float update_ema(float previous, float sample, float weight) {
    return previous == 0.0f ? sample : previous + weight * (sample - previous);
}

} // namespace

AdaptiveTileBudgetController::AdaptiveTileBudgetController(AdaptiveBudgetSettings settings) {
    configure(settings);
    reset();
}

void AdaptiveTileBudgetController::configure(AdaptiveBudgetSettings settings) {
    if (!valid_time(settings.target_frame_time_ms))
        throw std::invalid_argument("target frame time must be finite and positive");
    if (settings.minimum_budget > settings.maximum_budget)
        throw std::invalid_argument("minimum tile budget exceeds maximum");
    if (settings.recovery_samples == 0)
        throw std::invalid_argument("recovery sample count must be non-zero");
    settings_ = settings;
    budget_ = std::clamp(budget_, settings_.minimum_budget, settings_.maximum_budget);
}

void AdaptiveTileBudgetController::reset(std::uint32_t initial_budget) {
    budget_ = std::clamp(initial_budget, settings_.minimum_budget, settings_.maximum_budget);
    healthy_samples_ = 0;
    frame_time_ema_ms_ = 0.0f;
    tile_time_ema_ms_ = 0.0f;
}

AdaptiveBudgetDecision AdaptiveTileBudgetController::observe(
    const AdaptiveBudgetSample &sample) {
    if (valid_time(sample.frame_time_ms) && sample.frame_time_ms <= 250.0f)
        frame_time_ema_ms_ = update_ema(frame_time_ema_ms_, sample.frame_time_ms, 0.125f);
    if (sample.attempted_tiles != 0 && valid_time(sample.worker_time_ms)) {
        const auto per_tile = sample.worker_time_ms /
            static_cast<float>(sample.attempted_tiles);
        tile_time_ema_ms_ = update_ema(tile_time_ema_ms_, per_tile, 0.25f);
    }

    const float target = settings_.target_frame_time_ms;
    const float headroom = frame_time_ema_ms_ == 0.0f
        ? 0.0f : target - frame_time_ema_ms_;
    const bool slow_frame = frame_time_ema_ms_ > target * 1.05f;
    const bool marginal_with_expensive_tiles =
        frame_time_ema_ms_ > target * 0.95f &&
        tile_time_ema_ms_ > target * 0.50f;
    const bool pressure = sample.capture_backlog || slow_frame ||
        marginal_with_expensive_tiles;
    const auto previous = budget_;

    if (pressure) {
        healthy_samples_ = 0;
        if (budget_ > settings_.minimum_budget) --budget_;
    } else {
        const bool enough_headroom = frame_time_ema_ms_ != 0.0f &&
            frame_time_ema_ms_ <= target * 0.90f;
        // When suspended, a healthy render cadence is sufficient to probe one
        // tile. Otherwise avoid adding work if each tile already consumes most
        // of the target-frame interval.
        const bool affordable = budget_ == 0 || tile_time_ema_ms_ == 0.0f ||
            tile_time_ema_ms_ <= target * 0.75f;
        if (enough_headroom && affordable) {
            if (++healthy_samples_ >= settings_.recovery_samples) {
                if (budget_ < settings_.maximum_budget) ++budget_;
                healthy_samples_ = 0;
            }
        } else {
            healthy_samples_ = 0;
        }
    }

    return {
        budget_, frame_time_ema_ms_, tile_time_ema_ms_, headroom,
        pressure, budget_ != previous};
}

} // namespace neuralpass
