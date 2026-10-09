#include "neuralpass/scene_cache.hpp"

#include <algorithm>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <optional>
#include <stdexcept>
#include <string>
#include <unordered_set>
#include <vector>

namespace neuralpass {
namespace {

constexpr char k_magic[8] {'N', 'P', 'S', 'C', 'N', '0', '1', '\0'};
constexpr std::uint64_t k_offset = 1469598103934665603ull;
constexpr std::uint64_t k_prime = 1099511628211ull;
constexpr std::uint64_t k_max_bindings = 1'000'000;

void append_u64(std::vector<std::uint8_t> &bytes, std::uint64_t value) {
    for (unsigned int shift = 0; shift != 64; shift += 8)
        bytes.push_back(static_cast<std::uint8_t>(value >> shift));
}

bool read_u64(const std::vector<std::uint8_t> &bytes, std::size_t &offset,
              std::uint64_t &value) {
    if (offset + 8 > bytes.size()) return false;
    value = 0;
    for (unsigned int shift = 0; shift != 64; shift += 8)
        value |= static_cast<std::uint64_t>(bytes[offset++]) << shift;
    return true;
}

std::uint64_t checksum(const std::uint8_t *data, std::size_t size) {
    std::uint64_t hash = k_offset;
    for (std::size_t index = 0; index < size; ++index) {
        hash ^= data[index];
        hash *= k_prime;
    }
    return hash;
}

std::string hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = digits[value & 0xfu];
        value >>= 4;
    }
    return result;
}

std::vector<std::uint64_t> canonical(std::span<const std::uint64_t> ids) {
    std::vector<std::uint64_t> result;
    result.reserve(ids.size());
    for (const auto id : ids) if (id != 0) result.push_back(id);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::uint64_t identity(const std::vector<std::uint64_t> &ids) {
    std::vector<std::uint8_t> bytes;
    bytes.reserve(ids.size() * 8);
    for (const auto id : ids) append_u64(bytes, id);
    auto result = checksum(bytes.data(), bytes.size());
    return result == 0 ? 1 : result;
}

struct Manifest {
    std::uint64_t identity = 0;
    std::vector<std::uint64_t> bindings;
};

std::optional<Manifest> load_manifest(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::nullopt;
    std::vector<std::uint8_t> file((std::istreambuf_iterator<char>(input)), {});
    if (file.size() < sizeof(k_magic) + 24 ||
        std::memcmp(file.data(), k_magic, sizeof(k_magic)) != 0) return std::nullopt;
    std::size_t offset = sizeof(k_magic);
    std::uint64_t scene = 0, count = 0, expected = 0;
    if (!read_u64(file, offset, scene) || !read_u64(file, offset, count) ||
        !read_u64(file, offset, expected) || scene == 0 || count == 0 ||
        count > k_max_bindings || file.size() - offset != count * 8 ||
        checksum(file.data() + offset, file.size() - offset) != expected)
        return std::nullopt;
    Manifest result;
    result.identity = scene;
    result.bindings.reserve(static_cast<std::size_t>(count));
    for (std::uint64_t index = 0, value = 0; index < count; ++index) {
        if (!read_u64(file, offset, value) || value == 0) return std::nullopt;
        result.bindings.push_back(value);
    }
    if (!std::is_sorted(result.bindings.begin(), result.bindings.end()) ||
        std::adjacent_find(result.bindings.begin(), result.bindings.end()) != result.bindings.end())
        return std::nullopt;
    return result;
}

std::optional<Manifest> newest_manifest(const std::filesystem::path &directory) {
    std::error_code error;
    std::optional<Manifest> result;
    std::filesystem::file_time_type newest {};
    std::filesystem::path newest_path;
    for (const auto &entry : std::filesystem::directory_iterator(directory, error)) {
        if (error) break;
        if (!entry.is_regular_file(error) || entry.path().extension() != ".npscene") continue;
        auto manifest = load_manifest(entry.path());
        if (!manifest) continue;
        const auto modified = entry.last_write_time(error);
        if (error) { error.clear(); continue; }
        if (!result || manifest->bindings.size() > result->bindings.size() ||
            (manifest->bindings.size() == result->bindings.size() &&
             (modified > newest || (modified == newest && entry.path() > newest_path)))) {
            newest = modified;
            newest_path = entry.path();
            result = std::move(manifest);
        }
    }
    return result;
}

float overlap(const std::vector<std::uint64_t> &left,
              const std::vector<std::uint64_t> &right) {
    std::size_t shared = 0, a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (left[a] == right[b]) { ++shared; ++a; ++b; }
        else if (left[a] < right[b]) ++a;
        else ++b;
    }
    return static_cast<float>(shared) /
        static_cast<float>(std::min(left.size(), right.size()));
}

} // namespace

