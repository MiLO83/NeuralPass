#pragma once

#include <cstdint>
#include <string>
#include <string_view>

namespace neuralpass {

struct RuntimeDisplayEvidence {
    std::string_view generated_utc;
    std::string_view process_architecture;
    std::string_view graphics_api;
    std::uint32_t graphics_api_value = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::string_view backbuffer_format;
    std::uint32_t backbuffer_format_value = 0;
    std::string_view swapchain_color_space;
    std::uint32_t swapchain_color_space_value = 0;
    bool display_capture_supported = false;
    std::string_view display_encoding;
    bool hdr_path = false;
    std::string_view classification;
};

// Deterministic schema-v1 JSON used by the loaded add-on and packaged
// diagnostics. The terminating newline makes interrupted/manual inspection
// unambiguous; publication itself is an atomic platform operation.
[[nodiscard]] std::string runtime_display_evidence_json(
    const RuntimeDisplayEvidence &evidence);

} // namespace neuralpass
