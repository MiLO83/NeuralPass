#include "neuralpass/cache_control.hpp"

#include <string>

namespace neuralpass {
namespace {

std::string hex(std::uint64_t value) {
    constexpr char digits[] = "0123456789abcdef";
    std::string result(16, '0');
    for (int index = 15; index >= 0; --index) {
        result[static_cast<std::size_t>(index)] = digits[value & 0xfu];
        value >>= 4;
    }
    return result;
}

bool component_prefix(const std::filesystem::path &path, const char *prefix) {
    const auto name = path.filename().string();
    return name.starts_with(prefix);
}

bool lexically_within(const std::filesystem::path &root,
                      const std::filesystem::path &target) {
    if (root.empty() || target.empty()) return false;
    std::error_code error;
    const auto normalized_root = std::filesystem::absolute(root, error).lexically_normal();
    if (error) return false;
    const auto normalized_target = std::filesystem::absolute(target, error).lexically_normal();
    if (error) return false;
    auto root_part = normalized_root.begin();
    auto target_part = normalized_target.begin();
    for (; root_part != normalized_root.end(); ++root_part, ++target_part)
        if (target_part == normalized_target.end() || *root_part != *target_part)
            return false;
    return target_part != normalized_target.end();
}

bool safe_cache_target(const std::filesystem::path &root,
                       const std::filesystem::path &target) {
    if (root.filename() != "NeuralPassCache" || !lexically_within(root, target))
        return false;
    std::error_code error;
    const bool root_exists = std::filesystem::exists(root, error);
    if (error) return false;
    if (!root_exists) return true;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(root, error)) || error)
        return false;
    const auto canonical_root = std::filesystem::weakly_canonical(root, error);
    if (error) return false;
    const auto canonical_target = std::filesystem::weakly_canonical(target, error);
    if (error) return false;
    return lexically_within(canonical_root, canonical_target);
}

std::uintmax_t tree_bytes(const std::filesystem::path &path) {
    std::error_code error;
    std::uintmax_t bytes = 0;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             path, std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) { error.clear(); continue; }
        if (entry.is_symlink(error)) { error.clear(); continue; }
        if (entry.is_regular_file(error)) {
            const auto size = entry.file_size(error);
            if (!error) bytes += size;
            else error.clear();
        }
    }
    return bytes;
}

CacheEraseStats erase_tree(const std::filesystem::path &path) {
    CacheEraseStats result;
    std::error_code error;
    if (!std::filesystem::exists(path, error)) return result;
    if (error) { result.rejected = 1; return result; }
    result.bytes_removed = tree_bytes(path);
    const auto removed = std::filesystem::remove_all(path, error);
    if (error) {
        ++result.rejected;
        result.entries_removed = 0;
        result.bytes_removed = 0;
    } else {
        result.entries_removed = removed;
    }
    return result;
}

} // namespace

CacheEraseStats erase_binding_cache(
    const std::filesystem::path &cache_root,
    const std::filesystem::path &atlas_directory,
    std::uint64_t material_id) {
    CacheEraseStats result;
    if (atlas_directory.empty() || material_id == 0 ||
        !safe_cache_target(cache_root, atlas_directory) ||
        atlas_directory.filename() != "atlases") {
        result.unsafe_path = true;
        return result;
    }
    std::error_code error;
    if (!std::filesystem::exists(atlas_directory, error)) return result;
    if (error) { result.rejected = 1; return result; }
    const auto prefix = hex(material_id) + "-";
    for (const auto &entry : std::filesystem::directory_iterator(atlas_directory, error)) {
        if (error) { ++result.rejected; break; }
        if (entry.is_symlink(error)) { error.clear(); ++result.rejected; continue; }
        if (!entry.is_regular_file(error)) { error.clear(); continue; }
        const auto filename = entry.path().filename().string();
        const bool generation = filename.starts_with(prefix) &&
            (entry.path().extension() == ".npatlas" || entry.path().extension() == ".tmp");
        if (!generation) continue;
        const auto size = entry.file_size(error);
        if (error) { error.clear(); ++result.rejected; continue; }
        if (std::filesystem::remove(entry.path(), error)) {
            ++result.entries_removed;
            result.bytes_removed += size;
        } else {
            ++result.rejected;
        }
        error.clear();
    }
    return result;
}

CacheEraseStats erase_scene_cache(
    const std::filesystem::path &cache_root,
    const std::filesystem::path &scene_directory) {
    if (!safe_cache_target(cache_root, scene_directory) ||
        !component_prefix(scene_directory, "scene-")) {
        CacheEraseStats result;
        result.unsafe_path = true;
        return result;
    }
    return erase_tree(scene_directory);
}

CacheEraseStats erase_all_caches(const std::filesystem::path &cache_root) {
    if (cache_root.empty() || cache_root.filename() != "NeuralPassCache") {
        CacheEraseStats result;
        result.unsafe_path = true;
        return result;
    }
    std::error_code error;
    const bool exists = std::filesystem::exists(cache_root, error);
    bool unsafe = static_cast<bool>(error);
    if (exists && !unsafe) {
        const auto status = std::filesystem::symlink_status(cache_root, error);
        unsafe = error || std::filesystem::is_symlink(status);
    }
    if (unsafe) {
        CacheEraseStats result;
        result.unsafe_path = true;
        return result;
    }
    return erase_tree(cache_root);
}

} // namespace neuralpass
