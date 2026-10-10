#pragma once

#include <cstdint>
#include <filesystem>

namespace neuralpass {

struct CacheEraseStats {
    std::uintmax_t entries_removed = 0;
    std::uintmax_t bytes_removed = 0;
    std::uintmax_t rejected = 0;
    bool unsafe_path = false;
};

// Removes every generation (including interrupted temporary writes) for one
// material from one exact atlas namespace. Unrelated files are untouched.
[[nodiscard]] CacheEraseStats erase_binding_cache(
    const std::filesystem::path &cache_root,
    const std::filesystem::path &atlas_directory,
    std::uint64_t material_id);

// Scene deletion is accepted only for a scene-* directory lexically below the
// supplied NeuralPassCache root.
[[nodiscard]] CacheEraseStats erase_scene_cache(
    const std::filesystem::path &cache_root,
    const std::filesystem::path &scene_directory);

// Global deletion deliberately requires the final directory component to be
// NeuralPassCache, preventing a malformed caller from targeting a broad root.
[[nodiscard]] CacheEraseStats erase_all_caches(
    const std::filesystem::path &cache_root);

} // namespace neuralpass
