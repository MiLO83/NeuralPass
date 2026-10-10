#define ImTextureID ImU64
#include <imgui.h>
#include <reshade.hpp>
#include <descriptor_tracking.hpp>

#include "d3d9_surface_capture.hpp"
#include "d3d10_surface_capture.hpp"
#include "d3d11_surface_capture.hpp"
#include "d3d12_surface_capture.hpp"
#include "vulkan_surface_capture.hpp"
#include "neuralpass/adaptive_budget.hpp"
#include "neuralpass/binding_identity.hpp"
#include "neuralpass/binding_style.hpp"
#include "neuralpass/cache_namespace.hpp"
#include "neuralpass/color_pipeline.hpp"
#include "neuralpass/inference.hpp"
#include "neuralpass/runtime_evidence.hpp"
#include "neuralpass/scene_cache.hpp"
#include "neuralpass/texture_baker.hpp"
#include "neuralpass/temporal.hpp"
#include "neuralpass/tile_scheduler.hpp"
#include "neuralpass/visibility.hpp"

#include <Windows.h>
#include <algorithm>
#include <atomic>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <cstdlib>
#include <deque>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <mutex>
#include <optional>
#include <stdexcept>
#include <string>
#include <thread>
#include <tuple>
#include <unordered_map>
#include <unordered_set>
#include <vector>

using neuralpass::Color;
using neuralpass::HistoryFrame;
using neuralpass::Image;

namespace {

std::filesystem::path g_addon_directory;
std::uint64_t g_game_build_identity = 1;
std::mutex g_baker_probe_mutex;
constexpr std::uint64_t k_fnv_offset = 1469598103934665603ull;
constexpr std::uint64_t k_fnv_prime = 1099511628211ull;

void hash_bytes(std::uint64_t &hash, const void *data, std::size_t size) {
    if (data == nullptr || size == 0) return;
    const auto *bytes = static_cast<const std::uint8_t *>(data);
    // Creation uploads can be very large. Sampling at a deterministic stride
    // makes identity collection bounded while still covering the whole image.
    constexpr std::size_t k_max_samples = 64 * 1024;
    const auto stride = std::max<std::size_t>(1, (size + k_max_samples - 1) / k_max_samples);
    for (std::size_t offset = 0; offset < size; offset += stride) {
        hash ^= bytes[offset];
        hash *= k_fnv_prime;
    }
    hash ^= static_cast<std::uint64_t>(size);
    hash *= k_fnv_prime;
}

template <typename Value>
void hash_value(std::uint64_t &hash, const Value &value) {
    hash_bytes(hash, &value, sizeof(value));
}

std::uint64_t nonzero_hash(std::uint64_t hash) {
    return hash == 0 ? 1 : hash;
}

void hash_string(std::uint64_t &hash, std::string_view value) {
    hash_bytes(hash, value.data(), value.size());
}

std::uint64_t file_content_identity(const std::filesystem::path &path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return 1;
    std::uint64_t hash = k_fnv_offset;
    std::array<char, 64 * 1024> block {};
    while (input) {
        input.read(block.data(), static_cast<std::streamsize>(block.size()));
        const auto count = static_cast<std::size_t>(input.gcount());
        hash_bytes(hash, block.data(), count);
    }
    return nonzero_hash(hash);
}

std::uint64_t game_build_identity() {
    std::array<wchar_t, 32768> path_buffer {};
    const DWORD length = GetModuleFileNameW(nullptr, path_buffer.data(),
                                             static_cast<DWORD>(path_buffer.size()));
    if (length == 0 || length == path_buffer.size()) return 1;
    const std::filesystem::path path(std::wstring_view(path_buffer.data(), length));
    std::uint64_t hash = k_fnv_offset;
    const auto name = path.filename().wstring();
    hash_bytes(hash, name.data(), name.size() * sizeof(wchar_t));
    std::error_code error;
    const auto size = std::filesystem::file_size(path, error);
    if (!error) hash_value(hash, size);
    error.clear();
    const auto modified = std::filesystem::last_write_time(path, error);
    if (!error) {
        const auto ticks = modified.time_since_epoch().count();
        hash_value(hash, ticks);
    }
    return nonzero_hash(hash);
}

neuralpass::CacheNamespace cache_namespace_for(
        std::string_view preset, std::string_view backend_name,
        std::uint64_t binding_control_identity) {
    std::uint64_t style = k_fnv_offset;
    hash_string(style, "fixed-style-v1");
    hash_string(style, preset);
    // Preserve the pre-control namespace for the all-default configuration so
    // an upgrade does not discard compatible atlases. Only a real override
    // extends the style identity.
    if (binding_control_identity != neuralpass::binding_style_control_identity({})) {
        hash_string(style, "binding-controls-v1");
        hash_value(style, binding_control_identity);
    }
    const auto model_path = g_addon_directory / "models" / "downloads" /
                            (std::string(preset) + "-9.onnx");
    std::uint64_t model = k_fnv_offset;
    hash_string(model, backend_name);
    if (std::filesystem::exists(model_path)) {
        const auto content = file_content_identity(model_path);
        hash_value(model, content);
    } else {
        hash_string(model, "builtin-preview-v1");
    }
    return {g_game_build_identity, nonzero_hash(style), nonzero_hash(model)};
}

struct ResourceFingerprint {
    std::uint64_t value = 0;
    bool content_backed = false;
};

std::unordered_set<std::uint64_t> g_uv_pipelines;
struct PipelineCaptureInfo {
    reshade::api::pipeline_layout layout = {};
    std::uint32_t uv_binding = 0;
    std::uint32_t uv_offset = 0;
    std::uint32_t uv_stride = 0;
    reshade::api::format uv_format = reshade::api::format::unknown;
    bool has_position = false;
};
std::unordered_map<std::uint64_t, PipelineCaptureInfo> g_pipeline_capture_info;
std::unordered_map<std::uint64_t, neuralpass::capture::UvInput> g_vertex_uv_outputs;
std::unordered_map<std::uint64_t, std::uint64_t> g_pipeline_fingerprints;
std::unordered_map<std::uint64_t, ResourceFingerprint> g_resource_fingerprints;
std::unordered_map<std::uint64_t, reshade::api::resource_usage> g_resource_states;
std::unordered_set<std::uint64_t> g_vertex_buffers_probed;
std::atomic_uint64_t g_pipelines_seen = 0;
std::atomic_uint64_t g_uv_pipelines_seen = 0;
std::atomic_uint64_t g_explicit_uv_pipelines_seen = 0;
std::atomic_uint64_t g_inferred_uv_pipelines_seen = 0;
std::atomic_uint64_t g_draws_seen = 0;
std::atomic_uint64_t g_uv_draws_seen = 0;
std::atomic_uint64_t g_uv_vertices_seen = 0;
std::atomic_uint64_t g_pretransform_probe_attempts = 0;
std::atomic_uint64_t g_pretransform_probe_successes = 0;
std::atomic_uint64_t g_pretransform_uv_samples = 0;
std::atomic_uint64_t g_material_draws_seen = 0;
std::atomic_uint64_t g_material_texture_candidates = 0;
std::atomic_uint64_t g_restart_stable_material_draws = 0;
std::atomic_uint64_t g_session_material_draws = 0;
std::atomic_uint64_t g_surface_frames_captured = 0;
std::atomic_uint64_t g_surface_pixels_captured = 0;
std::atomic_uint64_t g_swapchain_resets = 0;
std::unordered_set<std::uint64_t> g_materials_seen;
std::unordered_set<std::uint64_t> g_restart_stable_materials;
std::unordered_map<std::uint64_t, int> g_source_slot_overrides;
std::unordered_set<std::uint64_t> g_source_overrides_loaded;
neuralpass::BindingStyleControls g_binding_style_controls;
std::unordered_set<std::uint64_t> g_binding_style_controls_loaded;
std::atomic_uint64_t g_binding_style_revision = 1;
std::unordered_map<reshade::api::device *,
    std::unique_ptr<neuralpass::capture::SurfaceCaptureBackend>> g_surface_captures;
std::unordered_map<void *, reshade::api::color_space> g_swapchain_color_spaces;
std::mutex g_runtime_evidence_mutex;

constexpr char k_source_slot_section[] = "NeuralPass.SourceSlots";
constexpr char k_binding_strength_section[] = "NeuralPass.BindingStrength";

std::string source_slot_key(std::uint64_t binding) {
    return std::to_string(binding);
}

void persist_source_slot(std::uint64_t binding, int slot) {
    const auto key = source_slot_key(binding);
    reshade::set_config_value(nullptr, k_source_slot_section, key.c_str(), slot);
}

void load_source_slot_once(std::uint64_t binding) {
    bool should_load = false;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        should_load = g_source_overrides_loaded.insert(binding).second;
    }
    if (!should_load) return;
    int slot = -1;
    const auto key = source_slot_key(binding);
    if (reshade::get_config_value(nullptr, k_source_slot_section, key.c_str(), slot) &&
        slot >= 0 && slot < static_cast<int>(D3D11_COMMONSHADER_INPUT_RESOURCE_SLOT_COUNT)) {
        std::lock_guard lock(g_baker_probe_mutex);
        g_source_slot_overrides[binding] = slot;
    }
}

void persist_binding_strength(std::uint64_t binding, int strength) {
    const auto key = source_slot_key(binding);
    reshade::set_config_value(nullptr, k_binding_strength_section, key.c_str(), strength);
}

void load_binding_strength_once(std::uint64_t binding) {
    bool should_load = false;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        should_load = g_binding_style_controls_loaded.insert(binding).second;
    }
    if (!should_load) return;
    int strength = 100;
    const auto key = source_slot_key(binding);
    if (reshade::get_config_value(nullptr, k_binding_strength_section, key.c_str(), strength) &&
        strength >= 0 && strength < 100) {
        std::lock_guard lock(g_baker_probe_mutex);
        g_binding_style_controls[binding] = static_cast<std::uint8_t>(strength);
        ++g_binding_style_revision;
    }
}

struct BakerVertexBinding {
    reshade::api::resource buffer = {};
    std::uint64_t offset = 0;
    std::uint32_t stride = 0;
};

struct BakerPixelResource {
    reshade::api::resource_view view = {};
    std::uint32_t dx_register_index = 0;
    std::uint32_t dx_register_space = 0;
    reshade::api::descriptor_table table = {};
    std::uint32_t param = 0;
    std::uint32_t binding = 0;
    std::uint32_t array_offset = 0;
    reshade::api::descriptor_type type = reshade::api::descriptor_type::shader_resource_view;
};

struct BakerPixelSampler {
    reshade::api::sampler sampler = {};
    std::uint32_t dx_register_index = 0;
    std::uint32_t dx_register_space = 0;
    reshade::api::descriptor_table table = {};
    std::uint32_t param = 0;
    std::uint32_t binding = 0;
    std::uint32_t array_offset = 0;
    reshade::api::descriptor_type type = reshade::api::descriptor_type::sampler;
};

struct BakerDescriptorTable {
    reshade::api::descriptor_table table = {};
    bool has_dynamic_offsets = false;
};

struct __declspec(uuid("69062FC3-7159-466E-A9B2-4E505542414B")) BakerCommandState {
    // 'pipeline' is the input-layout/combined pipeline that owns the UV
    // declaration. Legacy D3D APIs bind shaders as separate pipeline objects.
    reshade::api::pipeline pipeline = {};
    reshade::api::pipeline vertex_pipeline = {};
    reshade::api::pipeline pixel_pipeline = {};
    reshade::api::pipeline_layout layout = {};
    std::array<BakerVertexBinding, 16> vertex_buffers {};
    reshade::api::resource index_buffer = {};
    std::uint64_t index_offset = 0;
    std::uint32_t index_size = 0;
    std::array<reshade::api::resource_view, 8> render_targets {};
    std::uint32_t render_target_count = 0;
    reshade::api::resource_view depth_stencil = {};
    bool inside_render_pass = false;
    reshade::api::primitive_topology primitive_topology =
        reshade::api::primitive_topology::undefined;
    std::unordered_map<std::uint64_t, BakerPixelResource> pixel_resources;
    std::unordered_map<std::uint64_t, BakerPixelSampler> pixel_samplers;
    std::unordered_map<std::uint32_t, BakerDescriptorTable> descriptor_tables;
};
constexpr std::array<const char *, 5> k_presets {
    "candy", "mosaic", "rain-princess", "udnie", "photo-detail"
};
constexpr std::uint32_t k_stream_width = 512;
constexpr std::uint32_t k_stream_height = 288;
constexpr char k_default_prompt[] =
    "Santa's North Pole Workshop, the same scene redesigned as a magical Christmas toy factory, "
    "red velvet, green enamel, brass toy machinery, candy-cane accents, snow, frost, warm golden "
    "fairy lights, cinematic realistic video game graphics, preserve silhouettes and composition";

#pragma pack(push, 1)
struct BridgeHeader {
    char magic[4] {'N', 'P', 'F', '1'};
    std::uint64_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
};
struct BridgeOutputHeader {
    char magic[4] {'N', 'P', 'F', '2'};
    std::uint64_t sequence = 0;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::uint32_t frame_count = 0;
};
#pragma pack(pop)

struct CapturedFrame {
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::vector<std::uint8_t> rgba;
};

enum class ManualSceneCommand : int {
    none,
    keep_current,
    start_new,
    merge_visible,
};

void on_baker_init_pipeline(reshade::api::device *device, reshade::api::pipeline_layout layout,
                            std::uint32_t subobject_count,
                            const reshade::api::pipeline_subobject *subobjects,
                            reshade::api::pipeline pipeline) {
    ++g_pipelines_seen;
    bool has_explicit_uv = false;
    bool has_inferred_uv = false;
    neuralpass::capture::UvInput declared_uv;
    neuralpass::capture::UvInput inferred_uv;
    std::uint64_t pipeline_fingerprint = k_fnv_offset;
    PipelineCaptureInfo capture_info;
    capture_info.layout = layout;
    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto &subobject = subobjects[index];
        hash_value(pipeline_fingerprint, subobject.type);
        if (subobject.type == reshade::api::pipeline_subobject_type::vertex_shader ||
            subobject.type == reshade::api::pipeline_subobject_type::pixel_shader) {
            const auto *shaders = static_cast<const reshade::api::shader_desc *>(subobject.data);
            for (std::uint32_t shader = 0; shader < subobject.count; ++shader) {
                hash_bytes(pipeline_fingerprint, shaders[shader].code, shaders[shader].code_size);
                if (shaders[shader].entry_point != nullptr)
                    hash_bytes(pipeline_fingerprint, shaders[shader].entry_point,
                               std::strlen(shaders[shader].entry_point));
            }
            if ((device->get_api() == reshade::api::device_api::d3d9 ||
                 device->get_api() == reshade::api::device_api::d3d10 ||
                 device->get_api() == reshade::api::device_api::d3d11 ||
                 device->get_api() == reshade::api::device_api::d3d12) &&
                subobject.type == reshade::api::pipeline_subobject_type::vertex_shader &&
                subobject.count != 0) {
                const auto uv = device->get_api() == reshade::api::device_api::d3d9
                    ? neuralpass::d3d9_capture::inspect_uv_output(
                        shaders[0].code, shaders[0].code_size)
                    : neuralpass::d3d11_capture::inspect_uv_output(
                        shaders[0].code, shaders[0].code_size);
                if (uv.valid()) {
                    std::lock_guard lock(g_baker_probe_mutex);
                    g_vertex_uv_outputs[pipeline.handle] = uv;
                }
            }
        }
        if (subobject.type == reshade::api::pipeline_subobject_type::input_layout) {
            const auto *elements = static_cast<const reshade::api::input_element *>(subobject.data);
            for (std::uint32_t element = 0; element < subobject.count; ++element) {
                if (elements[element].semantic != nullptr)
                    hash_bytes(pipeline_fingerprint, elements[element].semantic,
                               std::strlen(elements[element].semantic));
                hash_value(pipeline_fingerprint, elements[element].location);
                hash_value(pipeline_fingerprint, elements[element].format);
                hash_value(pipeline_fingerprint, elements[element].buffer_binding);
                hash_value(pipeline_fingerprint, elements[element].offset);
                hash_value(pipeline_fingerprint, elements[element].stride);
                hash_value(pipeline_fingerprint, elements[element].instance_step_rate);
                if (elements[element].semantic != nullptr &&
                    _stricmp(elements[element].semantic, "POSITION") == 0)
                    capture_info.has_position = true;
                if (elements[element].semantic != nullptr &&
                    _stricmp(elements[element].semantic, "TEXCOORD") == 0) {
                    has_explicit_uv = true;
                    declared_uv.name = elements[element].semantic;
                    declared_uv.index = elements[element].semantic_index;
                    capture_info.uv_binding = elements[element].buffer_binding;
                    capture_info.uv_offset = elements[element].offset;
                    capture_info.uv_stride = elements[element].stride;
                    capture_info.uv_format = elements[element].format;
                } else if (elements[element].semantic == nullptr &&
                           elements[element].instance_step_rate == 0 &&
                           elements[element].location != 0) {
                    // Vulkan and OpenGL use attribute locations instead of HLSL
                    // semantics. Treat a non-position two-component attribute as
                    // a UV candidate, then confirm it during shader instrumentation.
                    switch (elements[element].format) {
                    case reshade::api::format::r8g8_unorm:
                    case reshade::api::format::r16g16_float:
                    case reshade::api::format::r16g16_unorm:
                    case reshade::api::format::r16g16_snorm:
                    case reshade::api::format::r32g32_float:
                        has_inferred_uv = true;
                        inferred_uv.location = elements[element].location;
                        capture_info.uv_binding = elements[element].buffer_binding;
                        capture_info.uv_offset = elements[element].offset;
                        capture_info.uv_stride = elements[element].stride;
                        capture_info.uv_format = elements[element].format;
                        break;
                    default:
                        break;
                    }
                }
            }
        }
    }
    const bool has_mesh_uv = has_explicit_uv || has_inferred_uv;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        g_pipeline_fingerprints[pipeline.handle] = pipeline_fingerprint;
        // Shader model 6 DXIL reflection requires dxcompiler, which is not
        // guaranteed to ship beside a game. The input declaration is a safe
        // D3D12 fallback: incompatible VS output semantics simply make capture
        // PSO creation fail without touching the application draw.
        if (device->get_api() == reshade::api::device_api::d3d12 &&
            declared_uv.valid() && !g_vertex_uv_outputs.contains(pipeline.handle))
            g_vertex_uv_outputs[pipeline.handle] = declared_uv;
        if (device->get_api() == reshade::api::device_api::vulkan &&
            inferred_uv.valid() && !g_vertex_uv_outputs.contains(pipeline.handle))
            g_vertex_uv_outputs[pipeline.handle] = inferred_uv;
    }
    // D3D9/10/11 create and bind input layouts, VS and PS as independent
    // pipeline objects. D3D12 and Vulkan generally include them in one object.
    // The UV declaration alone is sufficient to mark its owning object here.
    if (has_mesh_uv) {
        std::lock_guard lock(g_baker_probe_mutex);
        if (g_uv_pipelines.insert(pipeline.handle).second) {
            ++g_uv_pipelines_seen;
            if (has_explicit_uv) ++g_explicit_uv_pipelines_seen;
            else ++g_inferred_uv_pipelines_seen;
        }
        g_pipeline_capture_info[pipeline.handle] = capture_info;
    }
    if (device->get_api() == reshade::api::device_api::d3d12) {
        std::lock_guard lock(g_baker_probe_mutex);
        const auto capture = g_surface_captures.find(device);
        if (capture != g_surface_captures.end())
            if (auto *d3d12 = dynamic_cast<neuralpass::d3d12_capture::SurfaceCapture *>(
                    capture->second.get()))
                d3d12->register_pipeline(layout, subobject_count, subobjects, pipeline);
    } else if (device->get_api() == reshade::api::device_api::vulkan) {
        std::lock_guard lock(g_baker_probe_mutex);
        const auto capture = g_surface_captures.find(device);
        if (capture != g_surface_captures.end())
            if (auto *vulkan = dynamic_cast<neuralpass::vulkan_capture::SurfaceCapture *>(
                    capture->second.get()))
                vulkan->register_pipeline(layout, subobject_count, subobjects, pipeline);
    }
}

