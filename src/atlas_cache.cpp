#include "neuralpass/texture_baker.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cstring>
#include <fstream>
#include <system_error>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace neuralpass {
namespace {

constexpr char k_magic[8] {'N', 'P', 'A', 'T', 'L', '0', '1', '\0'};
constexpr std::uint32_t k_version = 1;
constexpr std::uint64_t k_fnv_offset = 1469598103934665603ull;
constexpr std::uint64_t k_fnv_prime = 1099511628211ull;
constexpr std::uint64_t k_max_texels = 16384ull * 16384ull;

void append_u32(std::vector<std::uint8_t> &bytes, std::uint32_t value) {
    for (unsigned int shift = 0; shift != 32; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

void append_float(std::vector<std::uint8_t> &bytes, float value) {
    append_u32(bytes, std::bit_cast<std::uint32_t>(value));
}

bool read_u32(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint32_t &value) {
    if (offset + 4 > bytes.size()) return false;
    value = 0;
    for (unsigned int shift = 0; shift != 32; shift += 8)
        value |= static_cast<std::uint32_t>(bytes[offset++]) << shift;
    return true;
}

bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint64_t &value) {
    if (offset + 8 > bytes.size()) return false;
    value = 0;
    for (unsigned int shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

bool read_float(const std::vector<std::uint8_t> &bytes, std::size_t &offset, float &value) {
    std::uint32_t bits = 0;
    if (!read_u32(bytes, offset, bits)) return false;
    value = std::bit_cast<float>(bits);
    return true;
}

std::uint64_t checksum(const std::uint8_t *data, std::size_t size) {
    std::uint64_t result = k_fnv_offset;
    for (std::size_t index = 0; index < size; ++index) {
        result ^= data[index];
        result *= k_fnv_prime;
    }
    return result;
}

std::string material_prefix(std::uint64_t material_id) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = digits[material_id & 0xFu];
        material_id >>= 4;
    }
    return result;
}

} // namespace

bool MaterialTextureAtlas::save(const std::filesystem::path &path) const {
    if (material_id_ == 0 || color_.empty() || color_.size() != history_weight_.size() ||
        color_.size() != coverage_.size()) return false;

    std::vector<std::uint8_t> payload;
    payload.reserve(color_.size() * (sizeof(float) * 5 + sizeof(std::uint8_t)));
    for (const auto &pixel : color_.pixels()) {
        append_float(payload, pixel.r);
        append_float(payload, pixel.g);
        append_float(payload, pixel.b);
        append_float(payload, pixel.a);
    }
    for (const auto weight : history_weight_.pixels()) append_float(payload, weight);
    payload.insert(payload.end(), coverage_.pixels().begin(), coverage_.pixels().end());

    std::vector<std::uint8_t> file;
    file.insert(file.end(), std::begin(k_magic), std::end(k_magic));
    append_u32(file, k_version);
    append_u64(file, material_id_);
    append_u32(file, width());
    append_u32(file, height());
    append_u64(file, payload.size());
    append_u64(file, checksum(payload.data(), payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());

    std::error_code error;
    std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        output.flush();
        if (!output) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) {
        std::filesystem::remove(temporary, error);
        return false;
    }
    return true;
}

std::optional<MaterialTextureAtlas> MaterialTextureAtlas::load(
    const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(input)), {});
    constexpr std::size_t header_size = 8 + 4 + 8 + 4 + 4 + 8 + 8;
    if (file.size() < header_size || std::memcmp(file.data(), k_magic, sizeof(k_magic)) != 0)
        return std::nullopt;
    std::size_t offset = sizeof(k_magic);
    std::uint32_t version = 0, width = 0, height = 0;
    std::uint64_t material_id = 0, payload_size = 0, expected_checksum = 0;
    if (!read_u32(file, offset, version) || !read_u64(file, offset, material_id) ||
        !read_u32(file, offset, width) || !read_u32(file, offset, height) ||
        !read_u64(file, offset, payload_size) || !read_u64(file, offset, expected_checksum) ||
        version != k_version || material_id == 0 || width == 0 || height == 0 ||
        static_cast<std::uint64_t>(width) * height > k_max_texels ||
        payload_size != static_cast<std::uint64_t>(width) * height * 21ull ||
        payload_size != file.size() - offset ||
        checksum(file.data() + offset, static_cast<std::size_t>(payload_size)) != expected_checksum)
        return std::nullopt;

    MaterialTextureAtlas atlas(material_id, width, height);
    for (auto &pixel : atlas.color_.pixels()) {
        if (!read_float(file, offset, pixel.r) || !read_float(file, offset, pixel.g) ||
            !read_float(file, offset, pixel.b) || !read_float(file, offset, pixel.a))
            return std::nullopt;
    }
    for (auto &weight : atlas.history_weight_.pixels())
        if (!read_float(file, offset, weight)) return std::nullopt;
    for (auto &coverage : atlas.coverage_.pixels()) {
        if (offset >= file.size() || file[offset] > kObserved) return std::nullopt;
        coverage = file[offset++];
    }
    if (offset != file.size()) return std::nullopt;
    return atlas;
}

AtlasCacheStats MaterialTextureBaker::save_cache(
    const std::filesystem::path &directory,
    std::span<const std::uint64_t> restart_stable_material_ids) const {
    AtlasCacheStats stats;
    std::unordered_set<std::uint64_t> stable(restart_stable_material_ids.begin(),
                                             restart_stable_material_ids.end());
    const auto generation = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    for (const auto &[material_id, atlas] : atlases_) {
        if (!stable.contains(material_id)) continue;
        const auto filename = material_prefix(material_id) + "-" +
            std::to_string(generation) + ".npatlas";
        if (atlas.save(directory / filename)) ++stats.saved;
        else ++stats.rejected;
    }
    return stats;
}

AtlasCacheStats MaterialTextureBaker::load_cache(
    const std::filesystem::path &directory, bool replace) {
    AtlasCacheStats stats;
    std::error_code error;
    if (!std::filesystem::exists(directory, error) || error) return stats;

    struct Candidate {
        std::filesystem::path path;
        std::filesystem::file_time_type modified;
    };
    std::unordered_map<std::uint64_t, Candidate> newest;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || entry.path().extension() != ".npatlas") continue;
        auto atlas = MaterialTextureAtlas::load(entry.path());
        if (!atlas) {
            ++stats.rejected;
            continue;
        }
        const auto modified = entry.last_write_time(error);
        if (error) {
            error.clear();
            ++stats.rejected;
            continue;
        }
        const auto known = newest.find(atlas->material_id());
        if (known == newest.end() || known->second.modified < modified)
            newest[atlas->material_id()] = {entry.path(), modified};
    }

    if (replace) atlases_.clear();
    for (const auto &[material_id, candidate] : newest) {
        auto atlas = MaterialTextureAtlas::load(candidate.path);
        if (!atlas || atlas->width() != settings_.atlas_width ||
            atlas->height() != settings_.atlas_height) {
            ++stats.rejected;
            continue;
        }
        atlases_.insert_or_assign(material_id, std::move(*atlas));
        ++stats.loaded;
    }
    ++epoch_;
    return stats;
}

} // namespace neuralpass
