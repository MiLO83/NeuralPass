#include "neuralpass/cache_namespace.hpp"

#include <stdexcept>
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

void require_valid(const CacheNamespace &value) {
    if (!value.valid()) throw std::invalid_argument("cache namespace identities must be nonzero");
}

} // namespace

std::filesystem::path CacheNamespace::scene_catalog_root(
        const std::filesystem::path &cache_root) const {
    require_valid(*this);
    return cache_root / ("game-" + hex(game_build)) / "scenes";
}

std::filesystem::path CacheNamespace::atlas_root(
        const std::filesystem::path &scene_directory) const {
    require_valid(*this);
    return scene_directory / "styles" / ("style-" + hex(style)) /
           ("model-" + hex(model)) / "atlases";
}

} // namespace neuralpass