std::uint64_t resource_descriptor_fingerprint(const reshade::api::resource_desc &desc) {
    std::uint64_t hash = k_fnv_offset;
    hash_value(hash, desc.type);
    if (desc.type == reshade::api::resource_type::buffer) {
        hash_value(hash, desc.buffer.size);
        hash_value(hash, desc.buffer.structured.stride);
    } else {
        hash_value(hash, desc.texture.width);
        hash_value(hash, desc.texture.height);
        hash_value(hash, desc.texture.depth_or_layers);
        hash_value(hash, desc.texture.levels);
        hash_value(hash, desc.texture.format);
        hash_value(hash, desc.texture.samples);
    }
    hash_value(hash, desc.flags);
    return hash;
}

void on_baker_init_resource(reshade::api::device *, const reshade::api::resource_desc &desc,
                            const reshade::api::subresource_data *initial_data,
                            reshade::api::resource_usage initial_state,
                            reshade::api::resource resource) {
    if (desc.type != reshade::api::resource_type::buffer &&
        desc.type != reshade::api::resource_type::texture_1d &&
        desc.type != reshade::api::resource_type::texture_2d &&
        desc.type != reshade::api::resource_type::texture_3d) return;
    ResourceFingerprint fingerprint {resource_descriptor_fingerprint(desc), false};
    if (initial_data != nullptr && initial_data[0].data != nullptr) {
        if (desc.type == reshade::api::resource_type::buffer) {
            hash_bytes(fingerprint.value, initial_data[0].data,
                       static_cast<std::size_t>(desc.buffer.size));
            fingerprint.content_backed = desc.buffer.size != 0;
        } else {
            const auto row_pitch = initial_data[0].row_pitch != 0 ? initial_data[0].row_pitch :
                reshade::api::format_row_pitch(desc.texture.format, desc.texture.width);
            const auto slice_pitch = initial_data[0].slice_pitch != 0 ? initial_data[0].slice_pitch :
                reshade::api::format_slice_pitch(desc.texture.format, row_pitch,
                                                 std::max(1u, desc.texture.height));
            hash_bytes(fingerprint.value, initial_data[0].data, slice_pitch);
            fingerprint.content_backed = slice_pitch != 0;
        }
    }
    std::lock_guard lock(g_baker_probe_mutex);
    g_resource_fingerprints[resource.handle] = fingerprint;
    g_resource_states[resource.handle] = initial_state;
}

void on_baker_barrier(reshade::api::command_list *, std::uint32_t count,
                      const reshade::api::resource *resources,
                      const reshade::api::resource_usage *,
                      const reshade::api::resource_usage *new_states) {
    if (resources == nullptr || new_states == nullptr) return;
    std::lock_guard lock(g_baker_probe_mutex);
    for (std::uint32_t index = 0; index < count; ++index)
        if (resources[index] != 0)
            g_resource_states[resources[index].handle] = new_states[index];
}

bool on_baker_update_buffer(reshade::api::device *device, const void *,
                            reshade::api::resource destination, std::uint64_t,
                            std::uint64_t) {
    const auto desc = device->get_resource_desc(destination);
    if (desc.type != reshade::api::resource_type::buffer) return false;
    ResourceFingerprint fingerprint {resource_descriptor_fingerprint(desc), false};
    // Mutable geometry cannot be a restart-stable topology identity. Keep one
    // session identity per live resource rather than hashing every animation update.
    hash_value(fingerprint.value, destination.handle);
    std::lock_guard lock(g_baker_probe_mutex);
    g_resource_fingerprints[destination.handle] = fingerprint;
    return false;
}

bool on_baker_update_buffer_command(reshade::api::command_list *command_list,
                                    const void *data, reshade::api::resource destination,
                                    std::uint64_t offset, std::uint64_t size) {
    return on_baker_update_buffer(
        command_list->get_device(), data, destination, offset, size);
}

bool on_baker_update_texture(reshade::api::device *device,
                             const reshade::api::subresource_data &data,
                             reshade::api::resource destination, std::uint32_t subresource,
                             const reshade::api::subresource_box *box) {
    if (data.data == nullptr) return false;
    const auto desc = device->get_resource_desc(destination);
    const auto height = box != nullptr ? std::max(1u, box->height()) :
        std::max(1u, desc.texture.height >> std::min(subresource, 31u));
    const auto byte_count = data.slice_pitch != 0 ? data.slice_pitch :
        static_cast<std::size_t>(data.row_pitch) * height;
    std::lock_guard lock(g_baker_probe_mutex);
    auto &fingerprint = g_resource_fingerprints[destination.handle];
    if (fingerprint.value == 0) fingerprint.value = resource_descriptor_fingerprint(desc);
    hash_value(fingerprint.value, subresource);
    if (box != nullptr) hash_bytes(fingerprint.value, box, sizeof(*box));
    hash_bytes(fingerprint.value, data.data, byte_count);
    fingerprint.content_backed = byte_count != 0;
    return false;
}

void on_baker_destroy_resource(reshade::api::device *, reshade::api::resource resource) {
    std::lock_guard lock(g_baker_probe_mutex);
    g_resource_fingerprints.erase(resource.handle);
    g_resource_states.erase(resource.handle);
}

void on_baker_destroy_pipeline(reshade::api::device *device, reshade::api::pipeline pipeline) {
    neuralpass::capture::SurfaceCaptureBackend *backend = nullptr;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto capture = g_surface_captures.find(device);
            capture != g_surface_captures.end())
            backend = capture->second.get();
    }
    if (auto *d3d12 = dynamic_cast<neuralpass::d3d12_capture::SurfaceCapture *>(backend))
        d3d12->unregister_pipeline(pipeline);
    if (auto *vulkan = dynamic_cast<neuralpass::vulkan_capture::SurfaceCapture *>(backend))
        vulkan->unregister_pipeline(pipeline);
    std::lock_guard lock(g_baker_probe_mutex);
    g_pipeline_fingerprints.erase(pipeline.handle);
    g_vertex_uv_outputs.erase(pipeline.handle);
    g_pipeline_capture_info.erase(pipeline.handle);
    g_uv_pipelines.erase(pipeline.handle);
}

void on_baker_init_command_list(reshade::api::command_list *command_list) {
    command_list->create_private_data<BakerCommandState>();
}

void on_baker_destroy_command_list(reshade::api::command_list *command_list) {
    command_list->destroy_private_data<BakerCommandState>();
}

void on_baker_bind_pipeline(reshade::api::command_list *command_list,
                            reshade::api::pipeline_stage stages, reshade::api::pipeline pipeline) {
    if (auto *state = command_list->get_private_data<BakerCommandState>()) {
        PipelineCaptureInfo info;
        bool owns_uv_layout = false;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            const auto found = g_pipeline_capture_info.find(pipeline.handle);
            if (found != g_pipeline_capture_info.end()) {
                info = found->second;
                owns_uv_layout = true;
            }
        }
        if ((stages & reshade::api::pipeline_stage::vertex_shader) ==
            reshade::api::pipeline_stage::vertex_shader)
            state->vertex_pipeline = pipeline;
        if ((stages & reshade::api::pipeline_stage::pixel_shader) ==
            reshade::api::pipeline_stage::pixel_shader)
            state->pixel_pipeline = pipeline;
        if (owns_uv_layout ||
            (stages & reshade::api::pipeline_stage::input_assembler) ==
                reshade::api::pipeline_stage::input_assembler)
            state->pipeline = pipeline;
        if (owns_uv_layout && state->layout != info.layout) {
            state->layout = info.layout;
            state->pixel_resources.clear();
            state->pixel_samplers.clear();
            state->descriptor_tables.clear();
        }
    }
}

void on_baker_bind_pipeline_states(reshade::api::command_list *command_list,
                                   std::uint32_t count,
                                   const reshade::api::dynamic_state *states,
                                   const std::uint32_t *values) {
    auto *state = command_list->get_private_data<BakerCommandState>();
    if (state == nullptr || states == nullptr || values == nullptr) return;
    for (std::uint32_t index = 0; index < count; ++index)
        if (states[index] == reshade::api::dynamic_state::primitive_topology)
            state->primitive_topology =
                static_cast<reshade::api::primitive_topology>(values[index]);
}

std::uint64_t descriptor_slot(reshade::api::pipeline_layout layout,
                              std::uint32_t param, std::uint32_t binding,
                              std::uint32_t array_offset) {
    (void)layout;
    // Native layout handles are process-local and must never enter a cache key.
    // The owning pipeline fingerprint distinguishes layouts; this identifies
    // the descriptor's semantic position inside that layout.
    return (static_cast<std::uint64_t>(param) << 48) |
        (static_cast<std::uint64_t>(binding) << 16) | array_offset;
}

void track_pixel_descriptor(BakerCommandState &state, std::uint64_t slot,
                            reshade::api::resource_view view,
                            std::uint32_t dx_register_index = 0,
                            std::uint32_t dx_register_space = 0,
                            reshade::api::descriptor_table table = {},
                            std::uint32_t param = 0,
                            std::uint32_t binding = 0,
                            std::uint32_t array_offset = 0,
                            reshade::api::descriptor_type type =
                                reshade::api::descriptor_type::shader_resource_view) {
    if (view == 0) state.pixel_resources.erase(slot);
    else state.pixel_resources[slot] = {view, dx_register_index, dx_register_space,
        table, param, binding, array_offset, type};
}

void track_pixel_sampler(BakerCommandState &state, std::uint64_t slot,
                         reshade::api::sampler sampler,
                         std::uint32_t dx_register_index,
                         std::uint32_t dx_register_space,
                         reshade::api::descriptor_table table = {},
                         std::uint32_t param = 0,
                         std::uint32_t binding = 0,
                         std::uint32_t array_offset = 0,
                         reshade::api::descriptor_type type =
                             reshade::api::descriptor_type::sampler) {
    if (sampler == 0) state.pixel_samplers.erase(slot);
    else state.pixel_samplers[slot] = {sampler, dx_register_index, dx_register_space,
        table, param, binding, array_offset, type};
}

void on_baker_push_descriptors(reshade::api::command_list *command_list,
                               reshade::api::shader_stage stages,
                               reshade::api::pipeline_layout layout, std::uint32_t param,
                               const reshade::api::descriptor_table_update &update) {
    if ((stages & reshade::api::shader_stage::pixel) != reshade::api::shader_stage::pixel) return;
    auto *state = command_list->get_private_data<BakerCommandState>();
    if (!state || (state->layout != 0 && state->layout != layout)) return;
    state->layout = layout;
    state->descriptor_tables.erase(param);
    for (std::uint32_t index = 0; index < update.count; ++index) {
        reshade::api::resource_view view = {};
        if (update.type == reshade::api::descriptor_type::shader_resource_view)
            view = static_cast<const reshade::api::resource_view *>(update.descriptors)[index];
        else if (update.type == reshade::api::descriptor_type::sampler_with_resource_view) {
            const auto pair = static_cast<const reshade::api::sampler_with_resource_view *>(
                update.descriptors)[index];
            view = pair.view;
            track_pixel_sampler(*state,
                descriptor_slot(layout, param, update.binding, update.array_offset + index),
                pair.sampler, update.binding + index, 0, {}, param, update.binding,
                update.array_offset + index, update.type);
        }
        else if (update.type == reshade::api::descriptor_type::sampler) {
            const auto sampler = static_cast<const reshade::api::sampler *>(
                update.descriptors)[index];
            track_pixel_sampler(*state,
                descriptor_slot(layout, param, update.binding, update.array_offset + index),
                sampler, update.binding + index, 0, {}, param, update.binding,
                update.array_offset + index, update.type);
            continue;
        } else
            continue;
        track_pixel_descriptor(*state,
            descriptor_slot(layout, param, update.binding, update.array_offset + index),
            view, update.binding + index, 0, {}, param, update.binding,
            update.array_offset + index, update.type);
    }
}

void on_baker_bind_descriptor_tables(reshade::api::command_list *command_list,
                                     reshade::api::shader_stage stages,
                                     reshade::api::pipeline_layout layout, std::uint32_t first,
                                     std::uint32_t count, const reshade::api::descriptor_table *tables,
                                     std::uint32_t dynamic_offset_count,
                                     const std::uint32_t *) {
    if ((stages & reshade::api::shader_stage::pixel) != reshade::api::shader_stage::pixel) return;
    auto *state = command_list->get_private_data<BakerCommandState>();
    auto *tracking = command_list->get_device()->get_private_data<descriptor_tracking>();
    if (!state || !tracking || (state->layout != 0 && state->layout != layout)) return;
    state->layout = layout;
    try {
        for (std::uint32_t table_index = 0; table_index < count; ++table_index) {
            state->descriptor_tables[first + table_index] = {
                tables[table_index], dynamic_offset_count != 0};
            const auto param = tracking->get_pipeline_layout_param(layout, first + table_index);
            if (param.type != reshade::api::pipeline_layout_param_type::descriptor_table) continue;
            for (std::uint32_t range_index = 0; range_index < param.descriptor_table.count; ++range_index) {
                const auto &range = param.descriptor_table.ranges[range_index];
                if (range.count == UINT32_MAX ||
                    (range.visibility & reshade::api::shader_stage::pixel) != reshade::api::shader_stage::pixel)
                    continue;
                std::uint32_t base_offset = 0;
                reshade::api::descriptor_heap heap = {};
                command_list->get_device()->get_descriptor_heap_offset(
                    tables[table_index], range.binding, 0, &heap, &base_offset);
                for (std::uint32_t element = 0; element < range.count; ++element) {
                    const auto array_size = std::max(1u, range.array_size);
                    const auto binding = range.binding + element / array_size;
                    const auto array_offset = element % array_size;
                    const auto slot = descriptor_slot(
                        layout, first + table_index, binding, array_offset);
                    if (range.type == reshade::api::descriptor_type::shader_resource_view ||
                        range.type == reshade::api::descriptor_type::sampler_with_resource_view)
                        track_pixel_descriptor(*state, slot,
                            tracking->get_resource_view(heap, base_offset + element),
                            range.dx_register_index + element, range.dx_register_space,
                            tables[table_index], first + table_index,
                            binding, array_offset, range.type);
                    if (range.type == reshade::api::descriptor_type::sampler ||
                        range.type == reshade::api::descriptor_type::sampler_with_resource_view)
                        track_pixel_sampler(*state, slot,
                            tracking->get_sampler(heap, base_offset + element),
                            range.dx_register_index + element, range.dx_register_space,
                            tables[table_index], first + table_index, binding,
                            array_offset, range.type);
                }
            }
        }
    } catch (const std::out_of_range &) {
        // A layout or descriptor heap may have been created before tracking was
        // enabled. Skipping it is safe; throwing through a render callback is not.
    }
}

neuralpass::BindingInstanceKey current_material_id(reshade::api::device *device,
                                                   const BakerCommandState &state) {
    std::vector<neuralpass::DescriptorIdentity> resources;
    resources.reserve(state.pixel_resources.size() + state.vertex_buffers.size() + 1);
    for (const auto &[slot, binding] : state.pixel_resources) {
        if (binding.view == 0) continue;
        const auto view = binding.view;
        const auto resource = device->get_resource_from_view(view);
        if (resource == 0) continue;
        const auto desc = device->get_resource_desc(resource);
        if (desc.type != reshade::api::resource_type::texture_1d &&
            desc.type != reshade::api::resource_type::texture_2d &&
            desc.type != reshade::api::resource_type::texture_3d)
            continue;
        const auto transient_usage = reshade::api::resource_usage::render_target |
            reshade::api::resource_usage::depth_stencil |
            reshade::api::resource_usage::unordered_access;
        if ((desc.usage & transient_usage) != 0 || desc.texture.width < 16 ||
            desc.texture.height < 16)
            continue;
        ResourceFingerprint fingerprint;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            const auto known = g_resource_fingerprints.find(resource.handle);
            if (known != g_resource_fingerprints.end()) fingerprint = known->second;
        }
        if (fingerprint.value == 0) {
            fingerprint.value = resource_descriptor_fingerprint(desc);
            // Dimensions and format alone are not unique. Add the live resource
            // handle to prevent two unknown textures from sharing one atlas,
            // while keeping this material explicitly session-scoped.
            hash_value(fingerprint.value, resource.handle);
        }
        resources.push_back({slot, fingerprint.value, fingerprint.content_backed});
    }
    const auto texture_resource_count = resources.size();
    if (texture_resource_count == 0) return {};
    constexpr std::uint64_t k_vertex_slot_base = 0xfffd000000000000ull;
    constexpr std::uint64_t k_index_slot = 0xfffe000000000000ull;
    auto append_geometry = [&](reshade::api::resource resource, std::uint64_t slot,
                               std::uint64_t offset, std::uint32_t stride_or_index_size) {
        if (resource == 0) return;
        ResourceFingerprint fingerprint;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            const auto known = g_resource_fingerprints.find(resource.handle);
            if (known != g_resource_fingerprints.end()) fingerprint = known->second;
        }
        if (fingerprint.value == 0) {
            const auto desc = device->get_resource_desc(resource);
            if (desc.type != reshade::api::resource_type::buffer) return;
            fingerprint.value = resource_descriptor_fingerprint(desc);
            hash_value(fingerprint.value, resource.handle);
        }
        hash_value(fingerprint.value, offset);
        hash_value(fingerprint.value, stride_or_index_size);
        resources.push_back({slot, fingerprint.value, fingerprint.content_backed});
    };
    for (std::size_t binding = 0; binding < state.vertex_buffers.size(); ++binding) {
        const auto &vertex = state.vertex_buffers[binding];
        append_geometry(vertex.buffer, k_vertex_slot_base + binding,
                        vertex.offset, vertex.stride);
    }
    append_geometry(state.index_buffer, k_index_slot, state.index_offset, state.index_size);
    g_material_texture_candidates += texture_resource_count;
    std::vector<std::uint64_t> pipelines;
    bool complete_pipeline = true;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        for (const auto pipeline : {state.pipeline, state.vertex_pipeline, state.pixel_pipeline}) {
            if (pipeline == 0) continue;
            const auto known = g_pipeline_fingerprints.find(pipeline.handle);
            if (known != g_pipeline_fingerprints.end()) {
                pipelines.push_back(known->second);
            } else complete_pipeline = false;
        }
    }
    complete_pipeline = complete_pipeline && !pipelines.empty();
    return neuralpass::make_binding_instance_key(pipelines, resources, complete_pipeline);
}

