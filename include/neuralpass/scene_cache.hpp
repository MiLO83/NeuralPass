#pragma once

#include <cstdint>
#include <filesystem>
#include <span>

namespace neuralpass {

struct SceneCacheSelection {
    std::uint64_t identity = 0;
    std::filesystem::path directory;
    bool matched_existing = false;

    [[nodiscard]] bool valid() const noexcept { return identity != 0; }
};

// Persistent scene namespaces are selected only from restart-stable binding
// identities. Manifests accumulate the bindings observed from different views;
// a returning view may therefore match without reproducing its first camera.
class SceneCacheCatalog {
public:
    explicit SceneCacheCatalog(std::filesystem::path root,
                               float overlap_threshold = 0.25f);

    [[nodiscard]] SceneCacheSelection resolve(
        std::span<const std::uint64_t> visible_binding_ids) const;
    [[nodiscard]] bool record(
        std::uint64_t scene_identity,
        std::span<const std::uint64_t> visible_binding_ids) const;

private:
    std::filesystem::path root_;
    float overlap_threshold_ = 0.25f;
};

} // namespace neuralpass
