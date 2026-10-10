#include "neuralpass/cache_migration.hpp"

#include "neuralpass/scene_cache.hpp"

#include <algorithm>
#include <charconv>
#include <chrono>
#include <cmath>
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

bool parse_named_identity(const std::filesystem::path &path, const char *prefix,
                          std::uint64_t &identity) {
    const auto name = path.filename().string();
    const std::string expected(prefix);
    if (!name.starts_with(expected) || name.size() != expected.size() + 16) return false;
    const auto first = name.data() + expected.size();
    const auto last = name.data() + name.size();
    const auto parsed = std::from_chars(first, last, identity, 16);
    return parsed.ec == std::errc {} && parsed.ptr == last && identity != 0;
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

bool safe_existing_path(const std::filesystem::path &cache_root,
                        const std::filesystem::path &target) {
    if (cache_root.filename() != "NeuralPassCache" ||
        !lexically_within(cache_root, target)) return false;
    std::error_code error;
    if (std::filesystem::is_symlink(std::filesystem::symlink_status(cache_root, error)) || error)
        return false;
    const auto canonical_root = std::filesystem::weakly_canonical(cache_root, error);
    if (error) return false;
    const auto canonical_target = std::filesystem::weakly_canonical(target, error);
    return !error && lexically_within(canonical_root, canonical_target);
}

bool symlink_or_status_error_if_present(const std::filesystem::path &path) {
    std::error_code error;
    const bool exists = std::filesystem::exists(path, error);
    if (error) return true;
    if (!exists) return false;
    const auto status = std::filesystem::symlink_status(path, error);
    return static_cast<bool>(error) || std::filesystem::is_symlink(status);
}

std::vector<std::uint64_t> canonical(std::span<const std::uint64_t> ids) {
    std::vector<std::uint64_t> result;
    result.reserve(ids.size());
    for (const auto id : ids) if (id != 0) result.push_back(id);
    std::sort(result.begin(), result.end());
    result.erase(std::unique(result.begin(), result.end()), result.end());
    return result;
}

std::size_t shared_count(const std::vector<std::uint64_t> &left,
                         const std::vector<std::uint64_t> &right) {
    std::size_t shared = 0, a = 0, b = 0;
    while (a < left.size() && b < right.size()) {
        if (left[a] == right[b]) { ++shared; ++a; ++b; }
        else if (left[a] < right[b]) ++a;
        else ++b;
    }
    return shared;
}

bool source_shape_matches(const std::filesystem::path &cache_root,
                          const CacheMigrationCandidate &candidate) {
    std::uint64_t scene = 0, game = 0;
    const auto scenes = candidate.scene_directory.parent_path();
    const auto game_directory = scenes.parent_path();
    return scenes.filename() == "scenes" &&
        parse_named_identity(candidate.scene_directory, "scene-", scene) &&
        parse_named_identity(game_directory, "game-", game) &&
        scene == candidate.scene_identity && game == candidate.game_build &&
        game_directory.parent_path().lexically_normal() == cache_root.lexically_normal();
}

} // namespace

CacheMigrationPlan plan_cache_migration(
    const std::filesystem::path &cache_root,
    std::uint64_t current_game_build,
    std::span<const std::uint64_t> visible_binding_ids,
    float overlap_threshold) {
    CacheMigrationPlan result;
    const auto visible = canonical(visible_binding_ids);
    if (cache_root.filename() != "NeuralPassCache" || current_game_build == 0 ||
        visible.empty() || !std::isfinite(overlap_threshold) ||
        overlap_threshold <= 0.0f || overlap_threshold > 1.0f)
        return result;
    std::error_code error;
    if (!std::filesystem::exists(cache_root, error) || error ||
        !safe_existing_path(cache_root, cache_root / "probe")) return result;

    for (const auto &game_entry : std::filesystem::directory_iterator(cache_root, error)) {
        if (error) break;
        if (game_entry.is_symlink(error)) { error.clear(); continue; }
        if (!game_entry.is_directory(error)) { error.clear(); continue; }
        std::uint64_t game_build = 0;
        if (!parse_named_identity(game_entry.path(), "game-", game_build) ||
            game_build == current_game_build) continue;
        const auto scenes = game_entry.path() / "scenes";
        if (std::filesystem::is_symlink(std::filesystem::symlink_status(scenes, error))) {
            error.clear();
            continue;
        }
        if (error) { error.clear(); continue; }
        if (!safe_existing_path(cache_root, scenes)) continue;
        for (auto &record : SceneCacheCatalog(scenes, overlap_threshold).records()) {
            const auto shared = shared_count(visible, record.bindings);
            // Cross-build reuse is deliberately stricter than same-build scene
            // recognition: one coincidental binding must not validate an otherwise
            // unrelated scene. Jaccard overlap requires evidence in both sets.
            const auto denominator = visible.size() + record.bindings.size() - shared;
            if (denominator == 0) continue;
            const float score = static_cast<float>(shared) /
                                static_cast<float>(denominator);
            if (score < overlap_threshold) continue;
            result.candidates.push_back({game_build, record.identity,
                std::move(record.directory), score, shared, record.bindings.size()});
        }
    }
    if (result.candidates.empty()) return result;
    std::sort(result.candidates.begin(), result.candidates.end(),
        [](const auto &left, const auto &right) {
            if (left.overlap != right.overlap) return left.overlap > right.overlap;
            if (left.shared_bindings != right.shared_bindings)
                return left.shared_bindings > right.shared_bindings;
            if (left.game_build != right.game_build) return left.game_build < right.game_build;
            if (left.scene_identity != right.scene_identity)
                return left.scene_identity < right.scene_identity;
            return left.scene_directory < right.scene_directory;
        });
    const auto best_overlap = result.candidates.front().overlap;
    const auto best_shared = result.candidates.front().shared_bindings;
    result.candidates.erase(std::remove_if(result.candidates.begin(), result.candidates.end(),
        [&](const auto &candidate) {
            return std::abs(candidate.overlap - best_overlap) > 0.000001f ||
                   candidate.shared_bindings != best_shared;
        }), result.candidates.end());
    result.disposition = result.candidates.size() == 1
        ? CacheMigrationDisposition::unique
        : CacheMigrationDisposition::ambiguous;
    return result;
}