void on_baker_bind_vertex_buffers(reshade::api::command_list *command_list,
                                  std::uint32_t first, std::uint32_t count,
                                  const reshade::api::resource *buffers,
                                  const std::uint64_t *offsets,
                                  const std::uint32_t *strides) {
    auto *state = command_list->get_private_data<BakerCommandState>();
    if (!state) return;
    for (std::uint32_t index = 0; index < count && first + index < state->vertex_buffers.size(); ++index) {
        auto &binding = state->vertex_buffers[first + index];
        binding.buffer = buffers[index];
        binding.offset = offsets != nullptr ? offsets[index] : 0;
        binding.stride = strides != nullptr ? strides[index] : 0;
    }
}

void on_baker_bind_index_buffer(reshade::api::command_list *command_list,
                                reshade::api::resource buffer, std::uint64_t offset,
                                std::uint32_t index_size) {
    if (auto *state = command_list->get_private_data<BakerCommandState>()) {
        state->index_buffer = buffer;
        state->index_offset = offset;
        state->index_size = index_size;
    }
}

void on_baker_bind_targets(reshade::api::command_list *command_list, std::uint32_t count,
                           const reshade::api::resource_view *rtvs,
                           reshade::api::resource_view dsv) {
    if (auto *state = command_list->get_private_data<BakerCommandState>()) {
        state->inside_render_pass = false;
        state->render_targets.fill({});
        state->render_target_count = std::min<std::uint32_t>(
            count, static_cast<std::uint32_t>(state->render_targets.size()));
        if (rtvs != nullptr)
            std::copy_n(rtvs, state->render_target_count, state->render_targets.begin());
        state->depth_stencil = dsv;
    }
}

bool on_baker_begin_render_pass(
    reshade::api::command_list *command_list, std::uint32_t count,
    const reshade::api::render_pass_render_target_desc *rtvs,
    const reshade::api::render_pass_depth_stencil_desc *dsv,
    reshade::api::render_pass_flags) {
    if (auto *state = command_list->get_private_data<BakerCommandState>()) {
        state->inside_render_pass = true;
        state->render_targets.fill({});
        state->render_target_count = std::min<std::uint32_t>(
            count, static_cast<std::uint32_t>(state->render_targets.size()));
        for (std::uint32_t index = 0; index < state->render_target_count; ++index)
            state->render_targets[index] = rtvs[index].view;
        state->depth_stencil = dsv != nullptr ? dsv->view : reshade::api::resource_view {};
    }
    return false;
}

bool on_baker_end_render_pass(reshade::api::command_list *command_list) {
    if (auto *state = command_list->get_private_data<BakerCommandState>())
        state->inside_render_pass = false;
    return false;
}

float half_to_float(std::uint16_t value) {
    const std::uint32_t sign = (value & 0x8000u) << 16;
    std::uint32_t exponent = (value >> 10) & 0x1Fu;
    std::uint32_t mantissa = value & 0x3FFu;
    std::uint32_t bits = 0;
    if (exponent == 0) {
        if (mantissa == 0) bits = sign;
        else {
            exponent = 127 - 15 + 1;
            while ((mantissa & 0x400u) == 0) { mantissa <<= 1; --exponent; }
            bits = sign | (exponent << 23) | ((mantissa & 0x3FFu) << 13);
        }
    } else if (exponent == 31) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent + 127 - 15) << 23) | (mantissa << 13);
    }
    float result;
    std::memcpy(&result, &bits, sizeof(result));
    return result;
}

bool decode_uv(const std::uint8_t *data, reshade::api::format format, float &u, float &v) {
    switch (format) {
    case reshade::api::format::r32g32_float:
        std::memcpy(&u, data, sizeof(float));
        std::memcpy(&v, data + sizeof(float), sizeof(float));
        return true;
    case reshade::api::format::r16g16_float: {
        std::uint16_t values[2]; std::memcpy(values, data, sizeof(values));
        u = half_to_float(values[0]); v = half_to_float(values[1]); return true;
    }
    case reshade::api::format::r16g16_unorm: {
        std::uint16_t values[2]; std::memcpy(values, data, sizeof(values));
        u = values[0] / 65535.0f; v = values[1] / 65535.0f; return true;
    }
    case reshade::api::format::r16g16_snorm: {
        std::int16_t values[2]; std::memcpy(values, data, sizeof(values));
        u = std::max(values[0] / 32767.0f, -1.0f); v = std::max(values[1] / 32767.0f, -1.0f); return true;
    }
    case reshade::api::format::r8g8_unorm:
        u = data[0] / 255.0f; v = data[1] / 255.0f; return true;
    default:
        return false;
    }
}

void probe_pretransform_buffer(reshade::api::command_list *command_list,
                               const BakerCommandState &state,
                               const PipelineCaptureInfo &info) {
    if (!info.has_position || info.uv_binding >= state.vertex_buffers.size()) return;
    const auto binding = state.vertex_buffers[info.uv_binding];
    if (binding.buffer == 0) return;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (!g_vertex_buffers_probed.insert(binding.buffer.handle).second) return;
    }
    ++g_pretransform_probe_attempts;
    const auto desc = command_list->get_device()->get_resource_desc(binding.buffer);
    const auto stride = binding.stride != 0 ? binding.stride : info.uv_stride;
    const auto start = binding.offset + info.uv_offset;
    if (desc.type != reshade::api::resource_type::buffer || stride == 0 || start >= desc.buffer.size) return;
    const auto bytes = std::min<std::uint64_t>(desc.buffer.size - start,
                                               static_cast<std::uint64_t>(stride) * 64);
    void *mapped = nullptr;
    if (!command_list->get_device()->map_buffer_region(binding.buffer, start, bytes,
            reshade::api::map_access::read_only, &mapped)) return;
    ++g_pretransform_probe_successes;
    const auto *data = static_cast<const std::uint8_t *>(mapped);
    for (std::uint64_t offset = 0; offset + 8 <= bytes; offset += stride) {
        float u = 0.0f, v = 0.0f;
        if (decode_uv(data + offset, info.uv_format, u, v) &&
            std::isfinite(u) && std::isfinite(v) && std::abs(u) < 65536.0f && std::abs(v) < 65536.0f)
            ++g_pretransform_uv_samples;
    }
    command_list->get_device()->unmap_buffer_region(binding.buffer);
}

neuralpass::BindingInstanceKey record_baker_draw(
    reshade::api::command_list *command_list,
    std::uint32_t vertex_or_index_count,
    std::uint32_t instance_count) {
    ++g_draws_seen;
    const auto *state = command_list->get_private_data<BakerCommandState>();
    if (!state) return {};
    bool uv_pipeline = false;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        uv_pipeline = g_uv_pipelines.contains(state->pipeline.handle);
    }
    if (uv_pipeline) {
        ++g_uv_draws_seen;
        g_uv_vertices_seen += static_cast<std::uint64_t>(vertex_or_index_count) * instance_count;
        PipelineCaptureInfo info;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            const auto found = g_pipeline_capture_info.find(state->pipeline.handle);
            if (found != g_pipeline_capture_info.end()) info = found->second;
        }
        probe_pretransform_buffer(command_list, *state, info);
        const auto material = current_material_id(command_list->get_device(), *state);
        if (material.value != 0) {
            ++g_material_draws_seen;
            if (material.restart_stable) {
                ++g_restart_stable_material_draws;
                {
                    std::lock_guard lock(g_baker_probe_mutex);
                    g_restart_stable_materials.insert(material.value);
                }
                load_source_slot_once(material.value);
                load_binding_strength_once(material.value);
            }
            else ++g_session_material_draws;
            std::lock_guard lock(g_baker_probe_mutex);
            g_materials_seen.insert(material.value);
        }
        return material;
    }
    return {};
}

void configure_explicit_draw_state(reshade::api::command_list *command_list,
                                   const BakerCommandState &state,
                                   neuralpass::capture::SurfaceCaptureBackend *capture,
                                   neuralpass::capture::DrawCommand &draw,
                                   int source_override) {
    if (dynamic_cast<neuralpass::d3d12_capture::SurfaceCapture *>(capture) == nullptr &&
        dynamic_cast<neuralpass::vulkan_capture::SurfaceCapture *>(capture) == nullptr)
        return;
    draw.pipeline = state.pipeline.handle;
    draw.pipeline_layout = state.layout.handle;
    draw.render_target_count = state.render_target_count;
    draw.depth_stencil_view = state.depth_stencil.handle;
    draw.inside_render_pass = state.inside_render_pass;
    draw.api_command_list = command_list;
    for (std::uint32_t index = 0; index < state.render_target_count; ++index)
        draw.render_target_views[index] = state.render_targets[index].handle;

    const BakerPixelResource *best_source = nullptr;
    std::uint64_t best_source_slot = 0;
    std::uint64_t best_score = 0;
    for (const auto &[slot, binding] : state.pixel_resources) {
        (void)slot;
        if (binding.view == 0) continue;
        const auto resource = command_list->get_device()->get_resource_from_view(binding.view);
        if (resource == 0) continue;
        const auto desc = command_list->get_device()->get_resource_desc(resource);
        const auto view_desc = command_list->get_device()->get_resource_view_desc(binding.view);
        if (desc.type != reshade::api::resource_type::texture_2d ||
            view_desc.type != reshade::api::resource_view_type::texture_2d ||
            desc.texture.samples != 1 || desc.texture.width < 4 || desc.texture.height < 4)
            continue;
        const auto transient_usage = reshade::api::resource_usage::render_target |
            reshade::api::resource_usage::depth_stencil |
            reshade::api::resource_usage::unordered_access;
        if ((desc.usage & transient_usage) != 0) continue;
        const bool selected = source_override >= 0 &&
            binding.dx_register_index == static_cast<std::uint32_t>(source_override);
        const auto score = static_cast<std::uint64_t>(desc.texture.width) *
            desc.texture.height + (static_cast<std::uint64_t>(desc.texture.levels) << 24);
        if (selected || (source_override < 0 && score > best_score)) {
            best_source = &binding;
            best_source_slot = slot;
            best_score = score;
            if (selected) break;
        }
    }
    if (best_source != nullptr) {
        const auto source_resource = command_list->get_device()->get_resource_from_view(
            best_source->view);
        draw.source_view = best_source->view.handle;
        draw.source_resource = source_resource.handle;
        draw.source_descriptor_param = best_source->param;
        draw.source_descriptor_binding = best_source->binding;
        draw.source_descriptor_array_offset = best_source->array_offset;
        draw.source_descriptor_type = static_cast<std::uint32_t>(best_source->type);
        {
            std::lock_guard lock(g_baker_probe_mutex);
            if (const auto known = g_resource_states.find(source_resource.handle);
                known != g_resource_states.end())
                draw.source_usage = static_cast<std::uint32_t>(known->second);
        }
        const BakerPixelSampler *best_sampler = nullptr;
        if (const auto exact = state.pixel_samplers.find(best_source_slot);
            exact != state.pixel_samplers.end() && exact->second.sampler != 0)
            best_sampler = &exact->second;
        if (best_sampler == nullptr) {
            for (const auto &[slot, sampler] : state.pixel_samplers) {
                (void)slot;
                if (sampler.sampler == 0) continue;
                if (best_sampler == nullptr ||
                    std::tie(sampler.dx_register_space, sampler.dx_register_index) <
                        std::tie(best_sampler->dx_register_space,
                                 best_sampler->dx_register_index))
                    best_sampler = &sampler;
                if (sampler.dx_register_index == best_source->dx_register_index &&
                    sampler.dx_register_space == best_source->dx_register_space) {
                    best_sampler = &sampler;
                    break;
                }
            }
        }
        if (best_sampler != nullptr) {
            draw.source_register = best_source->dx_register_index;
            draw.source_space = best_source->dx_register_space;
            draw.sampler_register = best_sampler->dx_register_index;
            draw.sampler_space = best_sampler->dx_register_space;
            draw.source_sampler = best_sampler->sampler.handle;
            draw.sampler_descriptor_table = best_sampler->table.handle;
            draw.sampler_descriptor_param = best_sampler->param;
            draw.sampler_descriptor_binding = best_sampler->binding;
            draw.sampler_descriptor_array_offset = best_sampler->array_offset;
            draw.sampler_descriptor_type = static_cast<std::uint32_t>(best_sampler->type);
            draw.source_sampleable = true;
        }
        const auto bound_table = state.descriptor_tables.find(best_source->param);
        auto *tracking = command_list->get_device()->get_private_data<descriptor_tracking>();
        if (tracking != nullptr && best_source->table != 0 &&
            bound_table != state.descriptor_tables.end() &&
            bound_table->second.table == best_source->table &&
            !bound_table->second.has_dynamic_offsets) {
            try {
                const auto param = tracking->get_pipeline_layout_param(
                    state.layout, best_source->param);
                if (param.type == reshade::api::pipeline_layout_param_type::descriptor_table &&
                    param.descriptor_table.count <= draw.source_table_ranges.size()) {
                    bool bounded = true;
                    for (std::uint32_t index = 0; index < param.descriptor_table.count; ++index) {
                        const auto &range = param.descriptor_table.ranges[index];
                        if (range.count == UINT32_MAX) { bounded = false; break; }
                        draw.source_table_ranges[index] = {range.binding, range.count};
                    }
                    if (bounded) {
                        draw.source_descriptor_table = best_source->table.handle;
                        draw.source_table_range_count = param.descriptor_table.count;
                        draw.source_descriptor_isolatable = true;
                    }
                }
            } catch (const std::out_of_range &) {
                draw.source_descriptor_isolatable = false;
            }
        }
    }
    if (!state.inside_render_pass && state.render_target_count != 0 &&
        state.render_targets[0] != 0) {
        const auto resource = command_list->get_device()->get_resource_from_view(
            state.render_targets[0]);
        if (resource != 0) {
            const auto desc = command_list->get_device()->get_resource_desc(resource);
            draw.target_compatible = desc.type == reshade::api::resource_type::texture_2d &&
                desc.texture.width == capture->width() &&
                desc.texture.height == capture->height() && desc.texture.samples == 1;
        }
    }
}

void *capture_command_handle(reshade::api::command_list *command_list) {
    return command_list->get_device()->get_api() == reshade::api::device_api::vulkan
        ? static_cast<void *>(command_list)
        : reinterpret_cast<void *>(command_list->get_native());
}

bool on_baker_draw(reshade::api::command_list *command_list, std::uint32_t vertex_count,
                   std::uint32_t instance_count, std::uint32_t first_vertex,
                   std::uint32_t first_instance) {
    const auto material = record_baker_draw(command_list, vertex_count, instance_count);
    const auto api = command_list->get_device()->get_api();
    if (material.value == 0 ||
        (api != reshade::api::device_api::d3d9 &&
         api != reshade::api::device_api::d3d10 &&
         api != reshade::api::device_api::d3d11 &&
         api != reshade::api::device_api::d3d12 &&
         api != reshade::api::device_api::vulkan)) return false;
    const auto *state = command_list->get_private_data<BakerCommandState>();
    neuralpass::capture::UvInput uv;
    neuralpass::capture::SurfaceCaptureBackend *capture = nullptr;
    int source_override = -1;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_vertex_uv_outputs.find(state->vertex_pipeline.handle);
            found != g_vertex_uv_outputs.end()) uv = found->second;
        if (const auto found = g_surface_captures.find(command_list->get_device());
            found != g_surface_captures.end()) capture = found->second.get();
        if (const auto found = g_source_slot_overrides.find(material.value);
            found != g_source_slot_overrides.end()) source_override = found->second;
    }
    neuralpass::capture::DrawCommand draw {
        .kind = neuralpass::capture::DrawKind::direct,
        .vertex_or_index_count = vertex_count,
        .instance_count = instance_count,
        .first_vertex_or_index = first_vertex,
        .first_instance = first_instance,
        .primitive_topology = static_cast<std::uint32_t>(state->primitive_topology),
    };
    configure_explicit_draw_state(command_list, *state, capture, draw, source_override);
    return capture != nullptr && uv.valid() && capture->replay(
        capture_command_handle(command_list), uv, material.value,
        draw, source_override);
}

bool on_baker_draw_indexed(reshade::api::command_list *command_list,
                           std::uint32_t index_count, std::uint32_t instance_count,
                           std::uint32_t first_index, std::int32_t vertex_offset,
                           std::uint32_t first_instance) {
    const auto material = record_baker_draw(command_list, index_count, instance_count);
    const auto api = command_list->get_device()->get_api();
    if (material.value == 0 ||
        (api != reshade::api::device_api::d3d9 &&
         api != reshade::api::device_api::d3d10 &&
         api != reshade::api::device_api::d3d11 &&
         api != reshade::api::device_api::d3d12 &&
         api != reshade::api::device_api::vulkan)) return false;
    const auto *state = command_list->get_private_data<BakerCommandState>();
    neuralpass::capture::UvInput uv;
    neuralpass::capture::SurfaceCaptureBackend *capture = nullptr;
    int source_override = -1;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_vertex_uv_outputs.find(state->vertex_pipeline.handle);
            found != g_vertex_uv_outputs.end()) uv = found->second;
        if (const auto found = g_surface_captures.find(command_list->get_device());
            found != g_surface_captures.end()) capture = found->second.get();
        if (const auto found = g_source_slot_overrides.find(material.value);
            found != g_source_slot_overrides.end()) source_override = found->second;
    }
    neuralpass::capture::DrawCommand draw {
        .kind = neuralpass::capture::DrawKind::indexed,
        .vertex_or_index_count = index_count,
        .instance_count = instance_count,
        .first_vertex_or_index = first_index,
        .vertex_offset = vertex_offset,
        .first_instance = first_instance,
        .primitive_topology = static_cast<std::uint32_t>(state->primitive_topology),
    };
    configure_explicit_draw_state(command_list, *state, capture, draw, source_override);
    return capture != nullptr && uv.valid() && capture->replay(
        capture_command_handle(command_list), uv, material.value,
        draw, source_override);
}