SceneCacheCatalog::SceneCacheCatalog(std::filesystem::path root, float overlap_threshold)
    : root_(std::move(root)), overlap_threshold_(overlap_threshold) {
    if (!std::isfinite(overlap_threshold_) || overlap_threshold_ <= 0.0f ||
        overlap_threshold_ > 1.0f)
        throw std::invalid_argument("invalid scene cache overlap threshold");
}

SceneCacheSelection SceneCacheCatalog::resolve(
    std::span<const std::uint64_t> visible_binding_ids) const {
    const auto visible = canonical(visible_binding_ids);
    if (visible.empty()) return {};
    std::error_code error;
    std::uint64_t best_identity = 0;
    float best_overlap = 0.0f;
    if (std::filesystem::exists(root_, error) && !error) {
        for (const auto &entry : std::filesystem::directory_iterator(root_, error)) {
            if (error) break;
            if (!entry.is_directory(error)) continue;
            const auto manifest = newest_manifest(entry.path());
            if (!manifest) continue;
            const float score = overlap(visible, manifest->bindings);
            if (score >= overlap_threshold_ && score > best_overlap) {
                best_overlap = score;
                best_identity = manifest->identity;
            }
        }
    }
    if (best_identity != 0)
        return {best_identity, root_ / ("scene-" + hex(best_identity)), true};
    const auto created = identity(visible);
    return {created, root_ / ("scene-" + hex(created)), false};
}

SceneCacheSelection SceneCacheCatalog::create_new(
    std::span<const std::uint64_t> visible_binding_ids,
    std::uint64_t discriminator) const {
    auto visible = canonical(visible_binding_ids);
    if (visible.empty()) return {};
    visible.push_back(discriminator == 0 ? 1 : discriminator);
    const auto created = identity(visible);
    return {created, root_ / ("scene-" + hex(created)), false};
}

bool SceneCacheCatalog::record(
    std::uint64_t scene_identity,
    std::span<const std::uint64_t> visible_binding_ids) const {
    if (scene_identity == 0) return false;
    auto bindings = canonical(visible_binding_ids);
    if (bindings.empty()) return false;
    const auto directory = root_ / ("scene-" + hex(scene_identity));
    if (const auto previous = newest_manifest(directory);
        previous && previous->identity == scene_identity) {
        bindings.insert(bindings.end(), previous->bindings.begin(), previous->bindings.end());
        std::sort(bindings.begin(), bindings.end());
        bindings.erase(std::unique(bindings.begin(), bindings.end()), bindings.end());
    }
    std::vector<std::uint8_t> payload;
    payload.reserve(bindings.size() * 8);
    for (const auto id : bindings) append_u64(payload, id);
    std::vector<std::uint8_t> file(std::begin(k_magic), std::end(k_magic));
    append_u64(file, scene_identity);
    append_u64(file, bindings.size());
    append_u64(file, checksum(payload.data(), payload.size()));
    file.insert(file.end(), payload.begin(), payload.end());
    std::error_code error;
    std::filesystem::create_directories(directory, error);
    if (error) return false;
    const auto generation = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
    const auto path = directory / ("materials-" + std::to_string(generation) + ".npscene");
    const auto temporary = path.string() + ".tmp";
    {
        std::ofstream output(temporary, std::ios::binary | std::ios::trunc);
        output.write(reinterpret_cast<const char *>(file.data()),
                     static_cast<std::streamsize>(file.size()));
        output.flush();
        if (!output) return false;
    }
    std::filesystem::rename(temporary, path, error);
    if (error) { std::filesystem::remove(temporary, error); return false; }
    return true;
}

} // namespace neuralpass
