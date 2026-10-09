#pragma once

#include <cstdint>
#include <span>

namespace neuralpass {

// Restart-stable input to the cross-API pipeline-plus-descriptor identity.
// `slot` is the semantic layout parameter/binding/array tuple; it must not
// contain a transient native handle.
struct DescriptorIdentity {
    std::uint64_t slot = 0;
    std::uint64_t resource = 0;
    bool restart_stable = false;
};

struct BindingInstanceKey {
    std::uint64_t value = 0;
    bool restart_stable = false;

    [[nodiscard]] bool valid() const noexcept { return value != 0; }
};

// Canonicalizes descriptor order but preserves slot placement. This prevents
// materials that use the same resources in different bindings or pipelines
// from sharing texture coverage.
[[nodiscard]] BindingInstanceKey make_binding_instance_key(
    std::span<const std::uint64_t> pipeline_fingerprints,
    std::span<const DescriptorIdentity> descriptors,
    bool pipeline_identity_complete);

} // namespace neuralpass