bool on_baker_draw_indirect(reshade::api::command_list *command_list,
                            reshade::api::indirect_command type,
                            reshade::api::resource buffer, std::uint64_t offset,
                            std::uint32_t draw_count, std::uint32_t stride) {
    if ((type != reshade::api::indirect_command::draw &&
         type != reshade::api::indirect_command::draw_indexed) ||
        buffer.handle == 0 || draw_count != 1 || offset > UINT_MAX)
        return false;
    const auto material = record_baker_draw(command_list, 0, 1);
    const auto api = command_list->get_device()->get_api();
    if (material.value == 0 ||
        (api != reshade::api::device_api::d3d11 &&
         api != reshade::api::device_api::d3d12 &&
         api != reshade::api::device_api::vulkan)) return false;
    const auto *state = command_list->get_private_data<BakerCommandState>();
    neuralpass::capture::UvInput uv;
    neuralpass::capture::SurfaceCaptureBackend *capture = nullptr;
    int source_override = -1;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_vertex_uv_outputs.find(state->vertex_pipeline.handle);
            found != g_vertex_uv_outputs.end()) uv = found->second;
        if (const auto found = g_surface_captures.find(command_list->get_device());
            found != g_surface_captures.end()) capture = found->second.get();
        if (const auto found = g_source_slot_overrides.find(material.value);
            found != g_source_slot_overrides.end()) source_override = found->second;
    }
    if (capture == nullptr || !uv.valid()) return false;
    neuralpass::capture::DrawCommand draw {
        .kind = type == reshade::api::indirect_command::draw
            ? neuralpass::capture::DrawKind::indirect
            : neuralpass::capture::DrawKind::indexed_indirect,
        .argument_buffer = reinterpret_cast<void *>(buffer.handle),
        .argument_offset = offset,
        .draw_count = draw_count,
        .argument_stride = stride,
    };
    configure_explicit_draw_state(command_list, *state, capture, draw, source_override);
    return capture->replay(capture_command_handle(command_list), uv,
                           material.value, draw, source_override);
}

struct __declspec(uuid("F3110BBA-813B-4A3C-A848-4C594E504153")) RuntimeState {
    std::mutex mutex;
    std::condition_variable_any wake;
    CapturedFrame pending;
    std::optional<neuralpass::SurfaceCaptureFrame> pending_surface;
    std::vector<std::uint8_t> ready_styled;
    std::vector<std::uint8_t> ready_valid;
    std::deque<std::vector<std::uint8_t>> bridge_ready;
    std::unordered_map<std::uint64_t, neuralpass::SurfaceCaptureFrame> surface_ready;
    std::unordered_map<std::uint64_t,
        std::vector<neuralpass::capture::ReplacementMip>> ready_replacements;
    bool has_pending = false;
    bool has_ready = false;
    std::uint32_t ready_width = 0;
    std::uint32_t ready_height = 0;
    bool stop = false;
    std::jthread worker;
    reshade::api::effect_texture_variable styled_variable = {};
    reshade::api::effect_texture_variable valid_variable = {};
    reshade::api::effect_uniform_variable color_mode_variable = {};
    std::atomic_uint64_t submitted = 0;
    std::atomic_uint64_t completed = 0;
    std::atomic_uint64_t dropped = 0;
    std::array<std::atomic_uint64_t, 6> visibility_counts {};
    std::atomic_uint32_t tile_budget = 2;
    std::atomic_uint32_t effective_tile_budget = 1;
    std::atomic_uint32_t target_frame_rate = 60;
    std::atomic_bool adaptive_tile_budget = true;
    std::atomic_uint64_t measured_frame_time_us = 0;
    std::atomic_uint64_t measured_tile_time_us = 0;
    std::atomic_uint64_t budget_pressure_events = 0;
    std::chrono::steady_clock::time_point last_effect_time {};
    std::atomic_uint32_t refresh_age = 120;
    std::atomic_bool reset_requested = false;
    std::atomic_bool sticky_history = true;
    // The packaged ONNX/preview backend must work without an external process.
    // The legacy WSL StreamDiffusion bridge is explicit opt-in.
    std::atomic_bool stream_bridge = false;
    std::atomic_uint64_t prompt_identity = 1;
    std::atomic_int scene_command = static_cast<int>(ManualSceneCommand::none);
    std::atomic_uint64_t active_scene_identity = 0;
    std::atomic_uint64_t scene_generation = 1;
    bool directml_enabled = false;
    std::uint32_t directml_device_id = 0;
    std::atomic_uint64_t replacement_epoch = 1;
    std::uint64_t applied_replacement_epoch = 0;
    std::atomic_int scene_transition = static_cast<int>(neuralpass::SceneTransition::stable);
    std::uint64_t selected_binding = 0;
    int selected_source_slot = -1;
    int selected_binding_strength = 100;
    std::atomic_int requested_preset = 0;
    std::array<char, 512> prompt {};
    std::vector<std::string> prompt_history;
    int prompt_history_selected = -1;
    std::atomic_uint64_t bridge_sent = 0;
    std::atomic_uint64_t bridge_received = 0;
    std::atomic_uint64_t live_sent = 0;
    std::atomic_uint64_t live_received = 0;
    std::uint64_t last_live_write_tick = 0;
    std::string backend_name = "starting";
    reshade::api::resource readback[3] = {};
    reshade::api::fence copy_fence = {};
    std::uint64_t next_signal = 1;
    std::uint64_t next_read = 1;
    std::uint32_t capture_width = 0;
    std::uint32_t capture_height = 0;
    reshade::api::format capture_format = reshade::api::format::unknown;
    neuralpass::CapturePixelLayout capture_layout = neuralpass::CapturePixelLayout::rgba8;
    neuralpass::DisplayEncoding capture_encoding = neuralpass::DisplayEncoding::sdr_srgb;
    std::uint32_t capture_bytes_per_pixel = 4;
    reshade::api::format rejected_format = reshade::api::format::unknown;
    reshade::api::color_space rejected_color_space = reshade::api::color_space::unknown;
    bool runtime_evidence_written = false;
    bool runtime_evidence_write_failed = false;
    reshade::api::device_api evidence_api = reshade::api::device_api::d3d9;
    reshade::api::format evidence_format = reshade::api::format::unknown;
    reshade::api::color_space evidence_color_space = reshade::api::color_space::unknown;
    std::uint32_t evidence_width = 0;
    std::uint32_t evidence_height = 0;
    bool evidence_supported = false;
};

std::filesystem::path bridge_directory() {
    return g_addon_directory / "NeuralPassBridge";
}

CapturedFrame resize_nearest(const CapturedFrame &source, std::uint32_t width, std::uint32_t height) {
    CapturedFrame result {width, height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(width) * height * 4)};
    for (std::uint32_t y = 0; y < height; ++y) {
        const auto sy = std::min(source.height - 1, y * source.height / height);
        for (std::uint32_t x = 0; x < width; ++x) {
            const auto sx = std::min(source.width - 1, x * source.width / width);
            const auto si = (static_cast<std::size_t>(sy) * source.width + sx) * 4;
            const auto di = (static_cast<std::size_t>(y) * width + x) * 4;
            std::copy_n(source.rgba.data() + si, 4, result.rgba.data() + di);
        }
    }
    return result;
}

