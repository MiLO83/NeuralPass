#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <vector>

namespace neuralpass {

enum class CacheMigrationDisposition {
    none,
    unique,
    ambiguous,
};

struct CacheMigrationCandidate {
    std::uint64_t game_build = 0;
    std::uint64_t scene_identity = 0;
    std::filesystem::path scene_directory;
    float overlap = 0.0f;
    std::size_t shared_bindings = 0;
    std::size_t known_bindings = 0;
};

struct CacheMigrationPlan {
    CacheMigrationDisposition disposition = CacheMigrationDisposition::none;
    std::vector<CacheMigrationCandidate> candidates;
};

struct CacheMigrationStats {
    std::filesystem::path target_scene;
    std::uintmax_t entries_copied = 0;
    std::uintmax_t bytes_copied = 0;
    bool unsafe_path = false;
    bool target_conflict = false;
    bool failed = false;
    std::string error;

    [[nodiscard]] bool succeeded() const noexcept {
        return !unsafe_path && !target_conflict && !failed && entries_copied != 0;
    }
};

// Searches only sibling game-* namespaces. A candidate must contain a valid
// scene manifest and enough restart-stable binding overlap to be considered.
// Equal best matches remain ambiguous until the user selects one.
[[nodiscard]] CacheMigrationPlan plan_cache_migration(
    const std::filesystem::path &cache_root,
    std::uint64_t current_game_build,
    std::span<const std::uint64_t> visible_binding_ids,
    float overlap_threshold = 0.5f);

// Copies one selected scene into the current game namespace through a temporary
// directory and a final rename. Existing targets and all symlinked inputs are
// rejected; no existing cache file is overwritten.
[[nodiscard]] CacheMigrationStats apply_cache_migration(
    const std::filesystem::path &cache_root,
    std::uint64_t current_game_build,
    const CacheMigrationCandidate &candidate);

} // namespace neuralpass
