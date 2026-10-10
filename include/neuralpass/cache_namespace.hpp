#pragma once

#include <cstdint>
#include <filesystem>

namespace neuralpass {

struct CacheNamespace {
    std::uint64_t game_build = 0;
    std::uint64_t style = 0;
    std::uint64_t model = 0;

    [[nodiscard]] bool valid() const noexcept {
        return game_build != 0 && style != 0 && model != 0;
    }
    [[nodiscard]] std::filesystem::path scene_catalog_root(
        const std::filesystem::path &cache_root) const;
    [[nodiscard]] std::filesystem::path atlas_root(
        const std::filesystem::path &scene_directory) const;
};

} // namespace neuralpass