bool write_bridge_frame(const CapturedFrame &frame, std::uint64_t sequence) {
    std::error_code error;
    std::filesystem::create_directories(bridge_directory(), error);
    const auto temp = bridge_directory() / "input.tmp";
    const auto target = bridge_directory() / ("input_" + std::to_string(sequence) + ".rgba");
    BridgeHeader header;
    header.sequence = sequence;
    header.width = frame.width;
    header.height = frame.height;
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char *>(&header), sizeof(header));
        output.write(reinterpret_cast<const char *>(frame.rgba.data()),
                     static_cast<std::streamsize>(frame.rgba.size()));
        if (!output) return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool write_live_frame(const CapturedFrame &frame, std::uint64_t sequence) {
    std::error_code error;
    std::filesystem::create_directories(bridge_directory(), error);
    const auto temp = bridge_directory() / "live_input.tmp";
    const auto target = bridge_directory() / "live_input.rgba";
    BridgeHeader header;
    std::memcpy(header.magic, "NPL1", 4);
    header.sequence = sequence;
    header.width = frame.width;
    header.height = frame.height;
    {
        std::ofstream output(temp, std::ios::binary | std::ios::trunc);
        if (!output) return false;
        output.write(reinterpret_cast<const char *>(&header), sizeof(header));
        output.write(reinterpret_cast<const char *>(frame.rgba.data()),
                     static_cast<std::streamsize>(frame.rgba.size()));
        if (!output) return false;
    }
    return MoveFileExW(temp.c_str(), target.c_str(),
                       MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != 0;
}

bool read_live_frame(std::uint64_t after_sequence, CapturedFrame &frame,
                     std::uint64_t &sequence) {
    const auto path = bridge_directory() / "live_output.rgba";
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    BridgeHeader header;
    input.read(reinterpret_cast<char *>(&header), sizeof(header));
    if (!input || std::memcmp(header.magic, "NPL2", 4) != 0 ||
        header.sequence <= after_sequence || header.width == 0 || header.height == 0 ||
        header.width > 4096 || header.height > 4096)
        return false;
    frame = {header.width, header.height,
        std::vector<std::uint8_t>(static_cast<std::size_t>(header.width) * header.height * 4)};
    input.read(reinterpret_cast<char *>(frame.rgba.data()),
               static_cast<std::streamsize>(frame.rgba.size()));
    if (!input) return false;
    sequence = header.sequence;
    return true;
}

bool read_bridge_frames(std::uint64_t sequence, std::vector<CapturedFrame> &frames) {
    const auto path = bridge_directory() / ("output_" + std::to_string(sequence) + ".rgba");
    std::ifstream input(path, std::ios::binary);
    if (!input) return false;
    BridgeOutputHeader header;
    input.read(reinterpret_cast<char *>(&header), sizeof(header));
    if (!input || std::memcmp(header.magic, "NPF2", 4) != 0 || header.sequence != sequence ||
        header.width == 0 || header.height == 0 || header.width > 4096 || header.height > 4096 ||
        header.frame_count == 0 || header.frame_count > 8)
        return false;
    const auto frame_bytes = static_cast<std::size_t>(header.width) * header.height * 4;
    frames.clear();
    frames.reserve(header.frame_count);
    for (std::uint32_t index = 0; index < header.frame_count; ++index) {
        CapturedFrame frame {header.width, header.height, std::vector<std::uint8_t>(frame_bytes)};
        input.read(reinterpret_cast<char *>(frame.rgba.data()),
                   static_cast<std::streamsize>(frame.rgba.size()));
        if (!input) return false;
        frames.push_back(std::move(frame));
    }
    input.close();
    std::error_code error;
    std::filesystem::remove(path, error);
    return true;
}

void publish_bridge_results(RuntimeState *state, const std::vector<CapturedFrame> &results,
                            std::uint32_t width, std::uint32_t height) {
    std::lock_guard lock(state->mutex);
    for (const auto &result : results) {
        auto display = resize_nearest(result, width, height);
        state->bridge_ready.push_back(std::move(display.rgba));
    }
    while (state->bridge_ready.size() > 8) state->bridge_ready.pop_front();
    state->ready_width = width;
    state->ready_height = height;
    ++state->completed;
}

std::string normalized_prompt(const char *text) {
    std::string result = text != nullptr ? text : "";
    std::replace(result.begin(), result.end(), '\r', ' ');
    std::replace(result.begin(), result.end(), '\n', ' ');
    while (!result.empty() && result.front() == ' ') result.erase(result.begin());
    while (!result.empty() && result.back() == ' ') result.pop_back();
    return result;
}

std::uint64_t stream_prompt_identity(const char *text) {
    const auto prompt = normalized_prompt(text);
    std::uint64_t hash = k_fnv_offset;
    hash_string(hash, "stream-prompt-v1");
    hash_string(hash, prompt);
    return nonzero_hash(hash);
}

void save_prompt_history(const RuntimeState &state) {
    std::error_code error;
    std::filesystem::create_directories(bridge_directory(), error);
    const auto temp = bridge_directory() / "prompt_history.tmp";
    const auto target = bridge_directory() / "prompt_history.txt";
    {
        std::ofstream output(temp, std::ios::trunc);
        for (const auto &prompt : state.prompt_history)
            output << normalized_prompt(prompt.c_str()) << '\n';
    }
    MoveFileExW(temp.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
}

void remember_prompt(RuntimeState &state, const std::string &prompt) {
    if (prompt.empty()) return;
    state.prompt_history.erase(std::remove(state.prompt_history.begin(),
        state.prompt_history.end(), prompt), state.prompt_history.end());
    state.prompt_history.insert(state.prompt_history.begin(), prompt);
    if (state.prompt_history.size() > 32) state.prompt_history.resize(32);
    state.prompt_history_selected = 0;
}

void save_prompt(RuntimeState &state) {
    std::error_code error;
    std::filesystem::create_directories(bridge_directory(), error);
    const auto temp = bridge_directory() / "prompt.tmp";
    const auto target = bridge_directory() / "prompt.txt";
    const auto prompt = normalized_prompt(state.prompt.data());
    state.prompt_identity = stream_prompt_identity(prompt.c_str());
    {
        std::ofstream output(temp, std::ios::trunc);
        output << prompt;
    }
    MoveFileExW(temp.c_str(), target.c_str(),
                MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH);
    remember_prompt(state, prompt);
    save_prompt_history(state);
}

void load_prompt_history(RuntimeState &state) {
    std::ifstream input(bridge_directory() / "prompt_history.txt");
    std::string line;
    while (std::getline(input, line)) {
        line = normalized_prompt(line.c_str());
        if (!line.empty() && std::find(state.prompt_history.begin(),
                state.prompt_history.end(), line) == state.prompt_history.end())
            state.prompt_history.push_back(std::move(line));
        if (state.prompt_history.size() >= 32) break;
    }
    remember_prompt(state, normalized_prompt(state.prompt.data()));
}

Image<Color> unpack(const CapturedFrame &frame) {
    Image<Color> image(frame.width, frame.height);
    for (std::size_t i = 0; i < image.size(); ++i) {
        image.pixels()[i] = {frame.rgba[i*4+0] / 255.0f, frame.rgba[i*4+1] / 255.0f,
                             frame.rgba[i*4+2] / 255.0f, frame.rgba[i*4+3] / 255.0f};
    }
    return image;
}

std::uint8_t byte(float value);

CapturedFrame pack(const Image<Color> &image) {
    CapturedFrame frame {image.width(), image.height(),
        std::vector<std::uint8_t>(image.size() * 4)};
    for (std::size_t index = 0; index < image.size(); ++index) {
        frame.rgba[index * 4 + 0] = byte(image.pixels()[index].r);
        frame.rgba[index * 4 + 1] = byte(image.pixels()[index].g);
        frame.rgba[index * 4 + 2] = byte(image.pixels()[index].b);
        frame.rgba[index * 4 + 3] = byte(image.pixels()[index].a);
    }
    return frame;
}

void publish_replacement_snapshots(
    RuntimeState &state, const neuralpass::MaterialTextureBaker &baker,
    std::span<const neuralpass::SurfaceCorrespondence> samples,
    std::unordered_set<std::uint64_t> &published, bool force) {
    std::unordered_set<std::uint64_t> material_ids;
    for (const auto &sample : samples)
        if (sample.material_id != 0) material_ids.insert(sample.material_id);
    std::unordered_map<std::uint64_t,
        std::vector<neuralpass::capture::ReplacementMip>> snapshots;
    for (const auto material_id : material_ids) {
        if (!force && published.contains(material_id)) continue;
        const auto *atlas = baker.find(material_id);
        if (atlas == nullptr) continue;
        std::vector<neuralpass::capture::ReplacementMip> packed;
        for (const auto &mip : atlas->generate_mips()) {
            neuralpass::capture::ReplacementMip output;
            output.width = mip.color.width();
            output.height = mip.color.height();
            output.rgba.resize(mip.color.size() * 4);
            output.coverage = mip.coverage.pixels();
            for (std::size_t index = 0; index < mip.color.size(); ++index) {
                output.rgba[index * 4 + 0] = byte(mip.color.pixels()[index].r);
                output.rgba[index * 4 + 1] = byte(mip.color.pixels()[index].g);
                output.rgba[index * 4 + 2] = byte(mip.color.pixels()[index].b);
                output.rgba[index * 4 + 3] = byte(mip.color.pixels()[index].a);
            }
            packed.push_back(std::move(output));
        }
        snapshots.emplace(material_id, std::move(packed));
        published.insert(material_id);
    }
    if (snapshots.empty()) return;
    std::lock_guard lock(state.mutex);
    for (auto &[material_id, mips] : snapshots)
        state.ready_replacements.insert_or_assign(material_id, std::move(mips));
}

std::uint8_t byte(float value) {
    return static_cast<std::uint8_t>(std::clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f);
}

Image<Color> extract(const Image<Color> &image, const neuralpass::Rect &rect) {
    Image<Color> tile(rect.width, rect.height);
    for (std::uint32_t y = 0; y < rect.height; ++y)
        for (std::uint32_t x = 0; x < rect.width; ++x)
            tile.at(x, y) = image.at(rect.x + x, rect.y + y);
    return tile;
}

void process_frames(RuntimeState *state, std::stop_token token) {
    HistoryFrame history;
    neuralpass::MaterialTextureBaker material_baker;
    neuralpass::SceneTransitionTracker scene_transitions;
    neuralpass::CacheNamespace active_namespace {g_game_build_identity, 1, 1};
    neuralpass::SceneCacheCatalog scene_catalog(active_namespace.scene_catalog_root(
        g_addon_directory / "NeuralPassCache"));
    std::optional<neuralpass::SceneCacheSelection> active_scene;
    std::unordered_set<std::uint64_t> active_scene_materials;
    bool active_stream = state->stream_bridge.load();
    std::uint64_t active_prompt_identity = state->prompt_identity.load();
    auto stable_visible = [](std::span<const neuralpass::SurfaceCorrespondence> samples) {
        std::vector<std::uint64_t> result;
        std::lock_guard lock(g_baker_probe_mutex);
        for (const auto &sample : samples)
            if (g_restart_stable_materials.contains(sample.material_id))
                result.push_back(sample.material_id);
        std::sort(result.begin(), result.end());
        result.erase(std::unique(result.begin(), result.end()), result.end());
        return result;
    };
    auto snapshot_binding_controls = [] {
        std::lock_guard lock(g_baker_probe_mutex);
        return std::pair {g_binding_style_controls, g_binding_style_revision.load()};
    };
    auto save_active_scene = [&] {
        if (!active_scene || !active_scene->valid()) return;
        std::vector<std::uint64_t> stable(active_scene_materials.begin(),
                                          active_scene_materials.end());
        // The unversioned legacy diffusion process cannot safely reuse an atlas
        // across launches. Keep its accumulation session-local.
        if (!active_stream)
            (void)material_baker.save_cache(
                active_namespace.atlas_root(active_scene->directory), stable);
        (void)scene_catalog.record(active_scene->identity, stable);
    };
    auto activate_scene = [&](std::span<const std::uint64_t> stable, bool force_new = false) {
        if (stable.empty()) return;
        active_scene = force_new
            ? scene_catalog.create_new(stable, static_cast<std::uint64_t>(
                std::chrono::steady_clock::now().time_since_epoch().count()))
            : scene_catalog.resolve(stable);
        active_scene_materials.insert(stable.begin(), stable.end());
        scene_transitions.set_scene_identity(active_scene->identity);
        state->active_scene_identity = active_scene->identity;
        if (force_new) (void)scene_catalog.record(active_scene->identity, stable);
        const auto loaded = active_stream ? neuralpass::AtlasCacheStats {} :
            material_baker.load_cache(active_namespace.atlas_root(active_scene->directory));
        if (loaded.loaded != 0)
            reshade::log::message(reshade::log::level::info,
                ("NeuralPass loaded " + std::to_string(loaded.loaded) +
                 " atlases for persistent scene " +
                 std::to_string(active_scene->identity) + ".").c_str());
    };
    auto [binding_controls, active_binding_style_revision] = snapshot_binding_controls();
    std::unique_ptr<neuralpass::InferenceBackend> backend;
    char *preset_value = nullptr;
    std::size_t preset_length = 0;
    _dupenv_s(&preset_value, &preset_length, "NEURALPASS_PRESET");
    const std::string preset_env = preset_value != nullptr ? preset_value : "";
    std::free(preset_value);
    int active_preset = 0;
    if (!preset_env.empty())
        for (std::size_t i = 0; i < k_presets.size(); ++i)
            if (preset_env == k_presets[i]) active_preset = static_cast<int>(i);
    state->requested_preset = active_preset;
    auto load_backend = [state](int preset_index) -> std::unique_ptr<neuralpass::InferenceBackend> {
        const std::string preset = k_presets.at(static_cast<std::size_t>(preset_index));
        const auto model = g_addon_directory / "models" / "downloads" / (preset + "-9.onnx");
#if defined(_WIN32) && !defined(_WIN64)
        try {
            auto worker = g_addon_directory / "NeuralPass" / "runtime" /
                "NeuralPassWorker.exe";
            if (!std::filesystem::is_regular_file(worker))
                worker = g_addon_directory / "NeuralPassWorker.exe";
            if (std::filesystem::exists(model) && std::filesystem::exists(worker))
                return neuralpass::make_worker_backend(
                    worker.string(), model.string(), state->directml_enabled,
                    state->directml_device_id);
        } catch (const std::exception &error) {
            reshade::log::message(reshade::log::level::error, error.what());
        }
#endif
#ifdef NEURALPASS_HAS_ONNXRUNTIME
        try {
            if (std::filesystem::exists(model))
                // DirectML is enabled for the validated D3D11 path. Other APIs use
                // the CPU provider until their device-loss stress gates pass.
                return neuralpass::make_onnx_backend(
                    model.string(), state->directml_enabled, state->directml_device_id);
        } catch (const std::exception &error) {
            reshade::log::message(reshade::log::level::error, error.what());
        }
#endif
        return neuralpass::make_preview_backend(preset);
    };
    backend = load_backend(active_preset);
    active_namespace = cache_namespace_for(
        k_presets.at(static_cast<std::size_t>(active_preset)), backend->name(),
        neuralpass::binding_style_control_identity(binding_controls));
    if (!backend->name().starts_with("onnx/") &&
        !backend->name().starts_with("worker/")) {
        reshade::log::message(reshade::log::level::warning,
            "NeuralPass is using the preview backend. Download a model and build with ONNXRUNTIME_ROOT for neural output.");
    }
    {
        std::lock_guard lock(state->mutex);
        state->backend_name = std::string(backend->name());
    }
    struct BridgeBakeContext {
        std::uint64_t sequence = 0;
        std::uint64_t style_generation = 0;
        neuralpass::SceneKey scene;
        std::uint32_t width = 0;
        std::uint32_t height = 0;
        neuralpass::MaterialTextureBakePlan plan;
        std::vector<neuralpass::SurfaceCorrespondence> correspondence;
        Image<std::uint8_t> binding_strength;
    };
    std::optional<BridgeBakeContext> bridge_bake;
    Image<Color> previous_scene;
    std::optional<neuralpass::SurfaceCaptureFrame> previous_surface;
    std::unordered_set<std::uint64_t> published_replacements;
    std::uint64_t style_generation = 1;
    neuralpass::AdaptiveTileBudgetController budget_controller;
    bool adaptive_was_enabled = true;

    while (!token.stop_requested()) {
        CapturedFrame frame;
        std::optional<neuralpass::SurfaceCaptureFrame> surface;
        {
            std::unique_lock lock(state->mutex);
            state->wake.wait(lock, token, [&] { return state->stop || state->has_pending; });
            if (state->stop || token.stop_requested()) break;
            frame = std::move(state->pending);
            surface = std::move(state->pending_surface);
            state->pending_surface.reset();
            state->has_pending = false;
        }
        const auto dropped_before_processing = state->dropped.load();
        const bool requested_stream = state->stream_bridge.load();
        const auto requested_prompt_identity = state->prompt_identity.load();
        if (requested_stream != active_stream ||
            (requested_stream && requested_prompt_identity != active_prompt_identity)) {
            save_active_scene();
            active_stream = requested_stream;
            active_prompt_identity = requested_prompt_identity;
            material_baker.reset_scene();
            published_replacements.clear();
            {
                std::lock_guard lock(state->mutex);
                state->ready_replacements.clear();
                state->bridge_ready.clear();
                if (state->ready_width != 0 && state->ready_height != 0) {
                    const auto bytes = static_cast<std::size_t>(state->ready_width) *
                                       state->ready_height * 4;
                    state->ready_styled.assign(bytes, 0);
                    state->ready_valid.assign(bytes, 0);
                    state->has_ready = true;
                }
                if (!active_stream) state->backend_name = std::string(backend->name());
            }
            ++state->replacement_epoch;
            ++style_generation;
            history = {};
            bridge_bake.reset();
            if (!active_stream && active_scene && active_scene->valid())
                (void)material_baker.load_cache(
                    active_namespace.atlas_root(active_scene->directory));
        }
        const Image<Color> current = unpack(frame);
        HistoryFrame cut_reference;
        cut_reference.source = previous_scene;
        const bool visual_cut = !previous_scene.empty() &&
            neuralpass::is_camera_cut(current, cut_reference);
        previous_scene = current;
        std::vector<neuralpass::SurfaceCorrespondence> correspondence;
        std::optional<Image<neuralpass::TextureRevealClass>> reveal_classes;
        if (surface && surface->width() == frame.width && surface->height() == frame.height) {
            const auto visibility = neuralpass::classify_visibility(
                *surface, visual_cut || !previous_surface ? nullptr : &*previous_surface, nullptr);
            for (std::size_t index = 0; index < visibility.counts.size(); ++index)
                state->visibility_counts[index] = visibility.counts[index];
            reveal_classes.emplace(frame.width, frame.height,
                                   neuralpass::TextureRevealClass::unknown);
            for (std::size_t index = 0; index < visibility.pixels.size(); ++index) {
                const auto value = visibility.pixels.pixels()[index];
                if (value == neuralpass::VisibilityClass::first_observation)
                    reveal_classes->pixels()[index] =
                        neuralpass::TextureRevealClass::first_observation;
                else if (value == neuralpass::VisibilityClass::disoccluded ||
                         value == neuralpass::VisibilityClass::newly_front_facing ||
                         value == neuralpass::VisibilityClass::offscreen_entry)
                    reveal_classes->pixels()[index] =
                        neuralpass::TextureRevealClass::newly_visible;
            }
            correspondence = surface->correspondences();
            previous_surface = std::move(surface);
        } else {
            previous_surface.reset();
            for (auto &count : state->visibility_counts) count = 0;
        }
        auto transition = scene_transitions.observe(visual_cut, correspondence);
        const auto visible_stable = stable_visible(correspondence);
        const auto manual = static_cast<ManualSceneCommand>(state->scene_command.exchange(
            static_cast<int>(ManualSceneCommand::none)));
        bool force_new_scene = false;
        if (manual == ManualSceneCommand::keep_current) {
            scene_transitions.keep_current_scene(correspondence);
            transition = neuralpass::SceneTransition::camera_cut;
        } else if (manual == ManualSceneCommand::start_new) {
            scene_transitions.start_new_scene(correspondence);
            transition = neuralpass::SceneTransition::scene_change;
            force_new_scene = true;
        } else if (manual == ManualSceneCommand::merge_visible) {
            if (active_scene && !visible_stable.empty()) {
                const auto imported = scene_catalog.resolve(visible_stable);
                if (!active_stream && imported.valid() &&
                    imported.identity != active_scene->identity)
                    (void)material_baker.load_cache(
                        active_namespace.atlas_root(imported.directory), false);
                active_scene_materials.insert(visible_stable.begin(), visible_stable.end());
                (void)scene_catalog.record(active_scene->identity, visible_stable);
            } else if (!active_scene) {
                activate_scene(visible_stable);
            }
            scene_transitions.keep_current_scene(correspondence);
            transition = neuralpass::SceneTransition::camera_cut;
        }
        if (transition == neuralpass::SceneTransition::scene_change) {
            save_active_scene();
            material_baker.reset_scene();
            published_replacements.clear();
            {
                std::lock_guard lock(state->mutex);
                state->ready_replacements.clear();
            }
            ++state->replacement_epoch;
            active_scene.reset();
            active_scene_materials.clear();
            state->active_scene_identity = 0;
            activate_scene(visible_stable, force_new_scene);
            bridge_bake.reset();
        } else if (transition == neuralpass::SceneTransition::camera_cut ||
                   transition == neuralpass::SceneTransition::pending_scene_change) {
            material_baker.invalidate_in_flight();
            bridge_bake.reset();
        }
        if (transition != neuralpass::SceneTransition::scene_change &&
            transition != neuralpass::SceneTransition::pending_scene_change) {
            if (!active_scene) activate_scene(visible_stable);
            else active_scene_materials.insert(visible_stable.begin(), visible_stable.end());
        }
        // A foreign cut is deliberately quarantined: screen-space output may
        // continue, but no UV observation is allowed to read or mutate either
        // scene's persistent atlases until identity resolves.
        if (transition == neuralpass::SceneTransition::pending_scene_change)
            correspondence.clear();
        state->scene_transition = static_cast<int>(transition);
        state->scene_generation = scene_transitions.scene_key().generation;

        auto [latest_binding_controls, binding_style_revision] =
            snapshot_binding_controls();
        if (binding_style_revision != active_binding_style_revision) {
            save_active_scene();
            binding_controls = std::move(latest_binding_controls);
            active_binding_style_revision = binding_style_revision;
            if (!active_stream)
                active_namespace = cache_namespace_for(
                    k_presets.at(static_cast<std::size_t>(active_preset)), backend->name(),
                    neuralpass::binding_style_control_identity(binding_controls));
            material_baker.reset_scene();
            published_replacements.clear();
            {
                std::lock_guard lock(state->mutex);
                state->ready_replacements.clear();
                state->bridge_ready.clear();
            }
            ++state->replacement_epoch;
            ++style_generation;
            history = {};
            bridge_bake.reset();
            if (!active_stream && active_scene && active_scene->valid()) {
                const auto loaded = material_baker.load_cache(
                    active_namespace.atlas_root(active_scene->directory));
                if (loaded.loaded != 0)
                    reshade::log::message(reshade::log::level::info,
                        ("NeuralPass loaded " + std::to_string(loaded.loaded) +
                         " atlases for the selected binding-control namespace.").c_str());
            }
        } else {
            binding_controls = std::move(latest_binding_controls);
        }
        auto binding_style = neuralpass::plan_binding_styles(
            frame.width, frame.height, correspondence, binding_controls);
        correspondence = binding_style.active_correspondence;
        if (active_stream) {
            auto current_plan = material_baker.plan(
                current, correspondence, reveal_classes ? &*reveal_classes : nullptr);
            publish_replacement_snapshots(
                *state, material_baker, correspondence, published_replacements, false);
            const auto seeded = pack(current_plan.composite);
            const auto resized = resize_nearest(seeded, k_stream_width, k_stream_height);
            const auto now = GetTickCount64();
            // A latest-frame mailbox feeds the lightweight optical-flow path independently
            // of diffusion. Cap it near 60 Hz to avoid needless NTFS traffic at high FPS.
            if (now - state->last_live_write_tick >= 16) {
                const auto live_sequence = std::max<std::uint64_t>(now, state->live_sent.load() + 1);
                if (write_live_frame(resized, live_sequence)) {
                    state->live_sent = live_sequence;
                    state->last_live_write_tick = now;
                }
            }
            CapturedFrame live;
            std::uint64_t live_sequence = 0;
            if (read_live_frame(state->live_received.load(), live, live_sequence)) {
                auto full_live = resize_nearest(live, frame.width, frame.height);
                auto live_image = unpack(full_live);
                Image<std::uint8_t> live_valid(frame.width, frame.height, 1);
                neuralpass::apply_binding_styles(live_image, live_valid, current,
                    binding_style.strength_percent, {0, 0, frame.width, frame.height});
                auto reconstructed = material_baker.reconstruct(live_image, correspondence);
                publish_bridge_results(state, {pack(reconstructed)}, frame.width, frame.height);
                state->live_received = live_sequence;
            }

            const auto sent = state->bridge_sent.load();
            auto received = state->bridge_received.load();
            {
                std::lock_guard lock(state->mutex);
                state->backend_name = sent == received ? "stream (ready)" : "stream (generating)";
            }
            if (sent != 0 && received != sent) {
                std::vector<CapturedFrame> results;
                if (read_bridge_frames(sent, results)) {
                    received = sent;
                    state->bridge_received = received;
                    // The motion mailbox will immediately warp this new anchor. Publish only
                    // the final diffusion result as a fallback, never the old RIFE burst.
                    if (!results.empty() && bridge_bake && bridge_bake->sequence == sent &&
                        bridge_bake->scene == scene_transitions.scene_key() &&
                        bridge_bake->style_generation == style_generation) {
                        auto generated = resize_nearest(results.back(), bridge_bake->width,
                                                        bridge_bake->height);
                        auto generated_image = unpack(generated);
                        Image<std::uint8_t> generated_valid(
                            bridge_bake->width, bridge_bake->height, 1);
                        neuralpass::apply_binding_styles(generated_image, generated_valid,
                            bridge_bake->plan.live_frame, bridge_bake->binding_strength,
                            {0, 0, bridge_bake->width, bridge_bake->height});
                        const auto baked = material_baker.commit(
                            bridge_bake->plan, generated_image, bridge_bake->correspondence);
                        if (!baked.stale) {
                            if (baked.accepted != 0)
                                publish_replacement_snapshots(*state, material_baker,
                                    bridge_bake->correspondence, published_replacements, true);
                            generated_image = material_baker.reconstruct(
                                generated_image, bridge_bake->correspondence);
                            publish_bridge_results(state, {pack(generated_image)},
                                bridge_bake->width, bridge_bake->height);
                        }
                        bridge_bake.reset();
                    }
                }
            }
            if (sent == received) {
                const auto sequence = std::max<std::uint64_t>(GetTickCount64(), sent + 1);
                if (write_bridge_frame(resized, sequence)) {
                    state->bridge_sent = sequence;
                    bridge_bake = BridgeBakeContext {sequence, style_generation,
                        scene_transitions.scene_key(),
                        frame.width, frame.height,
                        std::move(current_plan), correspondence,
                        binding_style.strength_percent};
                }
            }
            continue;
        }
        const int requested = state->requested_preset.load();
        if (requested != active_preset) {
            save_active_scene();
            active_preset = requested;
            backend = load_backend(active_preset);
            active_namespace = cache_namespace_for(
                k_presets.at(static_cast<std::size_t>(active_preset)), backend->name(),
                neuralpass::binding_style_control_identity(binding_controls));
            material_baker.reset_scene();
            published_replacements.clear();
            {
                std::lock_guard lock(state->mutex);
                state->ready_replacements.clear();
                state->bridge_ready.clear();
                if (state->ready_width != 0 && state->ready_height != 0) {
                    const auto bytes = static_cast<std::size_t>(state->ready_width) *
                                       state->ready_height * 4;
                    state->ready_styled.assign(bytes, 0);
                    state->ready_valid.assign(bytes, 0);
                    state->has_ready = true;
                }
                state->backend_name = std::string(backend->name());
            }
            ++state->replacement_epoch;
            ++style_generation;
            if (active_scene && active_scene->valid()) {
                const auto loaded = material_baker.load_cache(
                    active_namespace.atlas_root(active_scene->directory));
                if (loaded.loaded != 0)
                    reshade::log::message(reshade::log::level::info,
                        ("NeuralPass loaded " + std::to_string(loaded.loaded) +
                         " atlases for the selected style/model namespace.").c_str());
            }
            history = {};
        }
        if (state->reset_requested.exchange(false)) history = {};
        const bool sticky_history = state->sticky_history.load();
        const bool reset = history.source.width() != frame.width ||
                           history.source.height() != frame.height || visual_cut;
        if (reset) {
            history.source = current;
            history.styled = current;
            history.depth = Image<float>(frame.width, frame.height, 1.0f);
            history.valid = Image<std::uint8_t>(frame.width, frame.height, 0);
            history.age = Image<std::uint16_t>(frame.width, frame.height, 0);
        } else {
            neuralpass::TemporalSettings temporal_settings;
            temporal_settings.maximum_age = static_cast<std::uint16_t>(state->refresh_age.load());
            if (sticky_history)
                temporal_settings.color_threshold = 1.0f;
            auto temporal = neuralpass::reproject_history(
                current, nullptr, nullptr, history, temporal_settings);
            history = std::move(temporal.reprojected);
        }

        const auto bake_plan = material_baker.plan(
            current, correspondence, reveal_classes ? &*reveal_classes : nullptr);
        publish_replacement_snapshots(
            *state, material_baker, correspondence, published_replacements, false);
        for (const auto &sample : correspondence) {
            if (sample.screen_x >= frame.width || sample.screen_y >= frame.height) continue;
            if (bake_plan.reveal_mask.at(sample.screen_x, sample.screen_y) == 0) {
                history.styled.at(sample.screen_x, sample.screen_y) =
                    bake_plan.composite.at(sample.screen_x, sample.screen_y);
                history.valid.at(sample.screen_x, sample.screen_y) = 1;
            } else {
                history.valid.at(sample.screen_x, sample.screen_y) = 0;
            }
        }
        neuralpass::invalidate_bypassed_bindings(
            history.valid, binding_style.strength_percent);

        Image<std::uint8_t> dirty(frame.width, frame.height, 0);
        for (std::size_t i = 0; i < dirty.size(); ++i)
            dirty.pixels()[i] = binding_style.strength_percent.pixels()[i] == 0
                ? 0 : (history.valid.pixels()[i] ? 0 : 1);
        const auto maximum_tile_budget = state->tile_budget.load();
        const bool adaptive_budget = state->adaptive_tile_budget.load();
        if (adaptive_budget && !adaptive_was_enabled) {
            budget_controller.reset(std::min<std::uint32_t>(1, maximum_tile_budget));
        }
        adaptive_was_enabled = adaptive_budget;
        const auto admitted_tile_budget = adaptive_budget
            ? std::min(budget_controller.budget(), maximum_tile_budget)
            : maximum_tile_budget;
        state->effective_tile_budget = admitted_tile_budget;
        const auto jobs = neuralpass::schedule_tiles(dirty, history.age,
            {.tile_size=256, .halo=32, .dilation_radius=4,
             .tile_budget=admitted_tile_budget,
             .refresh_age=static_cast<std::uint16_t>(state->refresh_age.load())},
            &bake_plan.inpaint_mask);

        // The cross-architecture worker echoes all four identity dimensions.
        // A mismatched response can never be committed as a plausible tile.
        std::vector<std::uint64_t> visible_bindings;
        visible_bindings.reserve(correspondence.size());
        for (const auto &sample : correspondence)
            if (sample.material_id != 0) visible_bindings.push_back(sample.material_id);
        std::sort(visible_bindings.begin(), visible_bindings.end());
        visible_bindings.erase(std::unique(visible_bindings.begin(), visible_bindings.end()),
                               visible_bindings.end());
        std::uint64_t binding_generation = k_fnv_offset;
        for (const auto binding : visible_bindings) hash_value(binding_generation, binding);
        backend->set_generations({scene_transitions.scene_key().generation,
                                  style_generation,
                                  binding_generation});

        const auto inference_started = std::chrono::steady_clock::now();
        for (const auto &job : jobs) {
            try {
                const auto input = extract(current, job.padded);
                const auto output = backend->run(input);
                const auto ox = job.core.x - job.padded.x;
                const auto oy = job.core.y - job.padded.y;
                for (std::uint32_t y = 0; y < job.core.height; ++y)
                    for (std::uint32_t x = 0; x < job.core.width; ++x) {
                        const auto dx = job.core.x + x;
                        const auto dy = job.core.y + y;
                        history.source.at(dx, dy) = current.at(dx, dy);
                        history.styled.at(dx, dy) = output.at(ox + x, oy + y);
                        history.valid.at(dx, dy) = 1;
                        history.age.at(dx, dy) = 0;
                    }
                neuralpass::apply_binding_styles(history.styled, history.valid, current,
                    binding_style.strength_percent, job.core);
            } catch (const std::exception &error) {
                reshade::log::message(reshade::log::level::error, error.what());
            }
        }
        const auto inference_elapsed = std::chrono::duration<float, std::milli>(
            std::chrono::steady_clock::now() - inference_started).count();
        if (adaptive_budget) {
            const auto target_fps = std::clamp(state->target_frame_rate.load(), 30u, 240u);
            budget_controller.configure({
                .minimum_budget=0,
                .maximum_budget=maximum_tile_budget,
                .target_frame_time_ms=1000.0f / static_cast<float>(target_fps),
                .recovery_samples=30});
            const auto decision = budget_controller.observe({
                .frame_time_ms=static_cast<float>(state->measured_frame_time_us.load()) / 1000.0f,
                .worker_time_ms=inference_elapsed,
                .attempted_tiles=static_cast<std::uint32_t>(jobs.size()),
                .capture_backlog=state->dropped.load() != dropped_before_processing});
            state->effective_tile_budget = decision.tile_budget;
            state->measured_tile_time_us = static_cast<std::uint64_t>(
                std::max(0.0f, decision.smoothed_tile_time_ms) * 1000.0f);
            if (decision.under_pressure) ++state->budget_pressure_events;
        } else {
            state->effective_tile_budget = maximum_tile_budget;
            state->measured_tile_time_us = jobs.empty() ? 0 :
                static_cast<std::uint64_t>(inference_elapsed * 1000.0f /
                                           static_cast<float>(jobs.size()));
        }

        std::vector<neuralpass::SurfaceCorrespondence> generated_reveals;
        generated_reveals.reserve(correspondence.size());
        for (const auto &sample : correspondence)
            if (sample.screen_x < frame.width && sample.screen_y < frame.height &&
                bake_plan.reveal_mask.at(sample.screen_x, sample.screen_y) != 0 &&
                history.valid.at(sample.screen_x, sample.screen_y) != 0)
                generated_reveals.push_back(sample);
        const auto baked = material_baker.commit(
            bake_plan, history.styled, generated_reveals);
        if (!baked.stale && baked.accepted != 0) {
            publish_replacement_snapshots(
                *state, material_baker, generated_reveals, published_replacements, true);
            history.styled = material_baker.reconstruct(history.styled, correspondence);
        }

        std::vector<std::uint8_t> styled(history.styled.size() * 4);
        std::vector<std::uint8_t> valid(history.valid.size() * 4);
        for (std::size_t i = 0; i < history.styled.size(); ++i) {
            styled[i*4+0] = byte(history.styled.pixels()[i].r);
            styled[i*4+1] = byte(history.styled.pixels()[i].g);
            styled[i*4+2] = byte(history.styled.pixels()[i].b);
            styled[i*4+3] = 255;
            valid[i*4+0] = valid[i*4+1] = valid[i*4+2] = history.valid.pixels()[i] ? 255 : 0;
            valid[i*4+3] = 255;
        }
        {
            std::lock_guard lock(state->mutex);
            state->ready_styled = std::move(styled);
            state->ready_valid = std::move(valid);
            state->ready_width = frame.width;
            state->ready_height = frame.height;
            state->has_ready = true;
            ++state->completed;
        }
    }

    save_active_scene();
}

void find_effect_variables(reshade::api::effect_runtime *runtime) {
    auto *state = runtime->get_private_data<RuntimeState>();
    if (!state) return;
    state->styled_variable = runtime->find_texture_variable("NeuralPass.fx", "NeuralPassStyled");
    state->valid_variable = runtime->find_texture_variable("NeuralPass.fx", "NeuralPassValid");
    state->color_mode_variable = runtime->find_uniform_variable(
        "NeuralPass.fx", "NeuralPassColorMode");
}

struct CaptureColorContract {
    neuralpass::CapturePixelLayout layout;
    neuralpass::DisplayEncoding encoding;
    std::uint32_t bytes_per_pixel;
};

const char *api_name(reshade::api::device_api api) {
    switch (api) {
    case reshade::api::device_api::d3d9: return "d3d9";
    case reshade::api::device_api::d3d10: return "d3d10";
    case reshade::api::device_api::d3d11: return "d3d11";
    case reshade::api::device_api::d3d12: return "d3d12";
    case reshade::api::device_api::vulkan: return "vulkan";
    case reshade::api::device_api::opengl: return "opengl";
    default: return "unknown";
    }
}

const char *format_name(reshade::api::format format) {
    switch (format) {
    case reshade::api::format::r8g8b8a8_unorm: return "r8g8b8a8_unorm";
    case reshade::api::format::r8g8b8a8_unorm_srgb: return "r8g8b8a8_unorm_srgb";
    case reshade::api::format::r8g8b8x8_unorm: return "r8g8b8x8_unorm";
    case reshade::api::format::r8g8b8x8_unorm_srgb: return "r8g8b8x8_unorm_srgb";
    case reshade::api::format::b8g8r8a8_unorm: return "b8g8r8a8_unorm";
    case reshade::api::format::b8g8r8a8_unorm_srgb: return "b8g8r8a8_unorm_srgb";
    case reshade::api::format::b8g8r8x8_unorm: return "b8g8r8x8_unorm";
    case reshade::api::format::b8g8r8x8_unorm_srgb: return "b8g8r8x8_unorm_srgb";
    case reshade::api::format::r16g16b16a16_float: return "r16g16b16a16_float";
    case reshade::api::format::r10g10b10a2_unorm: return "r10g10b10a2_unorm";
    default: return "other";
    }
}

const char *color_space_name(reshade::api::color_space color_space) {
    switch (color_space) {
    case reshade::api::color_space::unknown: return "unknown";
    case reshade::api::color_space::srgb: return "srgb";
    case reshade::api::color_space::scrgb: return "scrgb";
    case reshade::api::color_space::hdr10_pq: return "hdr10_pq";
    case reshade::api::color_space::hdr10_hlg: return "hdr10_hlg";
    default: return "other";
    }
}

const char *encoding_name(neuralpass::DisplayEncoding encoding) {
    switch (encoding) {
    case neuralpass::DisplayEncoding::sdr_srgb: return "sdr_srgb";
    case neuralpass::DisplayEncoding::scrgb_linear: return "scrgb_linear";
    case neuralpass::DisplayEncoding::hdr10_pq: return "hdr10_pq";
    default: return "unknown";
    }
}

std::optional<CaptureColorContract> capture_color_contract(
        reshade::api::format format, reshade::api::color_space color_space) {
    switch (format) {
    case reshade::api::format::r8g8b8a8_unorm:
    case reshade::api::format::r8g8b8a8_unorm_srgb:
    case reshade::api::format::r8g8b8x8_unorm:
    case reshade::api::format::r8g8b8x8_unorm_srgb:
        if (color_space == reshade::api::color_space::unknown ||
            color_space == reshade::api::color_space::srgb)
            return CaptureColorContract {neuralpass::CapturePixelLayout::rgba8,
                neuralpass::DisplayEncoding::sdr_srgb, 4};
        return std::nullopt;
    case reshade::api::format::b8g8r8a8_unorm:
    case reshade::api::format::b8g8r8a8_unorm_srgb:
    case reshade::api::format::b8g8r8x8_unorm:
    case reshade::api::format::b8g8r8x8_unorm_srgb:
        if (color_space == reshade::api::color_space::unknown ||
            color_space == reshade::api::color_space::srgb)
            return CaptureColorContract {neuralpass::CapturePixelLayout::bgra8,
                neuralpass::DisplayEncoding::sdr_srgb, 4};
        return std::nullopt;
    case reshade::api::format::r16g16b16a16_float:
        if (color_space == reshade::api::color_space::scrgb)
            return CaptureColorContract {neuralpass::CapturePixelLayout::rgba16_float,
                neuralpass::DisplayEncoding::scrgb_linear, 8};
        return std::nullopt;
    case reshade::api::format::r10g10b10a2_unorm:
        if (color_space == reshade::api::color_space::hdr10_pq)
            return CaptureColorContract {neuralpass::CapturePixelLayout::rgb10a2,
                neuralpass::DisplayEncoding::hdr10_pq, 4};
        return std::nullopt;
    default:
        return std::nullopt;
    }
}

void write_runtime_evidence(RuntimeState &state, reshade::api::device_api api,
                            const reshade::api::resource_desc &source_desc,
                            reshade::api::color_space color_space,
                            const std::optional<CaptureColorContract> &contract) {
    const bool supported = contract.has_value();
    if (state.runtime_evidence_written && state.evidence_api == api &&
        state.evidence_format == source_desc.texture.format &&
        state.evidence_color_space == color_space &&
        state.evidence_width == source_desc.texture.width &&
        state.evidence_height == source_desc.texture.height &&
        state.evidence_supported == supported)
        return;

    std::lock_guard evidence_lock(g_runtime_evidence_mutex);
    const auto target = g_addon_directory / "NeuralPass-runtime-evidence.json";
    const auto temporary = g_addon_directory / "NeuralPass-runtime-evidence.tmp";
    DeleteFileW(temporary.c_str());
    SYSTEMTIME now {};
    GetSystemTime(&now);
    char timestamp[32] = {};
    std::snprintf(timestamp, sizeof(timestamp), "%04u-%02u-%02uT%02u:%02u:%02u.%03uZ",
        now.wYear, now.wMonth, now.wDay, now.wHour, now.wMinute, now.wSecond,
        now.wMilliseconds);
    bool serialized = false;
    {
        std::ofstream output(temporary, std::ios::trunc);
        if (output) {
            const char *architecture =
#if defined(_WIN64)
                "X64";
#else
                "X86";
#endif
            output << neuralpass::runtime_display_evidence_json({
                .generated_utc = timestamp,
                .process_architecture = architecture,
                .graphics_api = api_name(api),
                .graphics_api_value = static_cast<std::uint32_t>(api),
                .width = source_desc.texture.width,
                .height = source_desc.texture.height,
                .backbuffer_format = format_name(source_desc.texture.format),
                .backbuffer_format_value =
                    static_cast<std::uint32_t>(source_desc.texture.format),
                .swapchain_color_space = color_space_name(color_space),
                .swapchain_color_space_value = static_cast<std::uint32_t>(color_space),
                .display_capture_supported = supported,
                .display_encoding = supported ? encoding_name(contract->encoding) : "bypassed",
                .hdr_path = supported &&
                    contract->encoding != neuralpass::DisplayEncoding::sdr_srgb,
                .classification = supported ? "compatible_format_color_space"
                    : "unsupported_or_mismatched_format_color_space",
            });
            output.flush();
            serialized = output.good();
        }
    }
    const bool written = serialized && MoveFileExW(temporary.c_str(), target.c_str(),
        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    if (!written) DeleteFileW(temporary.c_str());
    if (!written && !state.runtime_evidence_write_failed) {
        reshade::log::message(reshade::log::level::warning,
            "NeuralPass could not publish runtime API/HDR evidence beside the game.");
        state.runtime_evidence_write_failed = true;
    }
    if (written) {
        state.runtime_evidence_written = true;
        state.runtime_evidence_write_failed = false;
        state.evidence_api = api;
        state.evidence_format = source_desc.texture.format;
        state.evidence_color_space = color_space;
        state.evidence_width = source_desc.texture.width;
        state.evidence_height = source_desc.texture.height;
        state.evidence_supported = supported;
    }
}

void destroy_readback(reshade::api::effect_runtime *runtime, RuntimeState &state) {
    auto *device = runtime->get_device();
    for (auto &resource : state.readback) {
        if (resource != 0) device->destroy_resource(resource);
        resource = {};
    }
    state.capture_width = state.capture_height = 0;
    state.capture_format = reshade::api::format::unknown;
    state.next_signal = state.next_read = 1;
}

bool ensure_readback(reshade::api::effect_runtime *runtime, RuntimeState &state,
                     const reshade::api::resource_desc &source_desc,
                     reshade::api::color_space color_space) {
    const auto contract = capture_color_contract(source_desc.texture.format, color_space);
    write_runtime_evidence(state, runtime->get_device()->get_api(), source_desc,
                           color_space, contract);
    if (!contract) {
        if (state.rejected_format != source_desc.texture.format ||
            state.rejected_color_space != color_space) {
            reshade::log::message(reshade::log::level::warning,
                "NeuralPass bypassed a backbuffer whose format/color-space pair is unsupported.");
            state.rejected_format = source_desc.texture.format;
            state.rejected_color_space = color_space;
        }
        return false;
    }
    if (state.capture_width == source_desc.texture.width &&
        state.capture_height == source_desc.texture.height &&
        state.capture_format == source_desc.texture.format &&
        state.capture_encoding == contract->encoding && state.readback[0] != 0)
        return true;
    runtime->get_command_queue()->wait_idle();
    destroy_readback(runtime, state);
    auto desc = source_desc;
    desc.type = reshade::api::resource_type::texture_2d;
    desc.heap = reshade::api::memory_heap::readback;
    desc.usage = reshade::api::resource_usage::copy_dest;
    desc.flags = reshade::api::resource_flags::none;
    for (auto &resource : state.readback) {
        if (!runtime->get_device()->create_resource(desc, nullptr,
                reshade::api::resource_usage::copy_dest, &resource)) {
            destroy_readback(runtime, state);
            reshade::log::message(reshade::log::level::error,
                "NeuralPass could not create a readback ring.");
            return false;
        }
    }
    state.capture_width = source_desc.texture.width;
    state.capture_height = source_desc.texture.height;
    state.capture_format = source_desc.texture.format;
    state.capture_layout = contract->layout;
    state.capture_encoding = contract->encoding;
    state.capture_bytes_per_pixel = contract->bytes_per_pixel;
    state.rejected_format = reshade::api::format::unknown;
    state.rejected_color_space = reshade::api::color_space::unknown;
    return true;
}

void on_init_swapchain(reshade::api::swapchain *swapchain, bool) {
    std::lock_guard lock(g_baker_probe_mutex);
    g_swapchain_color_spaces[swapchain->get_hwnd()] = swapchain->get_color_space();
}

void on_destroy_swapchain(reshade::api::swapchain *swapchain, bool resize) {
    std::lock_guard lock(g_baker_probe_mutex);
    g_swapchain_color_spaces.erase(swapchain->get_hwnd());
    if (resize && swapchain->get_device()->get_api() == reshade::api::device_api::d3d9) {
        if (const auto found = g_surface_captures.find(swapchain->get_device());
            found != g_surface_captures.end()) {
            // D3D9 default-pool capture resources must be released before
            // IDirect3DDevice9::Reset. on_begin_effects recreates them at the new
            // dimensions after the swapchain returns. Modern backends preserve
            // their API attachment and resize their surfaces in initialize().
            found->second->reset();
            ++g_swapchain_resets;
        }
    }
}

void on_present(reshade::api::command_queue *, reshade::api::swapchain *swapchain,
                const reshade::api::rect *, const reshade::api::rect *, std::uint32_t,
                const reshade::api::rect *) {
    std::lock_guard lock(g_baker_probe_mutex);
    g_swapchain_color_spaces[swapchain->get_hwnd()] = swapchain->get_color_space();
}

void on_baker_init_device(reshade::api::device *device) {
    std::unique_ptr<neuralpass::capture::SurfaceCaptureBackend> capture;
    if (device->get_api() == reshade::api::device_api::d3d12) {
        auto backend = std::make_unique<neuralpass::d3d12_capture::SurfaceCapture>();
        backend->attach(device, nullptr);
        capture = std::move(backend);
    } else if (device->get_api() == reshade::api::device_api::vulkan) {
        auto backend = std::make_unique<neuralpass::vulkan_capture::SurfaceCapture>();
        backend->attach(device, nullptr);
        capture = std::move(backend);
    } else {
        return;
    }
    std::lock_guard lock(g_baker_probe_mutex);
    g_surface_captures.try_emplace(device, std::move(capture));
}

void on_baker_destroy_device(reshade::api::device *device) {
    std::unique_ptr<neuralpass::capture::SurfaceCaptureBackend> capture;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_surface_captures.find(device);
            found != g_surface_captures.end()) {
            capture = std::move(found->second);
            g_surface_captures.erase(found);
        }
    }
}

std::uint32_t find_dxgi_adapter_index(ID3D11Device *device) {
    if (device == nullptr) return 0;
    IDXGIDevice *dxgi_device = nullptr;
    IDXGIAdapter *game_adapter = nullptr;
    IDXGIFactory *factory = nullptr;
    std::uint32_t result = 0;
    if (SUCCEEDED(device->QueryInterface(IID_PPV_ARGS(&dxgi_device))) &&
        SUCCEEDED(dxgi_device->GetAdapter(&game_adapter)) &&
        SUCCEEDED(game_adapter->GetParent(IID_PPV_ARGS(&factory)))) {
        DXGI_ADAPTER_DESC game_desc {};
        if (SUCCEEDED(game_adapter->GetDesc(&game_desc))) {
            for (UINT index = 0;; ++index) {
                IDXGIAdapter *candidate = nullptr;
                if (factory->EnumAdapters(index, &candidate) == DXGI_ERROR_NOT_FOUND) break;
                DXGI_ADAPTER_DESC candidate_desc {};
                const bool match = SUCCEEDED(candidate->GetDesc(&candidate_desc)) &&
                    candidate_desc.AdapterLuid.HighPart == game_desc.AdapterLuid.HighPart &&
                    candidate_desc.AdapterLuid.LowPart == game_desc.AdapterLuid.LowPart;
                candidate->Release();
                if (match) { result = index; break; }
            }
        }
    }
    if (factory != nullptr) factory->Release();
    if (game_adapter != nullptr) game_adapter->Release();
    if (dxgi_device != nullptr) dxgi_device->Release();
    return result;
}

void on_init(reshade::api::effect_runtime *runtime) {
    auto *state = runtime->create_private_data<RuntimeState>();
    state->directml_enabled =
        runtime->get_device()->get_api() == reshade::api::device_api::d3d11;
    if (state->directml_enabled)
        state->directml_device_id = find_dxgi_adapter_index(
            reinterpret_cast<ID3D11Device *>(runtime->get_device()->get_native()));
    std::copy_n(k_default_prompt, std::min(sizeof(k_default_prompt), state->prompt.size()),
                state->prompt.data());
    const auto prompt_path = bridge_directory() / "prompt.txt";
    std::ifstream prompt_input(prompt_path);
    if (prompt_input) {
        const std::string prompt((std::istreambuf_iterator<char>(prompt_input)),
                                 std::istreambuf_iterator<char>());
        state->prompt.fill(0);
        std::copy_n(prompt.data(), std::min(prompt.size(), state->prompt.size() - 1),
                    state->prompt.data());
    } else {
        save_prompt(*state);
    }
    state->prompt_identity = stream_prompt_identity(state->prompt.data());
    load_prompt_history(*state);
    if (!runtime->get_device()->create_fence(0, reshade::api::fence_flags::none,
                                              &state->copy_fence))
        reshade::log::message(reshade::log::level::error, "NeuralPass could not create its copy fence.");
    state->worker = std::jthread([state](std::stop_token token) { process_frames(state, token); });
    if (runtime->get_device()->get_api() == reshade::api::device_api::d3d9) {
        const auto back_buffer = runtime->get_back_buffer(0);
        const auto desc = runtime->get_device()->get_resource_desc(back_buffer);
        auto capture = std::make_unique<neuralpass::d3d9_capture::SurfaceCapture>();
        if (capture->initialize(
                reinterpret_cast<IDirect3DDevice9 *>(runtime->get_device()->get_native()),
                desc.texture.width, desc.texture.height)) {
            std::lock_guard lock(g_baker_probe_mutex);
            g_surface_captures[runtime->get_device()] = std::move(capture);
        } else {
            reshade::log::message(reshade::log::level::warning,
                "NeuralPass could not initialize the D3D9 mesh-UV capture surface.");
        }
    } else if (runtime->get_device()->get_api() == reshade::api::device_api::d3d10) {
        const auto back_buffer = runtime->get_back_buffer(0);
        const auto desc = runtime->get_device()->get_resource_desc(back_buffer);
        auto capture = std::make_unique<neuralpass::d3d10_capture::SurfaceCapture>();
        if (capture->initialize(
                reinterpret_cast<ID3D10Device *>(runtime->get_device()->get_native()),
                desc.texture.width, desc.texture.height)) {
            std::lock_guard lock(g_baker_probe_mutex);
            g_surface_captures[runtime->get_device()] = std::move(capture);
        } else {
            reshade::log::message(reshade::log::level::warning,
                "NeuralPass could not initialize the D3D10 mesh-UV capture surface.");
        }
    } else if (runtime->get_device()->get_api() == reshade::api::device_api::d3d11) {
        const auto back_buffer = runtime->get_back_buffer(0);
        const auto desc = runtime->get_device()->get_resource_desc(back_buffer);
        auto capture = std::make_unique<neuralpass::d3d11_capture::SurfaceCapture>();
        if (capture->initialize(
                reinterpret_cast<ID3D11Device *>(runtime->get_device()->get_native()),
                desc.texture.width, desc.texture.height)) {
            std::lock_guard lock(g_baker_probe_mutex);
            g_surface_captures[runtime->get_device()] = std::move(capture);
        } else {
            reshade::log::message(reshade::log::level::warning,
                "NeuralPass could not initialize the D3D11 mesh-UV capture surface.");
        }
    } else if (runtime->get_device()->get_api() == reshade::api::device_api::d3d12) {
        const auto back_buffer = runtime->get_back_buffer(0);
        const auto desc = runtime->get_device()->get_resource_desc(back_buffer);
        std::lock_guard lock(g_baker_probe_mutex);
        auto found = g_surface_captures.find(runtime->get_device());
        if (found == g_surface_captures.end()) {
            auto capture = std::make_unique<neuralpass::d3d12_capture::SurfaceCapture>();
            found = g_surface_captures.emplace(runtime->get_device(), std::move(capture)).first;
        }
        auto *capture = dynamic_cast<neuralpass::d3d12_capture::SurfaceCapture *>(
            found->second.get());
        if (capture != nullptr) {
            capture->attach(runtime->get_device(), runtime->get_command_queue());
            if (!capture->initialize(
                    reinterpret_cast<void *>(runtime->get_device()->get_native()),
                    desc.texture.width, desc.texture.height))
                reshade::log::message(reshade::log::level::warning,
                    "NeuralPass could not initialize the D3D12 mesh-UV capture surface.");
        }
    } else if (runtime->get_device()->get_api() == reshade::api::device_api::vulkan) {
        const auto back_buffer = runtime->get_back_buffer(0);
        const auto desc = runtime->get_device()->get_resource_desc(back_buffer);
        std::lock_guard lock(g_baker_probe_mutex);
        auto found = g_surface_captures.find(runtime->get_device());
        if (found == g_surface_captures.end()) {
            auto capture = std::make_unique<neuralpass::vulkan_capture::SurfaceCapture>();
            found = g_surface_captures.emplace(runtime->get_device(), std::move(capture)).first;
        }
        auto *capture = dynamic_cast<neuralpass::vulkan_capture::SurfaceCapture *>(
            found->second.get());
        if (capture != nullptr) {
            capture->attach(runtime->get_device(), runtime->get_command_queue());
            if (!capture->initialize(nullptr, desc.texture.width, desc.texture.height))
                reshade::log::message(reshade::log::level::warning,
                    "NeuralPass could not initialize the Vulkan mesh-UV capture surface.");
        }
    }
    find_effect_variables(runtime);
}

void on_destroy(reshade::api::effect_runtime *runtime) {
    auto *state = runtime->get_private_data<RuntimeState>();
    if (state != nullptr) {
        {
            std::lock_guard lock(state->mutex);
            state->stop = true;
        }
        state->worker.request_stop();
        state->wake.notify_all();
        if (state->worker.joinable()) state->worker.join();
        runtime->get_command_queue()->wait_idle();
        std::unique_ptr<neuralpass::capture::SurfaceCaptureBackend> capture;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            if (const auto found = g_surface_captures.find(runtime->get_device());
                found != g_surface_captures.end()) {
                capture = std::move(found->second);
                g_surface_captures.erase(found);
            }
        }
        destroy_readback(runtime, *state);
        if (state->copy_fence != 0) runtime->get_device()->destroy_fence(state->copy_fence);
    }
    {
        std::error_code error;
        std::filesystem::create_directories(bridge_directory(), error);
        std::ofstream report(bridge_directory() / "baker_probe.txt", std::ios::trunc);
        std::size_t material_count = 0;
        std::size_t binding_style_control_count = 0;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            material_count = g_materials_seen.size();
            binding_style_control_count = g_binding_style_controls.size();
        }
        report << "pipelines=" << g_pipelines_seen.load() << '\n'
               << "uv_pipelines=" << g_uv_pipelines_seen.load() << '\n'
               << "explicit_uv_pipelines=" << g_explicit_uv_pipelines_seen.load() << '\n'
               << "inferred_uv_pipelines=" << g_inferred_uv_pipelines_seen.load() << '\n'
               << "draws=" << g_draws_seen.load() << '\n'
               << "uv_draws=" << g_uv_draws_seen.load() << '\n'
               << "uv_vertices=" << g_uv_vertices_seen.load() << '\n'
               << "pretransform_probe_attempts=" << g_pretransform_probe_attempts.load() << '\n'
               << "pretransform_probe_successes=" << g_pretransform_probe_successes.load() << '\n'
               << "pretransform_uv_samples=" << g_pretransform_uv_samples.load() << '\n';
        report << "material_draws=" << g_material_draws_seen.load() << '\n'
               << "restart_stable_material_draws=" << g_restart_stable_material_draws.load() << '\n'
               << "session_material_draws=" << g_session_material_draws.load() << '\n'
               << "material_texture_candidates=" << g_material_texture_candidates.load() << '\n'
               << "surface_frames_captured=" << g_surface_frames_captured.load() << '\n'
               << "surface_pixels_captured=" << g_surface_pixels_captured.load() << '\n'
               << "swapchain_resets=" << g_swapchain_resets.load() << '\n'
               << "adaptive_tile_budget=" << (state != nullptr && state->adaptive_tile_budget.load()) << '\n'
               << "maximum_tile_budget=" << (state != nullptr ? state->tile_budget.load() : 0) << '\n'
               << "effective_tile_budget=" << (state != nullptr ? state->effective_tile_budget.load() : 0) << '\n'
               << "target_frame_rate=" << (state != nullptr ? state->target_frame_rate.load() : 0) << '\n'
               << "measured_frame_time_us=" << (state != nullptr ? state->measured_frame_time_us.load() : 0) << '\n'
               << "measured_tile_time_us=" << (state != nullptr ? state->measured_tile_time_us.load() : 0) << '\n'
               << "budget_pressure_events=" << (state != nullptr ? state->budget_pressure_events.load() : 0) << '\n'
               << "binding_style_controls=" << binding_style_control_count << '\n'
               << "binding_style_revision=" << g_binding_style_revision.load() << '\n'
               << "unique_materials=" << material_count << '\n';
    }
    runtime->destroy_private_data<RuntimeState>();
}