CacheMigrationStats apply_cache_migration(
    const std::filesystem::path &cache_root,
    std::uint64_t current_game_build,
    const CacheMigrationCandidate &candidate) {
    CacheMigrationStats result;
    if (current_game_build == 0 || candidate.game_build == current_game_build ||
        candidate.game_build == 0 || candidate.scene_identity == 0 ||
        !source_shape_matches(cache_root, candidate) ||
        !safe_existing_path(cache_root, candidate.scene_directory)) {
        result.unsafe_path = true;
        return result;
    }
    const auto target_scenes = cache_root / ("game-" + hex(current_game_build)) / "scenes";
    result.target_scene = target_scenes / ("scene-" + hex(candidate.scene_identity));
    std::error_code error;
    const auto source_status = std::filesystem::symlink_status(
        candidate.scene_directory, error);
    if (error || std::filesystem::is_symlink(source_status) ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(
            candidate.scene_directory.parent_path(), error)) || error ||
        std::filesystem::is_symlink(std::filesystem::symlink_status(
            candidate.scene_directory.parent_path().parent_path(), error)) || error ||
        symlink_or_status_error_if_present(target_scenes.parent_path()) ||
        symlink_or_status_error_if_present(target_scenes) ||
        !safe_existing_path(cache_root, result.target_scene)) {
        result.unsafe_path = true;
        return result;
    }
    if (!std::filesystem::exists(candidate.scene_directory, error) || error) {
        result.failed = true;
        result.error = error ? error.message() : "source scene does not exist";
        return result;
    }
    if (std::filesystem::exists(result.target_scene, error) || error) {
        result.target_conflict = true;
        return result;
    }

    // Validate the complete source before creating any destination. Reparse
    // points are rejected rather than copied or followed.
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             candidate.scene_directory,
             std::filesystem::directory_options::skip_permission_denied, error)) {
        if (error) { result.failed = true; result.error = error.message(); return result; }
        const auto status = entry.symlink_status(error);
        if (error || std::filesystem::is_symlink(status) ||
            (!std::filesystem::is_directory(status) &&
             !std::filesystem::is_regular_file(status))) {
            result.unsafe_path = true;
            return result;
        }
    }
    if (error) {
        result.failed = true;
        result.error = "validate source tree: " + error.message();
        return result;
    }

    std::filesystem::create_directories(target_scenes, error);
    if (error) { result.failed = true; result.error = "create target root: " + error.message(); return result; }
    const auto nonce = static_cast<std::uint64_t>(
        std::chrono::steady_clock::now().time_since_epoch().count());
    // Keep staging no longer than the final scene component so a valid source
    // path cannot cross legacy Windows MAX_PATH merely because it is migrated.
    const auto temporary = target_scenes / (".m-" + hex(nonce));
    std::filesystem::create_directory(temporary, error);
    if (error) { result.failed = true; result.error = "create staging root: " + error.message(); return result; }
    result.entries_copied = 1;
    std::filesystem::path failed_entry;
    std::filesystem::path failed_destination;
    for (const auto &entry : std::filesystem::recursive_directory_iterator(
             candidate.scene_directory, error)) {
        if (error) { failed_entry = entry.path(); break; }
        const auto relative = entry.path().lexically_relative(candidate.scene_directory);
        const auto destination = std::filesystem::path(temporary) / relative;
        if (entry.is_directory(error)) {
            std::filesystem::create_directories(destination, error);
            if (!error) ++result.entries_copied;
        } else if (entry.is_regular_file(error)) {
            std::filesystem::create_directories(destination.parent_path(), error);
            if (!error) {
                const auto size = entry.file_size(error);
                if (!error && std::filesystem::copy_file(entry.path(), destination,
                        std::filesystem::copy_options::none, error)) {
                    ++result.entries_copied;
                    result.bytes_copied += size;
                }
            }
        }
        if (error) { failed_entry = entry.path(); failed_destination = destination; break; }
    }
    const bool copy_failed = static_cast<bool>(error);
    if (!copy_failed) std::filesystem::rename(temporary, result.target_scene, error);
    if (error) {
        result.error = copy_failed
            ? "copy " + failed_entry.string() + " to " + failed_destination.string() +
                ": " + error.message()
            : "commit staging directory: " + error.message();
        std::error_code cleanup_error;
        std::filesystem::remove_all(temporary, cleanup_error);
        result.entries_copied = 0;
        result.bytes_copied = 0;
        result.failed = true;
    }
    return result;
}

} // namespace neuralpass
