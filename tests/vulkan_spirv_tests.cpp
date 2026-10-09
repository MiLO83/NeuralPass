#include "vulkan_capture_shader_spv.hpp"
#include "vulkan_capture_combined_spv.hpp"
#include "vulkan_instrument_test_spv.hpp"
#include "vulkan_spirv.hpp"

#include <cstring>
#include <iostream>
#include <span>
#include <stdexcept>
#include <vector>

namespace {

void require(bool condition, const char *message) {
    if (!condition) throw std::runtime_error(message);
}

std::vector<std::uint32_t> shader_words(const unsigned char *bytes,
                                        std::size_t byte_count) {
    require(byte_count % sizeof(std::uint32_t) == 0,
            "embedded Vulkan shader is not word aligned");
    std::vector<std::uint32_t> result(
        byte_count / sizeof(std::uint32_t));
    std::memcpy(result.data(), bytes, byte_count);
    return result;
}

std::size_t decoration_count(std::span<const std::uint32_t> words,
                             std::uint32_t decoration,
                             std::uint32_t value) {
    std::size_t result = 0;
    for (std::size_t offset = 5; offset < words.size();) {
        const auto count = words[offset] >> 16;
        require(count != 0 && offset + count <= words.size(),
                "malformed SPIR-V in location counter");
        if (static_cast<std::uint16_t>(words[offset]) == 71 && count >= 4 &&
            words[offset + 2] == decoration && words[offset + 3] == value)
            ++result;
        offset += count;
    }
    return result;
}

std::size_t location_count(std::span<const std::uint32_t> words,
                           std::uint32_t location) {
    return decoration_count(words, 30, location);
}

void test_capture_location_patch() {
    const auto original = shader_words(neuralpass_vulkan_capture_spv,
                                       sizeof(neuralpass_vulkan_capture_spv));
    require(location_count(original, 31) == 1,
            "capture shader sentinel location is not unique");
    const auto patched = neuralpass::vulkan_capture::spirv::patch_unique_location(
        original, 31, 7);
    require(!patched.empty(), "capture shader location patch failed");
    require(location_count(patched, 31) == 0 && location_count(patched, 7) == 1,
            "capture shader location patch changed the wrong decoration");
    require(location_count(patched, 0) == location_count(original, 0) &&
            location_count(patched, 1) == location_count(original, 1) &&
            location_count(patched, 2) == location_count(original, 2) &&
            location_count(patched, 3) == location_count(original, 3),
            "capture shader output locations changed during patching");
    auto ambiguous = original;
    ambiguous.insert(ambiguous.end(), {(4u << 16) | 71u, 999u, 30u, 31u});
    require(neuralpass::vulkan_capture::spirv::patch_unique_location(
                ambiguous, 31, 7).empty(),
            "ambiguous sentinel locations were accepted");
}

void test_combined_sampler_patch() {
    auto words = shader_words(neuralpass_vulkan_capture_combined_spv,
                              sizeof(neuralpass_vulkan_capture_combined_spv));
    require(decoration_count(words, 34, 31) == 1 &&
            decoration_count(words, 33, 31) == 1,
            "combined source shader descriptor sentinels are not unique");
    words = neuralpass::vulkan_capture::spirv::patch_unique_decoration(
        words, 34, 31, 3);
    require(!words.empty(), "combined source descriptor set patch failed");
    words = neuralpass::vulkan_capture::spirv::patch_unique_decoration(
        words, 33, 31, 9);
    require(!words.empty() && decoration_count(words, 34, 3) == 1 &&
            decoration_count(words, 33, 9) == 1 && location_count(words, 31) == 1,
            "combined source descriptor patch changed another decoration");
}

void test_vertex_uv_instrumentation() {
    const auto original = shader_words(neuralpass_vulkan_instrument_test_spv,
                                       sizeof(neuralpass_vulkan_instrument_test_spv));
    const auto instrumented =
        neuralpass::vulkan_capture::spirv::instrument_vertex_uv(original, 2, "main");
    require(static_cast<bool>(instrumented),
            "valid Vulkan vertex UV input could not be instrumented");
    require(instrumented.varying_location < 16 &&
            instrumented.varying_location != 0 &&
            instrumented.varying_location != 2,
            "instrumentation did not allocate a safe guaranteed output location");
    require(neuralpass::vulkan_capture::spirv::has_float2_output_at_location(
                instrumented.words, instrumented.varying_location),
            "instrumented float2 vertex output is missing");
    require(instrumented.words[3] > original[3],
            "instrumentation did not update the SPIR-V ID bound");
    require(!neuralpass::vulkan_capture::spirv::instrument_vertex_uv(
                original, 7, "main"),
            "missing UV input location was accepted");
    require(!neuralpass::vulkan_capture::spirv::instrument_vertex_uv(
                original, 2, "not_an_entry_point"),
            "missing vertex entry point was accepted");

    const auto capture = shader_words(neuralpass_vulkan_capture_spv,
                                      sizeof(neuralpass_vulkan_capture_spv));
    const auto patched_capture =
        neuralpass::vulkan_capture::spirv::patch_unique_location(
            capture, 31, instrumented.varying_location);
    require(!patched_capture.empty() &&
            location_count(patched_capture, instrumented.varying_location) == 1,
            "capture fragment input was not connected to instrumented vertex output");
}

void test_float2_output_detection() {
    // Header plus: float32, vec2, Output pointer, Location decoration, variable.
    const std::vector<std::uint32_t> module {
        0x07230203u, 0x00010000u, 0u, 6u, 0u,
        (3u << 16) | 22u, 1u, 32u,
        (4u << 16) | 23u, 2u, 1u, 2u,
        (4u << 16) | 32u, 3u, 3u, 2u,
        (4u << 16) | 71u, 4u, 30u, 5u,
        (4u << 16) | 59u, 3u, 4u, 3u,
    };
    require(neuralpass::vulkan_capture::spirv::has_float2_output_at_location(module, 5),
            "valid float2 vertex output was rejected");
    require(!neuralpass::vulkan_capture::spirv::has_float2_output_at_location(module, 6),
            "wrong vertex output location was accepted");
    auto malformed = module;
    malformed.back() = 99u;
    require(!neuralpass::vulkan_capture::spirv::has_float2_output_at_location(malformed, 5),
            "non-output storage class was accepted");
}

} // namespace

int main() {
    try {
        test_capture_location_patch();
        test_combined_sampler_patch();
        test_float2_output_detection();
        test_vertex_uv_instrumentation();
        std::cout << "Vulkan SPIR-V tests passed\n";
        return 0;
    } catch (const std::exception &error) {
        std::cerr << "Vulkan SPIR-V tests failed: " << error.what() << '\n';
        return 1;
    }
}