void on_begin_effects(reshade::api::effect_runtime *runtime, reshade::api::command_list *command_list,
                      reshade::api::resource_view rtv, reshade::api::resource_view) {
    auto *state = runtime->get_private_data<RuntimeState>();
    if (!state) return;
    const auto effect_time = std::chrono::steady_clock::now();
    if (state->last_effect_time.time_since_epoch().count() != 0) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
            effect_time - state->last_effect_time).count();
        // Ignore debugger breaks, loading screens, and clock discontinuities.
        if (elapsed >= 1000 && elapsed <= 250000)
            state->measured_frame_time_us = static_cast<std::uint64_t>(elapsed);
    }
    state->last_effect_time = effect_time;
    auto *device = runtime->get_device();
    auto *queue = runtime->get_command_queue();
    const auto source = device->get_resource_from_view(rtv);
    const auto source_desc = device->get_resource_desc(source);
    reshade::api::color_space color_space = reshade::api::color_space::unknown;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_swapchain_color_spaces.find(runtime->get_hwnd());
            found != g_swapchain_color_spaces.end()) color_space = found->second;
    }
    if ((device->get_api() == reshade::api::device_api::d3d9 ||
         device->get_api() == reshade::api::device_api::d3d10 ||
         device->get_api() == reshade::api::device_api::d3d11 ||
         device->get_api() == reshade::api::device_api::d3d12 ||
         device->get_api() == reshade::api::device_api::vulkan) && command_list != nullptr) {
        neuralpass::capture::SurfaceCaptureBackend *capture = nullptr;
        {
            std::scoped_lock lock(g_baker_probe_mutex, state->mutex);
            if (const auto found = g_surface_captures.find(device);
                found != g_surface_captures.end()) {
                capture = found->second.get();
                if (capture->width() != source_desc.texture.width ||
                    capture->height() != source_desc.texture.height) {
                    state->surface_ready.clear();
                    state->pending_surface.reset();
                    void *native_device = device->get_api() == reshade::api::device_api::vulkan
                        ? nullptr : reinterpret_cast<void *>(device->get_native());
                    if (!capture->initialize(native_device,
                            source_desc.texture.width, source_desc.texture.height)) {
                        reshade::log::message(reshade::log::level::warning,
                            "NeuralPass could not resize the mesh-UV capture surface.");
                    }
                }
                const auto replacement_epoch = state->replacement_epoch.load();
                if (state->applied_replacement_epoch != replacement_epoch) {
                    capture->clear_replacements();
                    state->applied_replacement_epoch = replacement_epoch;
                }
                for (auto &[material_id, mips] : state->ready_replacements)
                    capture->queue_replacement(material_id, std::move(mips));
                state->ready_replacements.clear();
            }
        }
        if (capture != nullptr) {
            auto surface = capture->finish_frame(
                device->get_api() == reshade::api::device_api::vulkan
                    ? static_cast<void *>(command_list)
                    : reinterpret_cast<void *>(command_list->get_native()));
            if (surface) {
                std::uint64_t supported = 0;
                for (const auto &pixel : surface->pixels().pixels())
                    if (pixel.material_id != 0 && pixel.confidence > 0.0f) ++supported;
                ++g_surface_frames_captured;
                g_surface_pixels_captured += supported;
                std::lock_guard lock(state->mutex);
                state->surface_ready.insert_or_assign(
                    surface->frame_index(), std::move(*surface));
                while (state->surface_ready.size() > 6)
                    state->surface_ready.erase(state->surface_ready.begin());
            }
        }
    }
    if (state->copy_fence == 0 ||
        !ensure_readback(runtime, *state, source_desc, color_space)) return;
    const auto width = state->capture_width;
    const auto height = state->capture_height;
    if (state->color_mode_variable != 0)
        runtime->set_uniform_value_int(state->color_mode_variable,
            static_cast<std::int32_t>(state->capture_encoding));

    std::vector<std::uint8_t> styled, valid;
    {
        std::lock_guard lock(state->mutex);
        if (!state->bridge_ready.empty() && state->ready_width == width && state->ready_height == height) {
            styled = std::move(state->bridge_ready.front());
            state->bridge_ready.pop_front();
            valid.resize(styled.size(), 255);
            // Stream frames carry packed UVW.W (visibility/disocclusion confidence)
            // in alpha. Expand that byte into the compositor's validity texture.
            for (std::size_t pixel = 0; pixel < styled.size() / 4; ++pixel) {
                const auto confidence = styled[pixel * 4 + 3];
                valid[pixel * 4 + 0] = confidence;
                valid[pixel * 4 + 1] = confidence;
                valid[pixel * 4 + 2] = confidence;
                valid[pixel * 4 + 3] = 255;
                styled[pixel * 4 + 3] = 255;
            }
        } else if (!state->bridge_ready.empty()) {
            state->bridge_ready.clear();
        } else if (state->has_ready && state->ready_width == width && state->ready_height == height) {
            styled = state->ready_styled;
            valid = state->ready_valid;
            state->has_ready = false;
        } else if (state->has_ready) {
            state->has_ready = false;
            state->ready_styled.clear();
            state->ready_valid.clear();
        }
    }
    if (!styled.empty() && state->styled_variable != 0 && state->valid_variable != 0) {
        runtime->update_texture(state->styled_variable, width, height, styled.data());
        runtime->update_texture(state->valid_variable, width, height, valid.data());
    }

    // Read the oldest completed copy without waiting on the GPU.
    if (state->next_read < state->next_signal && device->wait(state->copy_fence, state->next_read, 0)) {
        const auto index = static_cast<std::size_t>(state->next_read % std::size(state->readback));
        reshade::api::subresource_data mapped = {};
        if (device->map_texture_region(state->readback[index], 0, nullptr,
                reshade::api::map_access::read_only, &mapped)) {
            CapturedFrame captured {width, height,
                std::vector<std::uint8_t>(static_cast<std::size_t>(width)*height*4)};
            const auto *source_bytes = static_cast<const std::uint8_t *>(mapped.data);
            for (std::uint32_t y = 0; y < height; ++y) {
                for (std::uint32_t x = 0; x < width; ++x) {
                    const auto si = static_cast<std::size_t>(y) * mapped.row_pitch +
                        static_cast<std::size_t>(x) * state->capture_bytes_per_pixel;
                    const auto di = (static_cast<std::size_t>(y) * width + x) * 4;
                    const auto converted = neuralpass::decode_capture_pixel(source_bytes + si,
                        state->capture_layout, state->capture_encoding);
                    captured.rgba[di+0] = byte(converted.r);
                    captured.rgba[di+1] = byte(converted.g);
                    captured.rgba[di+2] = byte(converted.b);
                    captured.rgba[di+3] = byte(converted.a);
                }
            }
            device->unmap_texture_region(state->readback[index], 0);
            std::lock_guard lock(state->mutex);
            if (!state->has_pending) {
                state->pending = std::move(captured);
                if (const auto surface = state->surface_ready.find(state->next_read);
                    surface != state->surface_ready.end()) {
                    state->pending_surface = std::move(surface->second);
                    state->surface_ready.erase(surface);
                } else {
                    state->pending_surface.reset();
                }
                state->has_pending = true;
                ++state->submitted;
                state->wake.notify_one();
            } else {
                ++state->dropped;
            }
        }
        ++state->next_read;
    }

    // Never overwrite one of the three copies still in flight.
    if (state->next_signal - state->next_read >= std::size(state->readback)) {
        ++state->dropped;
        return;
    }
    const auto write_index = static_cast<std::size_t>(state->next_signal % std::size(state->readback));
    auto *copy = queue->get_immediate_command_list();
    copy->barrier(source, reshade::api::resource_usage::render_target,
                  reshade::api::resource_usage::copy_source);
    copy->copy_texture_region(source, 0, nullptr, state->readback[write_index], 0, nullptr);
    copy->barrier(source, reshade::api::resource_usage::copy_source,
                  reshade::api::resource_usage::render_target);
    queue->flush_immediate_command_list();
    queue->signal(state->copy_fence, state->next_signal++);
}

