#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neuralpass::vulkan_capture::spirv {

inline constexpr std::uint32_t k_magic = 0x07230203u;
inline constexpr std::uint32_t k_location_decoration = 30;
inline constexpr std::uint32_t k_output_storage_class = 3;

struct InstrumentedVertex {
    std::vector<std::uint32_t> words;
    std::uint32_t varying_location = UINT32_MAX;

    [[nodiscard]] explicit operator bool() const noexcept {
        return !words.empty() && varying_location != UINT32_MAX;
    }
};

[[nodiscard]] inline std::vector<std::uint32_t> patch_unique_decoration(
    std::span<const std::uint32_t> source, std::uint32_t decoration,
    std::uint32_t sentinel, std::uint32_t replacement) {
    if (source.size() < 5 || source.front() != k_magic) return {};
    std::vector<std::uint32_t> result(source.begin(), source.end());
    std::size_t matches = 0;
    for (std::size_t offset = 5; offset < result.size();) {
        const auto instruction = result[offset];
        const auto count = instruction >> 16;
        const auto opcode = static_cast<std::uint16_t>(instruction);
        if (count == 0 || offset + count > result.size()) return {};
        if (opcode == 71 && count >= 4 &&
            result[offset + 2] == decoration &&
            result[offset + 3] == sentinel) {
            result[offset + 3] = replacement;
            ++matches;
        }
        offset += count;
    }
    return matches == 1 ? result : std::vector<std::uint32_t> {};
}

// Changes one deliberately reserved interface location. Refuse malformed modules
// and ambiguous modules so a shader update cannot silently patch the wrong value.
[[nodiscard]] inline std::vector<std::uint32_t> patch_unique_location(
    std::span<const std::uint32_t> source, std::uint32_t sentinel,
    std::uint32_t replacement) {
    return patch_unique_decoration(
        source, k_location_decoration, sentinel, replacement);
}

// This is intentionally conservative. A location is accepted only when the
// application's vertex SPIR-V exposes a plain 32-bit float2 output there.
// Struct/member interfaces are skipped until they can be instrumented safely.
[[nodiscard]] inline bool has_float2_output_at_location(
    std::span<const std::uint32_t> words, std::uint32_t location) {
    if (words.size() < 5 || words.front() != k_magic) return false;

    std::unordered_map<std::uint32_t, std::uint32_t> locations;
    std::unordered_map<std::uint32_t, std::uint32_t> float_widths;
    struct VectorType { std::uint32_t component = 0; std::uint32_t count = 0; };
    struct PointerType { std::uint32_t storage = 0; std::uint32_t pointee = 0; };
    struct Variable { std::uint32_t pointer = 0; std::uint32_t storage = 0; };
    std::unordered_map<std::uint32_t, VectorType> vectors;
    std::unordered_map<std::uint32_t, PointerType> pointers;
    std::unordered_map<std::uint32_t, Variable> variables;

    for (std::size_t offset = 5; offset < words.size();) {
        const auto instruction = words[offset];
        const auto count = instruction >> 16;
        const auto opcode = static_cast<std::uint16_t>(instruction);
        if (count == 0 || offset + count > words.size()) return false;
        switch (opcode) {
        case 71: // OpDecorate
            if (count >= 4 && words[offset + 2] == k_location_decoration)
                locations[words[offset + 1]] = words[offset + 3];
            break;
        case 22: // OpTypeFloat
            if (count >= 3) float_widths[words[offset + 1]] = words[offset + 2];
            break;
        case 23: // OpTypeVector
            if (count >= 4)
                vectors[words[offset + 1]] = {words[offset + 2], words[offset + 3]};
            break;
        case 32: // OpTypePointer
            if (count >= 4)
                pointers[words[offset + 1]] = {words[offset + 2], words[offset + 3]};
            break;
        case 59: // OpVariable
            if (count >= 4)
                variables[words[offset + 2]] = {words[offset + 1], words[offset + 3]};
            break;
        default:
            break;
        }
        offset += count;
    }

    for (const auto &[variable_id, variable] : variables) {
        const auto decorated = locations.find(variable_id);
        if (decorated == locations.end() || decorated->second != location ||
            variable.storage != k_output_storage_class)
            continue;
        const auto pointer = pointers.find(variable.pointer);
        if (pointer == pointers.end() ||
            pointer->second.storage != k_output_storage_class)
            continue;
        const auto vector = vectors.find(pointer->second.pointee);
        if (vector == vectors.end() || vector->second.count != 2) continue;
        const auto width = float_widths.find(vector->second.component);
        if (width != float_widths.end() && width->second == 32) return true;
    }
    return false;
}

