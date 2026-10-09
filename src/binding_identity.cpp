#include "neuralpass/binding_identity.hpp"

#include <algorithm>
#include <vector>

namespace neuralpass {
namespace {

constexpr std::uint64_t k_offset = 1469598103934665603ull;
constexpr std::uint64_t k_prime = 1099511628211ull;

void hash_u64(std::uint64_t &hash, std::uint64_t value) noexcept {
    for (unsigned int shift = 0; shift != 64; shift += 8) {
        hash ^= static_cast<std::uint8_t>(value >> shift);
        hash *= k_prime;
    }
}

} // namespace

BindingInstanceKey make_binding_instance_key(
    std::span<const std::uint64_t> pipeline_fingerprints,
    std::span<const DescriptorIdentity> descriptors,
    bool pipeline_identity_complete) {
    if (pipeline_fingerprints.empty() || descriptors.empty()) return {};

    std::vector<std::uint64_t> pipelines(pipeline_fingerprints.begin(),
                                         pipeline_fingerprints.end());
    std::sort(pipelines.begin(), pipelines.end());
    pipelines.erase(std::unique(pipelines.begin(), pipelines.end()), pipelines.end());

    std::vector<DescriptorIdentity> canonical(descriptors.begin(), descriptors.end());
    std::sort(canonical.begin(), canonical.end(), [](const auto &left, const auto &right) {
        if (left.slot != right.slot) return left.slot < right.slot;
        return left.resource < right.resource;
    });

    std::uint64_t hash = k_offset;
    hash_u64(hash, 0x4e5042494e44494eull); // "NPBINDIN" domain separator
    hash_u64(hash, pipelines.size());
    for (const auto pipeline : pipelines) hash_u64(hash, pipeline);
    hash_u64(hash, canonical.size());
    bool resources_stable = true;
    for (const auto &descriptor : canonical) {
        if (descriptor.resource == 0) continue;
        hash_u64(hash, descriptor.slot);
        hash_u64(hash, descriptor.resource);
        resources_stable = resources_stable && descriptor.restart_stable;
    }
    if (hash == 0) hash = 1;
    return {hash, pipeline_identity_complete && resources_stable};
}

} // namespace neuralpass