void draw_overlay(reshade::api::effect_runtime *runtime) {
    auto *state = runtime->get_private_data<RuntimeState>();
    if (!state) return;
    std::string backend;
    {
        std::lock_guard lock(state->mutex);
        backend = state->backend_name;
    }
    ImGui::Text("Backend: %s", backend.c_str());
    if (state->directml_enabled)
        ImGui::Text("DirectML DXGI adapter index: %u", state->directml_device_id);
    const char *color_path = "SDR sRGB";
    if (state->capture_encoding == neuralpass::DisplayEncoding::scrgb_linear)
        color_path = "scRGB linear (experimental)";
    else if (state->capture_encoding == neuralpass::DisplayEncoding::hdr10_pq)
        color_path = "HDR10 PQ (experimental)";
    ImGui::Text("Display color path: %s", color_path);
    ImGui::Text("Submitted: %llu  Completed: %llu  Dropped: %llu",
        static_cast<unsigned long long>(state->submitted.load()),
        static_cast<unsigned long long>(state->completed.load()),
        static_cast<unsigned long long>(state->dropped.load()));
    const auto frame_time_ms = static_cast<double>(state->measured_frame_time_us.load()) / 1000.0;
    const auto tile_time_ms = static_cast<double>(state->measured_tile_time_us.load()) / 1000.0;
    ImGui::Text("Frame: %.2f ms  Inference/tile: %.2f ms  Active tiles: %u/%u",
        frame_time_ms, tile_time_ms, state->effective_tile_budget.load(),
        state->tile_budget.load());
    ImGui::Text("Adaptive pressure observations: %llu",
        static_cast<unsigned long long>(state->budget_pressure_events.load()));
    ImGui::Text("Visibility K:%llu D:%llu F:%llu O:%llu First:%llu Unsupported:%llu",
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::known_visible)].load()),
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::disoccluded)].load()),
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::newly_front_facing)].load()),
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::offscreen_entry)].load()),
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::first_observation)].load()),
        static_cast<unsigned long long>(state->visibility_counts[
            static_cast<std::size_t>(neuralpass::VisibilityClass::unsupported)].load()));
    ImGui::SeparatorText("Experimental mesh texture baker");
    ImGui::Text("Pipelines: %llu  UV pipelines: %llu",
        static_cast<unsigned long long>(g_pipelines_seen.load()),
        static_cast<unsigned long long>(g_uv_pipelines_seen.load()));
    ImGui::Text("Explicit semantics: %llu  Location-inferred: %llu",
        static_cast<unsigned long long>(g_explicit_uv_pipelines_seen.load()),
        static_cast<unsigned long long>(g_inferred_uv_pipelines_seen.load()));
    ImGui::Text("Draws: %llu  UV draws: %llu  UV vertices: %llu",
        static_cast<unsigned long long>(g_draws_seen.load()),
        static_cast<unsigned long long>(g_uv_draws_seen.load()),
        static_cast<unsigned long long>(g_uv_vertices_seen.load()));
    ImGui::Text("Pre-transform buffers: %llu/%llu readable; UV samples: %llu",
        static_cast<unsigned long long>(g_pretransform_probe_successes.load()),
        static_cast<unsigned long long>(g_pretransform_probe_attempts.load()),
        static_cast<unsigned long long>(g_pretransform_uv_samples.load()));
    std::size_t material_count = 0;
    std::vector<std::uint64_t> material_ids;
    {
        std::lock_guard lock(g_baker_probe_mutex);
        material_count = g_materials_seen.size();
        material_ids.assign(g_materials_seen.begin(), g_materials_seen.end());
    }
    std::sort(material_ids.begin(), material_ids.end());
    ImGui::Text("Material-bound UV draws: %llu  Unique material fingerprints: %llu",
        static_cast<unsigned long long>(g_material_draws_seen.load()),
        static_cast<unsigned long long>(material_count));
    ImGui::Text("Restart-stable draws: %llu  Session-only draws: %llu",
        static_cast<unsigned long long>(g_restart_stable_material_draws.load()),
        static_cast<unsigned long long>(g_session_material_draws.load()));
    ImGui::Text("Persistent texture candidates sampled: %llu",
        static_cast<unsigned long long>(g_material_texture_candidates.load()));
    if (!material_ids.empty()) {
        if (state->selected_binding == 0 ||
            !std::binary_search(material_ids.begin(), material_ids.end(), state->selected_binding)) {
            state->selected_binding = material_ids.front();
            std::lock_guard lock(g_baker_probe_mutex);
            const auto known = g_source_slot_overrides.find(state->selected_binding);
            state->selected_source_slot = known == g_source_slot_overrides.end()
                ? -1 : known->second;
            const auto style = g_binding_style_controls.find(state->selected_binding);
            state->selected_binding_strength = style == g_binding_style_controls.end()
                ? 100 : style->second;
        }
        const auto preview = std::to_string(state->selected_binding);
        if (ImGui::BeginCombo("Binding override", preview.c_str())) {
            for (const auto binding : material_ids) {
                const auto label = std::to_string(binding);
                const bool selected = binding == state->selected_binding;
                if (ImGui::Selectable(label.c_str(), selected)) {
                    state->selected_binding = binding;
                    std::lock_guard lock(g_baker_probe_mutex);
                    const auto known = g_source_slot_overrides.find(binding);
                    state->selected_source_slot = known == g_source_slot_overrides.end()
                        ? -1 : known->second;
                    const auto style = g_binding_style_controls.find(binding);
                    state->selected_binding_strength = style == g_binding_style_controls.end()
                        ? 100 : style->second;
                }
                if (selected) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
        ImGui::SliderInt("Forced source SRV slot (-1 = auto)",
                         &state->selected_source_slot, -1, 127);
        bool persistent_binding = false;
        {
            std::lock_guard lock(g_baker_probe_mutex);
            persistent_binding = g_restart_stable_materials.contains(state->selected_binding);
        }
        if (ImGui::Button("Apply source slot")) {
            {
                std::lock_guard lock(g_baker_probe_mutex);
                if (state->selected_source_slot < 0)
                    g_source_slot_overrides.erase(state->selected_binding);
                else
                    g_source_slot_overrides[state->selected_binding] = state->selected_source_slot;
            }
            if (persistent_binding)
                persist_source_slot(state->selected_binding, state->selected_source_slot);
        }
        ImGui::SameLine();
        if (ImGui::Button("Use automatic source")) {
            {
                std::lock_guard lock(g_baker_probe_mutex);
                g_source_slot_overrides.erase(state->selected_binding);
            }
            state->selected_source_slot = -1;
            if (persistent_binding) persist_source_slot(state->selected_binding, -1);
        }
        ImGui::TextDisabled(persistent_binding ?
            "Override persists for this restart-stable binding." :
            "This binding is session-only; its override cannot safely persist.");
        ImGui::SliderInt("Binding style strength (0 = bypass)",
                         &state->selected_binding_strength, 0, 100);
        if (ImGui::Button("Apply binding style")) {
            {
                std::lock_guard lock(g_baker_probe_mutex);
                if (state->selected_binding_strength >= 100)
                    g_binding_style_controls.erase(state->selected_binding);
                else
                    g_binding_style_controls[state->selected_binding] =
                        static_cast<std::uint8_t>(state->selected_binding_strength);
                ++g_binding_style_revision;
            }
            if (persistent_binding)
                persist_binding_strength(
                    state->selected_binding, state->selected_binding_strength);
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset binding style")) {
            {
                std::lock_guard lock(g_baker_probe_mutex);
                g_binding_style_controls.erase(state->selected_binding);
                ++g_binding_style_revision;
            }
            state->selected_binding_strength = 100;
            if (persistent_binding) persist_binding_strength(state->selected_binding, 100);
        }
        ImGui::TextDisabled("Strength changes switch to an isolated style-cache namespace.");
    }
    std::uint64_t replayed_draws = 0;
    std::uint64_t capture_drops = 0;
    std::uint64_t replacement_draws = 0;
    std::uint64_t rejected_replacements = 0;
    const char *capture_backend = "No";
    {
        std::lock_guard lock(g_baker_probe_mutex);
        if (const auto found = g_surface_captures.find(runtime->get_device());
            found != g_surface_captures.end()) {
            const auto statistics = found->second->statistics();
            replayed_draws = statistics.replayed_draws;
            capture_drops = statistics.dropped_frames;
            replacement_draws = statistics.replacement_draws;
            rejected_replacements = statistics.rejected_replacements;
            switch (found->second->backend()) {
            case neuralpass::capture::GraphicsBackend::d3d9: capture_backend = "D3D9"; break;
            case neuralpass::capture::GraphicsBackend::d3d10: capture_backend = "D3D10"; break;
            case neuralpass::capture::GraphicsBackend::d3d11: capture_backend = "D3D11"; break;
            case neuralpass::capture::GraphicsBackend::d3d12: capture_backend = "D3D12"; break;
            default: capture_backend = "Experimental"; break;
            }
        }
    }
    ImGui::Text("%s UV replay draws: %llu  Readback drops: %llu", capture_backend,
        static_cast<unsigned long long>(replayed_draws),
        static_cast<unsigned long long>(capture_drops));
    ImGui::Text("Replacement draws: %llu  Rejected source layouts: %llu",
        static_cast<unsigned long long>(replacement_draws),
        static_cast<unsigned long long>(rejected_replacements));
    ImGui::Text("Surface frames: %llu  Supported pixels: %llu",
        static_cast<unsigned long long>(g_surface_frames_captured.load()),
        static_cast<unsigned long long>(g_surface_pixels_captured.load()));
    ImGui::Text("D3D9 pre-reset releases: %llu",
        static_cast<unsigned long long>(g_swapchain_resets.load()));
    if (g_uv_draws_seen.load() == 0)
        ImGui::TextWrapped("Waiting for an explicit or location-inferred mesh UV vertex input.");
    else
        ImGui::TextWrapped(
            runtime->get_device()->get_api() == reshade::api::device_api::d3d9
                ? "D3D9 Shader Model 3 draws use legacy replay; depth is unavailable."
            : runtime->get_device()->get_api() == reshade::api::device_api::d3d10
                ? "D3D10 UV-bearing draws use native replay and isolated RGBA replacement."
            : runtime->get_device()->get_api() == reshade::api::device_api::d3d11
                ? "D3D11 UV-bearing draws are replayed into the asynchronous surface capture."
            : runtime->get_device()->get_api() == reshade::api::device_api::d3d12
                ? "D3D12 UV-bearing draws use experimental PSO replay and bounded-table RGBA replacement; native render passes remain fallback-only."
            : runtime->get_device()->get_api() == reshade::api::device_api::vulkan
                ? "Vulkan uses experimental vertex-SPIR-V replay and bounded-table RGBA replacement; native render passes remain fallback-only."
                : "UV-bearing draws detected; this API still needs its replay adapter.");
    ImGui::SeparatorText("Scene identity");
    const auto transition = static_cast<neuralpass::SceneTransition>(
        state->scene_transition.load());
    const char *transition_name = "stable";
    switch (transition) {
    case neuralpass::SceneTransition::camera_cut: transition_name = "camera cut"; break;
    case neuralpass::SceneTransition::pending_scene_change: transition_name = "quarantined"; break;
    case neuralpass::SceneTransition::scene_change: transition_name = "new scene"; break;
    default: break;
    }
    ImGui::Text("Persistent ID: %016llx  Generation: %llu  State: %s",
        static_cast<unsigned long long>(state->active_scene_identity.load()),
        static_cast<unsigned long long>(state->scene_generation.load()),
        transition_name);
    if (ImGui::Button("Keep current scene"))
        state->scene_command = static_cast<int>(ManualSceneCommand::keep_current);
    ImGui::SameLine();
    if (ImGui::Button("Start new scene"))
        state->scene_command = static_cast<int>(ManualSceneCommand::start_new);
    ImGui::SameLine();
    if (ImGui::Button("Merge visible scene"))
        state->scene_command = static_cast<int>(ManualSceneCommand::merge_visible);
    ImGui::TextWrapped("Keep accepts quarantined bindings; Start creates an isolated cache; "
                       "Merge imports the matching visible cache into the active scene.");
    bool adaptive_budget = state->adaptive_tile_budget.load();
    if (ImGui::Checkbox("Adaptive frame-time budget", &adaptive_budget))
        state->adaptive_tile_budget = adaptive_budget;
    int target_fps = static_cast<int>(state->target_frame_rate.load());
    if (ImGui::SliderInt("Target frame rate", &target_fps, 30, 240))
        state->target_frame_rate = static_cast<std::uint32_t>(target_fps);
    int budget = static_cast<int>(state->tile_budget.load());
    if (ImGui::SliderInt("Maximum tiles per worker update", &budget, 1, 16))
        state->tile_budget = static_cast<std::uint32_t>(budget);
    int age = static_cast<int>(state->refresh_age.load());
    if (ImGui::SliderInt("Maximum cache age", &age, 15, 600))
        state->refresh_age = static_cast<std::uint32_t>(age);
    if (ImGui::Button("Reset styled history")) state->reset_requested = true;
    bool sticky_history = state->sticky_history.load();
    if (ImGui::Checkbox("Sticky screen-space history", &sticky_history))
        state->sticky_history = sticky_history;
    int preset = state->requested_preset.load();
    if (ImGui::Combo("Preset", &preset, k_presets.data(), static_cast<int>(k_presets.size())))
        state->requested_preset = preset;
    bool stream_bridge = state->stream_bridge.load();
    if (ImGui::Checkbox("Prompt-driven StreamDiffusion", &stream_bridge)) {
        state->stream_bridge = stream_bridge;
        state->bridge_sent = 0;
        state->bridge_received = 0;
    }
    const char *history_preview = state->prompt_history_selected >= 0 &&
        state->prompt_history_selected < static_cast<int>(state->prompt_history.size())
        ? state->prompt_history[static_cast<std::size_t>(state->prompt_history_selected)].c_str()
        : "Previously used prompts";
    if (ImGui::BeginCombo("Prompt history", history_preview)) {
        for (int index = 0; index < static_cast<int>(state->prompt_history.size()); ++index) {
            const bool selected = index == state->prompt_history_selected;
            if (ImGui::Selectable(state->prompt_history[static_cast<std::size_t>(index)].c_str(), selected)) {
                state->prompt_history_selected = index;
                state->prompt.fill(0);
                const auto &chosen = state->prompt_history[static_cast<std::size_t>(index)];
                std::copy_n(chosen.data(), std::min(chosen.size(), state->prompt.size() - 1),
                            state->prompt.data());
            }
            if (selected) ImGui::SetItemDefaultFocus();
        }
        ImGui::EndCombo();
    }
    if (ImGui::InputTextMultiline("Prompt", state->prompt.data(), state->prompt.size(), ImVec2(-1, 90)))
        state->prompt_history_selected = -1;
    if (ImGui::Button("Apply prompt")) save_prompt(*state);
    ImGui::Text("Live pass: %ux%u; one generated frame in flight", k_stream_width, k_stream_height);
    ImGui::TextWrapped("Keep the NeuralPass technique last in the ReShade order.");
}

} // namespace