[[nodiscard]] inline std::string_view literal_string(
    std::span<const std::uint32_t> operands) {
    const auto *bytes = reinterpret_cast<const char *>(operands.data());
    const auto capacity = operands.size() * sizeof(std::uint32_t);
    std::size_t length = 0;
    while (length < capacity && bytes[length] != '\0') ++length;
    return length < capacity ? std::string_view(bytes, length) : std::string_view {};
}

// Adds a dedicated vertex-to-fragment float2 varying sourced directly from the
// selected application vertex input. This avoids assuming that input-layout and
// fragment-interface locations are related (they are not in Vulkan).
[[nodiscard]] inline InstrumentedVertex instrument_vertex_uv(
    std::span<const std::uint32_t> source, std::uint32_t input_location,
    std::string_view requested_entry = {}) {
    if (source.size() < 5 || source.front() != k_magic || source[3] == 0)
        return {};

    std::unordered_map<std::uint32_t, std::uint32_t> locations;
    std::unordered_set<std::uint32_t> occupied_locations;
    std::unordered_map<std::uint32_t, std::uint32_t> float_widths;
    struct VectorType { std::uint32_t component = 0; std::uint32_t count = 0; };
    struct PointerType { std::uint32_t storage = 0; std::uint32_t pointee = 0; };
    struct Variable { std::uint32_t pointer = 0; std::uint32_t storage = 0; };
    std::unordered_map<std::uint32_t, VectorType> vectors;
    std::unordered_map<std::uint32_t, PointerType> pointers;
    std::unordered_map<std::uint32_t, Variable> variables;
    std::unordered_map<std::uint32_t, std::size_t> definition_offsets;
    std::uint32_t entry_function = 0;
    std::size_t annotation_offset = 0;
    std::size_t return_count = 0;
    std::uint32_t current_function = 0;

    for (std::size_t offset = 5; offset < source.size();) {
        const auto instruction = source[offset];
        const auto count = instruction >> 16;
        const auto opcode = static_cast<std::uint16_t>(instruction);
        if (count == 0 || offset + count > source.size()) return {};
        if (annotation_offset == 0 && opcode >= 19 && opcode <= 39)
            annotation_offset = offset;
        switch (opcode) {
        case 15: { // OpEntryPoint
            if (count < 4 || source[offset + 1] != 0) break; // Vertex only.
            const auto name = literal_string(source.subspan(offset + 3, count - 3));
            if (requested_entry.empty() ? name == "main" : name == requested_entry)
                entry_function = source[offset + 2];
            break;
        }
        case 71: // OpDecorate
            if (count >= 4 && source[offset + 2] == k_location_decoration) {
                locations[source[offset + 1]] = source[offset + 3];
                occupied_locations.insert(source[offset + 3]);
            }
            break;
        case 72: // OpMemberDecorate
            if (count >= 5 && source[offset + 3] == k_location_decoration)
                occupied_locations.insert(source[offset + 4]);
            break;
        case 22: // OpTypeFloat
            if (count >= 3) {
                float_widths[source[offset + 1]] = source[offset + 2];
                definition_offsets[source[offset + 1]] = offset;
            }
            break;
        case 23: // OpTypeVector
            if (count >= 4) {
                vectors[source[offset + 1]] = {source[offset + 2], source[offset + 3]};
                definition_offsets[source[offset + 1]] = offset;
            }
            break;
        case 32: // OpTypePointer
            if (count >= 4) {
                pointers[source[offset + 1]] = {source[offset + 2], source[offset + 3]};
                definition_offsets[source[offset + 1]] = offset;
            }
            break;
        case 59: // OpVariable
            if (count >= 4)
                variables[source[offset + 2]] = {source[offset + 1], source[offset + 3]};
            break;
        case 54: // OpFunction
            if (count >= 3) current_function = source[offset + 2];
            break;
        case 56: // OpFunctionEnd
            current_function = 0;
            break;
        case 253: // OpReturn
            if (current_function != 0 && current_function == entry_function)
                ++return_count;
            break;
        default:
            break;
        }
        offset += count;
    }
    if (entry_function == 0 || annotation_offset == 0 || return_count == 0)
        return {};

    std::uint32_t input_variable = 0;
    std::uint32_t vector_type = 0;
    for (const auto &[variable_id, variable] : variables) {
        const auto decorated = locations.find(variable_id);
        if (decorated == locations.end() || decorated->second != input_location ||
            variable.storage != 1) // Input
            continue;
        const auto pointer = pointers.find(variable.pointer);
        if (pointer == pointers.end() || pointer->second.storage != 1) continue;
        const auto vector = vectors.find(pointer->second.pointee);
        if (vector == vectors.end() || vector->second.count != 2) continue;
        const auto width = float_widths.find(vector->second.component);
        if (width != float_widths.end() && width->second == 32) {
            input_variable = variable_id;
            vector_type = pointer->second.pointee;
            break;
        }
    }
    if (input_variable == 0) return {};

    std::uint32_t varying_location = UINT32_MAX;
    // Vulkan guarantees at least 64 vertex output components (16 vec4 slots).
    for (std::uint32_t candidate = 16; candidate-- != 0;) {
        if (!occupied_locations.contains(candidate)) {
            varying_location = candidate;
            break;
        }
    }
    if (varying_location == UINT32_MAX) return {};

    std::uint32_t output_pointer_type = 0;
    for (const auto &[id, pointer] : pointers)
        if (pointer.storage == k_output_storage_class && pointer.pointee == vector_type) {
            output_pointer_type = id;
            break;
        }

    std::uint32_t next_id = source[3];
    const bool add_pointer_type = output_pointer_type == 0;
    if (add_pointer_type) output_pointer_type = next_id++;
    const auto output_variable = next_id++;
    const auto insertion_definition = add_pointer_type ? vector_type : output_pointer_type;
    const auto insertion = definition_offsets.find(insertion_definition);
    if (insertion == definition_offsets.end()) return {};

    std::vector<std::uint32_t> result;
    result.reserve(source.size() + 16 + return_count * 7);
    result.insert(result.end(), source.begin(), source.begin() + 5);
    current_function = 0;
    for (std::size_t offset = 5; offset < source.size();) {
        const auto instruction = source[offset];
        const auto count = instruction >> 16;
        const auto opcode = static_cast<std::uint16_t>(instruction);

        if (offset == annotation_offset)
            result.insert(result.end(), {(4u << 16) | 71u, output_variable,
                                         k_location_decoration, varying_location});
        if (opcode == 15 && count >= 4 && source[offset + 1] == 0 &&
            source[offset + 2] == entry_function) {
            result.push_back(((count + 1) << 16) | opcode);
            result.insert(result.end(), source.begin() + offset + 1,
                          source.begin() + offset + count);
            result.push_back(output_variable);
        } else {
            if (opcode == 54 && count >= 3) current_function = source[offset + 2];
            if (opcode == 253 && current_function == entry_function) {
                const auto loaded_uv = next_id++;
                result.insert(result.end(), {(4u << 16) | 61u, vector_type,
                                             loaded_uv, input_variable,
                                             (3u << 16) | 62u, output_variable,
                                             loaded_uv});
            }
            result.insert(result.end(), source.begin() + offset,
                          source.begin() + offset + count);
            if (opcode == 56) current_function = 0;
        }
        if (offset == insertion->second) {
            if (add_pointer_type)
                result.insert(result.end(), {(4u << 16) | 32u, output_pointer_type,
                                             k_output_storage_class, vector_type});
            result.insert(result.end(), {(4u << 16) | 59u, output_pointer_type,
                                         output_variable, k_output_storage_class});
        }
        offset += count;
    }
    result[3] = next_id;
    return {std::move(result), varying_location};
}

} // namespace neuralpass::vulkan_capture::spirv