extern "C" __declspec(dllexport) const char *NAME = "NeuralPass";
extern "C" __declspec(dllexport) const char *DESCRIPTION =
    "Persistent sparse neural style transfer immediately before presentation.";

extern "C" __declspec(dllexport) bool AddonInit(HMODULE addon_module, HMODULE reshade_module) {
    if (!reshade::register_addon(addon_module, reshade_module)) return false;
    wchar_t path[MAX_PATH] = {};
    GetModuleFileNameW(addon_module, path, MAX_PATH);
    g_addon_directory = std::filesystem::path(path).parent_path();
    g_game_build_identity = game_build_identity();
    descriptor_tracking::register_events();
    reshade::register_event<reshade::addon_event::init_swapchain>(on_init_swapchain);
    reshade::register_event<reshade::addon_event::destroy_swapchain>(on_destroy_swapchain);
    reshade::register_event<reshade::addon_event::present>(on_present);
    reshade::register_event<reshade::addon_event::init_device>(on_baker_init_device);
    reshade::register_event<reshade::addon_event::destroy_device>(on_baker_destroy_device);
    reshade::register_event<reshade::addon_event::init_pipeline>(on_baker_init_pipeline);
    reshade::register_event<reshade::addon_event::destroy_pipeline>(on_baker_destroy_pipeline);
    reshade::register_event<reshade::addon_event::init_resource>(on_baker_init_resource);
    reshade::register_event<reshade::addon_event::destroy_resource>(on_baker_destroy_resource);
    reshade::register_event<reshade::addon_event::barrier>(on_baker_barrier);
    reshade::register_event<reshade::addon_event::update_buffer_region>(on_baker_update_buffer);
    reshade::register_event<reshade::addon_event::update_buffer_region_command>(on_baker_update_buffer_command);
    reshade::register_event<reshade::addon_event::update_texture_region>(on_baker_update_texture);
    reshade::register_event<reshade::addon_event::init_command_list>(on_baker_init_command_list);
    reshade::register_event<reshade::addon_event::destroy_command_list>(on_baker_destroy_command_list);
    reshade::register_event<reshade::addon_event::bind_pipeline>(on_baker_bind_pipeline);
    reshade::register_event<reshade::addon_event::bind_pipeline_states>(on_baker_bind_pipeline_states);
    reshade::register_event<reshade::addon_event::bind_vertex_buffers>(on_baker_bind_vertex_buffers);
    reshade::register_event<reshade::addon_event::bind_index_buffer>(on_baker_bind_index_buffer);
    reshade::register_event<reshade::addon_event::bind_render_targets_and_depth_stencil>(on_baker_bind_targets);
    reshade::register_event<reshade::addon_event::begin_render_pass>(on_baker_begin_render_pass);
    reshade::register_event<reshade::addon_event::end_render_pass>(on_baker_end_render_pass);
    reshade::register_event<reshade::addon_event::push_descriptors>(on_baker_push_descriptors);
    reshade::register_event<reshade::addon_event::bind_descriptor_tables>(on_baker_bind_descriptor_tables);
    reshade::register_event<reshade::addon_event::draw>(on_baker_draw);
    reshade::register_event<reshade::addon_event::draw_indexed>(on_baker_draw_indexed);
    reshade::register_event<reshade::addon_event::draw_or_dispatch_indirect>(on_baker_draw_indirect);
    reshade::register_event<reshade::addon_event::init_effect_runtime>(on_init);
    reshade::register_event<reshade::addon_event::destroy_effect_runtime>(on_destroy);
    reshade::register_event<reshade::addon_event::reshade_reloaded_effects>(find_effect_variables);
    reshade::register_event<reshade::addon_event::reshade_begin_effects>(on_begin_effects);
    reshade::register_overlay("NeuralPass", draw_overlay);
    return true;
}

extern "C" __declspec(dllexport) void AddonUninit(HMODULE addon_module, HMODULE reshade_module) {
    descriptor_tracking::unregister_events();
    reshade::unregister_addon(addon_module, reshade_module);
}
