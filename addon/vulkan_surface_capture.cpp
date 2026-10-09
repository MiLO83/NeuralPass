#include "vulkan_surface_capture.hpp"
#include "vulkan_capture_combined_spv.hpp"
#include "vulkan_capture_shader_spv.hpp"
#include "vulkan_spirv.hpp"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>

namespace neuralpass::vulkan_capture {
namespace {

struct ShaderBlob {
    std::vector<std::uint8_t> code;
    std::string entry;
    std::vector<std::uint32_t> specialization_ids;
    std::vector<std::uint32_t> specialization_values;
    [[nodiscard]] reshade::api::shader_desc desc() const {
        return {code.data(), code.size(), entry.empty() ? nullptr : entry.c_str(),
                static_cast<std::uint32_t>(specialization_ids.size()),
                specialization_ids.data(), specialization_values.data()};
    }
};

struct PipelineTemplate {
    reshade::api::pipeline_layout layout = {};
    ShaderBlob vertex, hull, domain, geometry;
    std::vector<reshade::api::input_element> input;
    std::vector<std::string> semantics;
    reshade::api::blend_desc blend {};
    reshade::api::rasterizer_desc rasterizer {};
    reshade::api::depth_stencil_desc depth_stencil {};
    reshade::api::primitive_topology topology = reshade::api::primitive_topology::undefined;
    reshade::api::format depth_format = reshade::api::format::unknown;
    std::uint32_t sample_mask = UINT32_MAX;
    std::uint32_t sample_count = 1;
    std::uint32_t viewport_count = 1;
    std::vector<reshade::api::dynamic_state> dynamic_states;
    bool has_vertex = false;
};

struct PipelineKey {
    std::uint64_t pipeline = 0;
    std::uint64_t material = 0;
    std::uint32_t uv_location = 0;
    std::uint32_t source_set = 0;
    std::uint32_t source_binding = 0;
    bool samples_source = false;
    bool operator==(const PipelineKey &) const = default;
};
struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey &key) const noexcept {
        auto value = std::hash<std::uint64_t> {}(key.pipeline);
        value ^= std::hash<std::uint64_t> {}(key.material) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.uv_location) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.source_set) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.source_binding) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<bool> {}(key.samples_source) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        return value;
    }
};

struct ReplacementTexture {
    std::vector<capture::ReplacementMip> mips;
    reshade::api::resource source = {};
    reshade::api::resource texture = {};
    reshade::api::resource upload = {};
    reshade::api::resource rejected_source = {};
    reshade::api::resource_view view = {};
    reshade::api::descriptor_table shadow_table = {};
    reshade::api::descriptor_table shadow_source_table = {};
    reshade::api::pipeline_layout shadow_layout = {};
    std::uint32_t shadow_param = 0;
    bool dirty = true;
    bool ready = false;
};

bool byte_color_format(reshade::api::format format) {
    return format == reshade::api::format::r8g8b8a8_unorm ||
        format == reshade::api::format::r8g8b8a8_unorm_srgb ||
        format == reshade::api::format::b8g8r8a8_unorm ||
        format == reshade::api::format::b8g8r8a8_unorm_srgb;
}

bool bgra_format(reshade::api::format format) {
    return format == reshade::api::format::b8g8r8a8_unorm ||
        format == reshade::api::format::b8g8r8a8_unorm_srgb;
}

void store_shader(ShaderBlob &destination, const reshade::api::shader_desc &source) {
    if (source.code == nullptr || source.code_size == 0) return;
    const auto *bytes = static_cast<const std::uint8_t *>(source.code);
    destination.code.assign(bytes, bytes + source.code_size);
    destination.entry = source.entry_point != nullptr ? source.entry_point : "";
    if (source.spec_constants != 0 && source.spec_constant_ids != nullptr &&
        source.spec_constant_values != nullptr) {
        destination.specialization_ids.assign(
            source.spec_constant_ids,
            source.spec_constant_ids + source.spec_constants);
        destination.specialization_values.assign(
            source.spec_constant_values,
            source.spec_constant_values + source.spec_constants);
    }
}

std::vector<std::uint32_t> capture_spirv(
    std::uint32_t uv_location, const capture::DrawCommand &draw,
    bool samples_source) {
    const auto *bytes = samples_source
        ? neuralpass_vulkan_capture_combined_spv
        : neuralpass_vulkan_capture_spv;
    const auto byte_count = samples_source
        ? sizeof(neuralpass_vulkan_capture_combined_spv)
        : sizeof(neuralpass_vulkan_capture_spv);
    if (byte_count % sizeof(std::uint32_t) != 0) return {};
    std::vector<std::uint32_t> words(byte_count / sizeof(std::uint32_t));
    std::memcpy(words.data(), bytes, byte_count);
    words = spirv::patch_unique_location(words, 31, uv_location);
    if (words.empty() || !samples_source) return words;
    constexpr std::uint32_t k_binding_decoration = 33;
    constexpr std::uint32_t k_descriptor_set_decoration = 34;
    words = spirv::patch_unique_decoration(
        words, k_descriptor_set_decoration, 31, draw.source_descriptor_param);
    if (words.empty()) return {};
    return spirv::patch_unique_decoration(
        words, k_binding_decoration, 31, draw.source_descriptor_binding);
}

} // namespace

struct SurfaceCapture::Impl {
    mutable std::recursive_mutex mutex;
    reshade::api::device *device = nullptr;
    reshade::api::command_queue *queue = nullptr;
    std::unordered_map<std::uint64_t, PipelineTemplate> pipelines;
    std::unordered_map<PipelineKey, reshade::api::pipeline, PipelineKeyHash> variants;
    std::unordered_map<std::uint64_t, ReplacementTexture> replacements;
    std::array<reshade::api::resource, 4> targets {};
    std::array<reshade::api::resource_view, 4> views {};
    struct ReadbackSlot {
        reshade::api::resource buffer = {};
        std::uint64_t fence_value = 0;
        std::uint64_t frame_index = 0;
        bool pending = false;
        bool ready_without_fence = false;
    };
    std::array<ReadbackSlot, 3> readback {};
    reshade::api::fence completion_fence = {};
    std::array<std::uint64_t, 4> offsets {};
    std::uint32_t width = 0, height = 0;
    std::uint64_t frame = 1;
    std::uint64_t next_fence_value = 1;
    std::size_t submit_cursor = 0;
    capture::CaptureStatistics stats;
    bool cleared = false;

    ~Impl() { release_all(); }

    void release_replacement(ReplacementTexture &entry) {
        if (device != nullptr) {
            if (entry.shadow_table != 0)
                device->free_descriptor_table(entry.shadow_table);
            if (entry.view != 0) device->destroy_resource_view(entry.view);
            if (entry.upload != 0) device->destroy_resource(entry.upload);
            if (entry.texture != 0) device->destroy_resource(entry.texture);
        }
        entry = {};
    }

    void release_replacements() {
        for (auto &[material, entry] : replacements) {
            (void)material;
            release_replacement(entry);
        }
        replacements.clear();
    }

    void release_surfaces() {
        if (device != nullptr) {
            for (auto &view : views) if (view != 0) device->destroy_resource_view(view);
            for (auto &target : targets) if (target != 0) device->destroy_resource(target);
            for (auto &slot : readback)
                if (slot.buffer != 0) device->destroy_resource(slot.buffer);
            if (completion_fence != 0) device->destroy_fence(completion_fence);
        }
        views = {}; targets = {}; readback = {}; completion_fence = {}; offsets = {};
        width = height = 0; cleared = false;
        next_fence_value = 1; submit_cursor = 0;
    }
    void release_all() {
        if (queue != nullptr) queue->wait_idle();
        release_surfaces();
        if (device != nullptr)
            for (const auto &[key, pipeline] : variants) {
                (void)key; device->destroy_pipeline(pipeline);
            }
        variants.clear(); pipelines.clear();
        release_replacements();
    }

    bool create_surfaces(std::uint32_t new_width, std::uint32_t new_height) {
        if (queue != nullptr) queue->wait_idle();
        release_surfaces();
        if (device == nullptr || new_width == 0 || new_height == 0 ||
            !device->check_capability(reshade::api::device_caps::copy_buffer_to_texture) ||
            !device->check_capability(
                reshade::api::device_caps::bind_render_targets_and_depth_stencil))
            return false;
        constexpr std::array<reshade::api::format, 4> formats {
            reshade::api::format::r32g32b32a32_uint,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32_float};
        for (std::size_t index = 0; index < targets.size(); ++index) {
            reshade::api::resource_desc desc(new_width, new_height, 1, 1,
                formats[index], 1, reshade::api::memory_heap::default_,
                reshade::api::resource_usage::render_target |
                    reshade::api::resource_usage::copy_source);
            if (!device->create_resource(desc, nullptr,
                    reshade::api::resource_usage::render_target, &targets[index]) ||
                !device->create_resource_view(targets[index],
                    reshade::api::resource_usage::render_target,
                    reshade::api::resource_view_desc(formats[index]), &views[index])) {
                release_surfaces(); return false;
            }
        }
        offsets[0] = 0;
        offsets[1] = static_cast<std::uint64_t>(new_width) * new_height * 16;
        offsets[2] = offsets[1] * 2;
        offsets[3] = offsets[2] + offsets[1];
        const auto total = offsets[3] + static_cast<std::uint64_t>(new_width) * new_height * 4;
        reshade::api::resource_desc buffer(total, reshade::api::memory_heap::readback,
                                            reshade::api::resource_usage::copy_dest);
        if (!device->create_fence(0, reshade::api::fence_flags::none,
                                  &completion_fence)) {
            release_surfaces(); return false;
        }
        for (auto &slot : readback)
            if (!device->create_resource(buffer, nullptr,
                    reshade::api::resource_usage::copy_dest, &slot.buffer)) {
                release_surfaces(); return false;
            }
        width = new_width; height = new_height;
        return true;
    }

    std::optional<SurfaceCaptureFrame> take_completed_frame() {
        if (device == nullptr || completion_fence == 0) return std::nullopt;
        const auto completed = device->get_completed_fence_value(completion_fence);
        std::size_t selected = readback.size();
        for (std::size_t index = 0; index < readback.size(); ++index) {
            const auto &slot = readback[index];
            if (!slot.pending ||
                (!slot.ready_without_fence && slot.fence_value > completed))
                continue;
            if (selected == readback.size() ||
                slot.frame_index < readback[selected].frame_index)
                selected = index;
        }
        if (selected == readback.size()) return std::nullopt;

        auto &slot = readback[selected];
        void *mapped = nullptr;
        if (!device->map_buffer_region(slot.buffer, 0, UINT64_MAX,
                reshade::api::map_access::read_only, &mapped))
            return std::nullopt;
        SurfaceCaptureFrame result(width, height, slot.frame_index);
        const auto *bytes = static_cast<const std::uint8_t *>(mapped);
        for (std::uint32_t y = 0; y < height; ++y)
            for (std::uint32_t x = 0; x < width; ++x) {
                const auto pixel_index = static_cast<std::size_t>(y) * width + x;
                const auto *id = reinterpret_cast<const std::uint32_t *>(
                    bytes + offsets[0]) + pixel_index * 4;
                const auto *gradient = reinterpret_cast<const float *>(
                    bytes + offsets[1]) + pixel_index * 4;
                const auto *source = reinterpret_cast<const float *>(
                    bytes + offsets[2]) + pixel_index * 4;
                const auto *depth = reinterpret_cast<const float *>(
                    bytes + offsets[3]) + pixel_index;
                auto &pixel = result.pixels().at(x, y);
                pixel.material_id = static_cast<std::uint64_t>(id[0]) |
                    (static_cast<std::uint64_t>(id[1]) << 32);
                std::memcpy(&pixel.u, id + 2, sizeof(float));
                std::memcpy(&pixel.v, id + 3, sizeof(float));
                pixel.du_dx = gradient[0]; pixel.du_dy = gradient[1];
                pixel.dv_dx = gradient[2]; pixel.dv_dy = gradient[3];
                pixel.source_r = source[0]; pixel.source_g = source[1];
                pixel.source_b = source[2]; pixel.source_a = source[3];
                pixel.framebuffer_depth = *depth; pixel.hit_depth = *depth;
                pixel.confidence = pixel.material_id == 0 ? 0.0f : 1.0f;
            }
        device->unmap_buffer_region(slot.buffer);
        slot = {};
        return result;
    }

    reshade::api::pipeline capture_pipeline(std::uint64_t source_pipeline,
                                            std::uint64_t material,
                                            std::uint32_t location,
                                            const capture::DrawCommand &draw) {
        const bool wants_source = draw.source_sampleable &&
            draw.source_descriptor_array_offset == 0 &&
            static_cast<reshade::api::descriptor_type>(draw.source_descriptor_type) ==
                reshade::api::descriptor_type::sampler_with_resource_view;
        const PipelineKey key {source_pipeline, material, location,
            draw.source_descriptor_param, draw.source_descriptor_binding, wants_source};
        if (const auto found = variants.find(key); found != variants.end()) return found->second;
        const auto known = pipelines.find(source_pipeline);
        if (known == pipelines.end() || device == nullptr || !known->second.has_vertex) return {};
        const auto &source = known->second;
        // A UV output added to the vertex stage would not automatically flow
        // through tessellation or geometry stages. Reject those pipelines until
        // the final pre-raster stage can be instrumented too.
        if (!source.hull.code.empty() || !source.domain.code.empty() ||
            !source.geometry.code.empty())
            return {};
        if (source.vertex.code.size() % sizeof(std::uint32_t) != 0) return {};
        std::vector<std::uint32_t> vertex_words(
            source.vertex.code.size() / sizeof(std::uint32_t));
        std::memcpy(vertex_words.data(), source.vertex.code.data(),
                    source.vertex.code.size());
        auto instrumented_vertex = spirv::instrument_vertex_uv(
            vertex_words, location, source.vertex.entry);
        if (!instrumented_vertex) return {};
        auto spirv = capture_spirv(
            instrumented_vertex.varying_location, draw, wants_source);
        if (spirv.empty()) return {};
        const std::array<std::uint32_t, 2> ids {0, 1};
        const std::array<std::uint32_t, 2> values {
            static_cast<std::uint32_t>(material), static_cast<std::uint32_t>(material >> 32)};
        auto vertex = source.vertex.desc();
        vertex.code = instrumented_vertex.words.data();
        vertex.code_size = instrumented_vertex.words.size() * sizeof(std::uint32_t);
        auto hull = source.hull.desc(); auto domain = source.domain.desc();
        auto geometry = source.geometry.desc();
        reshade::api::shader_desc pixel {spirv.data(), spirv.size() * sizeof(std::uint32_t),
                                         "main", 2, ids.data(), values.data()};
        auto blend = source.blend;
        for (std::size_t index = 0; index < 8; ++index) {
            blend.blend_enable[index] = false;
            blend.logic_op_enable[index] = false;
            blend.render_target_write_mask[index] = index < 4 ? 0xF : 0;
        }
        auto depth = source.depth_stencil;
        depth.depth_write_mask = false;
        if (depth.depth_enable) depth.depth_func = reshade::api::compare_op::equal;
        std::array<reshade::api::format, 4> formats {
            reshade::api::format::r32g32b32a32_uint,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32_float};
        const std::uint32_t samples = 1;
        std::vector<reshade::api::pipeline_subobject> objects;
        objects.push_back({reshade::api::pipeline_subobject_type::vertex_shader, 1, &vertex});
        if (!source.hull.code.empty()) objects.push_back({reshade::api::pipeline_subobject_type::hull_shader, 1, &hull});
        if (!source.domain.code.empty()) objects.push_back({reshade::api::pipeline_subobject_type::domain_shader, 1, &domain});
        if (!source.geometry.code.empty()) objects.push_back({reshade::api::pipeline_subobject_type::geometry_shader, 1, &geometry});
        objects.push_back({reshade::api::pipeline_subobject_type::pixel_shader, 1, &pixel});
        if (!source.input.empty()) objects.push_back({reshade::api::pipeline_subobject_type::input_layout,
            static_cast<std::uint32_t>(source.input.size()), const_cast<reshade::api::input_element *>(source.input.data())});
        objects.push_back({reshade::api::pipeline_subobject_type::blend_state, 1, &blend});
        objects.push_back({reshade::api::pipeline_subobject_type::rasterizer_state, 1, const_cast<reshade::api::rasterizer_desc *>(&source.rasterizer)});
        objects.push_back({reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depth});
        objects.push_back({reshade::api::pipeline_subobject_type::primitive_topology, 1, const_cast<reshade::api::primitive_topology *>(&source.topology)});
        objects.push_back({reshade::api::pipeline_subobject_type::depth_stencil_format, 1, const_cast<reshade::api::format *>(&source.depth_format)});
        objects.push_back({reshade::api::pipeline_subobject_type::render_target_formats, 4, formats.data()});
        objects.push_back({reshade::api::pipeline_subobject_type::sample_mask, 1, const_cast<std::uint32_t *>(&source.sample_mask)});
        objects.push_back({reshade::api::pipeline_subobject_type::sample_count, 1, const_cast<std::uint32_t *>(&samples)});
        objects.push_back({reshade::api::pipeline_subobject_type::viewport_count, 1,
            const_cast<std::uint32_t *>(&source.viewport_count)});
        if (!source.dynamic_states.empty()) objects.push_back({reshade::api::pipeline_subobject_type::dynamic_pipeline_states,
            static_cast<std::uint32_t>(source.dynamic_states.size()), const_cast<reshade::api::dynamic_state *>(source.dynamic_states.data())});
        reshade::api::pipeline result = {};
        if (!device->create_pipeline(source.layout, static_cast<std::uint32_t>(objects.size()),
                                     objects.data(), &result)) return {};
        variants.emplace(key, result);
        return result;
    }

    reshade::api::descriptor_table replacement_table(
        reshade::api::command_list *commands, std::uint64_t material,
        const capture::DrawCommand &draw) {
        const auto found = replacements.find(material);
        if (found == replacements.end() || found->second.mips.empty() ||
            device == nullptr || queue == nullptr || commands == nullptr ||
            draw.source_resource == 0 || draw.source_view == 0 ||
            !draw.source_descriptor_isolatable || draw.source_descriptor_table == 0 ||
            draw.pipeline_layout == 0)
            return {};
        const auto descriptor_type =
            static_cast<reshade::api::descriptor_type>(draw.source_descriptor_type);
        if (descriptor_type != reshade::api::descriptor_type::shader_resource_view &&
            descriptor_type != reshade::api::descriptor_type::sampler_with_resource_view)
            return {};
        if (descriptor_type == reshade::api::descriptor_type::sampler_with_resource_view &&
            draw.source_sampler == 0)
            return {};

        auto &entry = found->second;
        const reshade::api::resource source {draw.source_resource};
        if (entry.rejected_source == source) return {};
        const reshade::api::resource_view source_view {draw.source_view};
        const auto desc = device->get_resource_desc(source);
        const auto view_desc = device->get_resource_view_desc(source_view);
        const bool valid = desc.type == reshade::api::resource_type::texture_2d &&
            desc.texture.depth_or_layers == 1 && desc.texture.levels != 0 &&
            desc.texture.samples == 1 &&
            view_desc.type == reshade::api::resource_view_type::texture_2d &&
            view_desc.texture.first_layer == 0 &&
            view_desc.texture.first_level < desc.texture.levels &&
            byte_color_format(view_desc.format) &&
            (desc.usage & reshade::api::resource_usage::copy_source) !=
                reshade::api::resource_usage::undefined;
        auto reset_gpu = [&]() {
            auto mips = std::move(entry.mips);
            release_replacement(entry);
            entry.mips = std::move(mips);
        };
        auto reject = [&]() -> reshade::api::descriptor_table {
            entry.rejected_source = source;
            entry.ready = false;
            ++stats.rejected_replacements;
            return {};
        };
        if (!valid) return reject();

        if (entry.source != source) {
            queue->wait_idle();
            reset_gpu();
            auto replacement_desc = desc;
            replacement_desc.heap = reshade::api::memory_heap::default_;
            replacement_desc.usage = reshade::api::resource_usage::shader_resource |
                reshade::api::resource_usage::copy_dest;
            replacement_desc.flags = reshade::api::resource_flags::none;
            if (!device->create_resource(replacement_desc, nullptr,
                    reshade::api::resource_usage::copy_dest, &entry.texture) ||
                !device->create_resource_view(entry.texture,
                    reshade::api::resource_usage::shader_resource,
                    view_desc, &entry.view))
                return reject();

            std::uint64_t upload_size = 0;
            for (std::uint32_t level = 0; level < desc.texture.levels; ++level) {
                const auto mip_width = std::max(1u, desc.texture.width >> level);
                const auto mip_height = std::max(1u, desc.texture.height >> level);
                upload_size += static_cast<std::uint64_t>(mip_width) * mip_height * 4;
            }
            const reshade::api::resource_desc upload_desc(
                upload_size, reshade::api::memory_heap::upload,
                reshade::api::resource_usage::copy_source);
            if (upload_size == 0 || !device->create_resource(upload_desc, nullptr,
                    reshade::api::resource_usage::copy_source, &entry.upload))
                return reject();
            entry.source = source;
            entry.dirty = true;
            entry.ready = false;
        }

        if (entry.dirty) {
            if (entry.ready) queue->wait_idle();
            const auto old_usage =
                static_cast<reshade::api::resource_usage>(draw.source_usage);
            if ((old_usage & reshade::api::resource_usage::shader_resource) ==
                reshade::api::resource_usage::undefined)
                return reject();
            if (entry.ready)
                commands->barrier(entry.texture,
                    reshade::api::resource_usage::shader_resource_pixel,
                    reshade::api::resource_usage::copy_dest);
            commands->barrier(source, old_usage,
                              reshade::api::resource_usage::copy_source);
            commands->copy_resource(source, entry.texture);
            commands->barrier(source, reshade::api::resource_usage::copy_source,
                              old_usage);

            std::vector<std::uint64_t> mip_offsets(desc.texture.levels);
            std::uint64_t upload_size = 0;
            for (std::uint32_t level = 0; level < desc.texture.levels; ++level) {
                mip_offsets[level] = upload_size;
                upload_size += static_cast<std::uint64_t>(
                    std::max(1u, desc.texture.width >> level)) *
                    std::max(1u, desc.texture.height >> level) * 4;
            }
            void *mapped_data = nullptr;
            if (!device->map_buffer_region(entry.upload, 0, upload_size,
                    reshade::api::map_access::write_only, &mapped_data))
                return reject();
            auto *mapped = static_cast<std::uint8_t *>(mapped_data);
            const bool bgra = bgra_format(view_desc.format);
            const auto first_level = view_desc.texture.first_level;
            const auto view_levels = view_desc.texture.levels == UINT32_MAX
                ? desc.texture.levels - first_level : view_desc.texture.levels;
            const auto levels = std::min<std::size_t>(
                std::min<std::uint32_t>(view_levels, desc.texture.levels - first_level),
                entry.mips.size());
            for (std::size_t level = 0; level < levels; ++level) {
                const auto &mip = entry.mips[level];
                if (mip.width == 0 || mip.height == 0 ||
                    mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4 ||
                    mip.coverage.size() != static_cast<std::size_t>(mip.width) * mip.height)
                    continue;
                const auto subresource = first_level + static_cast<std::uint32_t>(level);
                const auto mip_width = std::max(1u, desc.texture.width >> subresource);
                const auto mip_height = std::max(1u, desc.texture.height >> subresource);
                for (std::uint32_t y = 0; y < mip_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / mip_height);
                    for (std::uint32_t x = 0; x < mip_width; ++x) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / mip_width);
                        const auto *rgba = mip.rgba.data() +
                            (static_cast<std::size_t>(ay) * mip.width + ax) * 4;
                        auto *destination = mapped + mip_offsets[subresource] +
                            (static_cast<std::uint64_t>(y) * mip_width + x) * 4;
                        destination[0] = rgba[bgra ? 2 : 0];
                        destination[1] = rgba[1];
                        destination[2] = rgba[bgra ? 0 : 2];
                        destination[3] = rgba[3];
                    }
                }
            }
            device->unmap_buffer_region(entry.upload);

            for (std::size_t level = 0; level < levels; ++level) {
                const auto &mip = entry.mips[level];
                if (mip.width == 0 || mip.height == 0 ||
                    mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4 ||
                    mip.coverage.size() != static_cast<std::size_t>(mip.width) * mip.height)
                    continue;
                const auto subresource = first_level + static_cast<std::uint32_t>(level);
                const auto mip_width = std::max(1u, desc.texture.width >> subresource);
                const auto mip_height = std::max(1u, desc.texture.height >> subresource);
                for (std::uint32_t y = 0; y < mip_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / mip_height);
                    std::uint32_t x = 0;
                    while (x < mip_width) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / mip_width);
                        if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + ax] == 0) {
                            ++x;
                            continue;
                        }
                        const auto begin = x++;
                        while (x < mip_width) {
                            const auto sample_x = std::min(
                                mip.width - 1, x * mip.width / mip_width);
                            if (mip.coverage[static_cast<std::size_t>(ay) * mip.width +
                                             sample_x] == 0)
                                break;
                            ++x;
                        }
                        const reshade::api::subresource_box box {
                            begin, y, 0, x, y + 1, 1};
                        const auto source_offset = mip_offsets[subresource] +
                            (static_cast<std::uint64_t>(y) * mip_width + begin) * 4;
                        commands->copy_buffer_to_texture(entry.upload, source_offset,
                            0, 0, entry.texture, subresource, &box);
                    }
                }
            }
            commands->barrier(entry.texture, reshade::api::resource_usage::copy_dest,
                              reshade::api::resource_usage::shader_resource_pixel);
            entry.dirty = false;
            entry.ready = true;
        }
        if (!entry.ready) return {};

        const reshade::api::pipeline_layout layout {draw.pipeline_layout};
        const reshade::api::descriptor_table original {draw.source_descriptor_table};
        bool refresh_shadow = entry.shadow_source_table != original;
        if (entry.shadow_table == 0 || entry.shadow_layout != layout ||
            entry.shadow_param != draw.source_descriptor_param) {
            if (entry.shadow_table != 0)
                device->free_descriptor_table(entry.shadow_table);
            entry.shadow_table = {};
            if (!device->allocate_descriptor_table(
                    layout, draw.source_descriptor_param, &entry.shadow_table)) {
                ++stats.rejected_replacements;
                return {};
            }
            entry.shadow_layout = layout;
            entry.shadow_param = draw.source_descriptor_param;
            refresh_shadow = true;
        }
        if (!refresh_shadow) return entry.shadow_table;
        std::array<reshade::api::descriptor_table_copy, 32> copies {};
        for (std::uint32_t index = 0; index < draw.source_table_range_count; ++index) {
            const auto &range = draw.source_table_ranges[index];
            copies[index] = {original, range.binding, 0, entry.shadow_table,
                             range.binding, 0, range.count};
        }
        device->copy_descriptor_tables(draw.source_table_range_count, copies.data());
        reshade::api::descriptor_table_update update {};
        update.table = entry.shadow_table;
        update.binding = draw.source_descriptor_binding;
        update.array_offset = draw.source_descriptor_array_offset;
        update.count = 1;
        update.type = descriptor_type;
        reshade::api::sampler_with_resource_view combined {};
        if (descriptor_type == reshade::api::descriptor_type::sampler_with_resource_view) {
            combined = {reshade::api::sampler {draw.source_sampler}, entry.view};
            update.descriptors = &combined;
        } else {
            update.descriptors = &entry.view;
        }
        device->update_descriptor_tables(1, &update);
        entry.shadow_source_table = original;
        return entry.shadow_table;
    }

    bool execute(reshade::api::command_list *commands, const capture::DrawCommand &draw) {
        switch (draw.kind) {
        case capture::DrawKind::direct:
            commands->draw(draw.vertex_or_index_count, draw.instance_count,
                           draw.first_vertex_or_index, draw.first_instance); return true;
        case capture::DrawKind::indexed:
            commands->draw_indexed(draw.vertex_or_index_count, draw.instance_count,
                draw.first_vertex_or_index, draw.vertex_offset, draw.first_instance); return true;
        case capture::DrawKind::indirect:
        case capture::DrawKind::indexed_indirect:
            if (draw.argument_buffer == nullptr) return false;
            commands->draw_or_dispatch_indirect(
                draw.kind == capture::DrawKind::indirect
                    ? reshade::api::indirect_command::draw
                    : reshade::api::indirect_command::draw_indexed,
                reshade::api::resource {reinterpret_cast<std::uint64_t>(draw.argument_buffer)},
                draw.argument_offset, draw.draw_count, draw.argument_stride); return true;
        }
        return false;
    }
};

SurfaceCapture::SurfaceCapture() : impl_(new Impl) {}
SurfaceCapture::~SurfaceCapture() { reset(); }
void SurfaceCapture::attach(reshade::api::device *device, reshade::api::command_queue *queue) {
    std::lock_guard lock(impl_->mutex); impl_->device = device; impl_->queue = queue;
}
void SurfaceCapture::register_pipeline(reshade::api::pipeline_layout layout,
    std::uint32_t count, const reshade::api::pipeline_subobject *objects,
    reshade::api::pipeline pipeline) {
    if (impl_ == nullptr || objects == nullptr || pipeline == 0) return;
    std::lock_guard lock(impl_->mutex);
    PipelineTemplate result; result.layout = layout;
    for (std::uint32_t index = 0; index < count; ++index) {
        const auto &object = objects[index];
        switch (object.type) {
        case reshade::api::pipeline_subobject_type::vertex_shader: if (object.count) { store_shader(result.vertex, *static_cast<const reshade::api::shader_desc *>(object.data)); result.has_vertex = true; } break;
        case reshade::api::pipeline_subobject_type::hull_shader: if (object.count) store_shader(result.hull, *static_cast<const reshade::api::shader_desc *>(object.data)); break;
        case reshade::api::pipeline_subobject_type::domain_shader: if (object.count) store_shader(result.domain, *static_cast<const reshade::api::shader_desc *>(object.data)); break;
        case reshade::api::pipeline_subobject_type::geometry_shader: if (object.count) store_shader(result.geometry, *static_cast<const reshade::api::shader_desc *>(object.data)); break;
        case reshade::api::pipeline_subobject_type::input_layout: {
            const auto *input = static_cast<const reshade::api::input_element *>(object.data);
            if (object.count) result.input.assign(input, input + object.count);
            result.semantics.reserve(object.count);
            for (std::uint32_t element = 0; element < object.count; ++element)
                result.semantics.emplace_back(input[element].semantic != nullptr ? input[element].semantic : "");
            for (std::uint32_t element = 0; element < object.count; ++element)
                result.input[element].semantic = result.semantics[element].empty() ? nullptr : result.semantics[element].c_str();
            break;
        }
        case reshade::api::pipeline_subobject_type::blend_state: result.blend = *static_cast<const reshade::api::blend_desc *>(object.data); break;
        case reshade::api::pipeline_subobject_type::rasterizer_state: result.rasterizer = *static_cast<const reshade::api::rasterizer_desc *>(object.data); break;
        case reshade::api::pipeline_subobject_type::depth_stencil_state: result.depth_stencil = *static_cast<const reshade::api::depth_stencil_desc *>(object.data); break;
        case reshade::api::pipeline_subobject_type::primitive_topology: result.topology = *static_cast<const reshade::api::primitive_topology *>(object.data); break;
        case reshade::api::pipeline_subobject_type::depth_stencil_format: result.depth_format = *static_cast<const reshade::api::format *>(object.data); break;
        case reshade::api::pipeline_subobject_type::sample_mask: result.sample_mask = *static_cast<const std::uint32_t *>(object.data); break;
        case reshade::api::pipeline_subobject_type::sample_count: result.sample_count = *static_cast<const std::uint32_t *>(object.data); break;
        case reshade::api::pipeline_subobject_type::viewport_count: result.viewport_count = *static_cast<const std::uint32_t *>(object.data); break;
        case reshade::api::pipeline_subobject_type::dynamic_pipeline_states: if (object.count) { const auto *states = static_cast<const reshade::api::dynamic_state *>(object.data); result.dynamic_states.assign(states, states + object.count); } break;
        default: break;
        }
    }
    impl_->pipelines.insert_or_assign(pipeline.handle, std::move(result));
}
void SurfaceCapture::unregister_pipeline(reshade::api::pipeline pipeline) {
    if (impl_ == nullptr) return;
    std::lock_guard lock(impl_->mutex);
    impl_->pipelines.erase(pipeline.handle);
    for (auto it = impl_->variants.begin(); it != impl_->variants.end();) {
        if (it->first.pipeline != pipeline.handle) { ++it; continue; }
        if (impl_->device != nullptr) impl_->device->destroy_pipeline(it->second);
        it = impl_->variants.erase(it);
    }
}
capture::GraphicsBackend SurfaceCapture::backend() const noexcept { return capture::GraphicsBackend::vulkan; }
capture::CaptureCapabilities SurfaceCapture::capabilities() const noexcept {
    return {.direct_draws=true, .indexed_draws=true, .indirect_draws=true,
            .replacement_textures=true, .asynchronous_readback=true,
            .shader_coverage_preserved=false};
}
bool SurfaceCapture::initialize(void *, std::uint32_t width, std::uint32_t height) {
    if (impl_ == nullptr) impl_ = new Impl;
    std::lock_guard lock(impl_->mutex);
    return impl_->create_surfaces(width, height);
}
void SurfaceCapture::reset() { delete impl_; impl_ = nullptr; }
bool SurfaceCapture::replay(void *command_list, const capture::UvInput &uv,
    std::uint64_t material, const capture::DrawCommand &draw, int) {
    if (impl_ == nullptr || command_list == nullptr || material == 0 ||
        uv.location == UINT32_MAX || !draw.target_compatible || draw.inside_render_pass ||
        draw.pipeline == 0 || draw.render_target_count == 0) return false;
    std::lock_guard lock(impl_->mutex);
    auto *commands = static_cast<reshade::api::command_list *>(command_list);
    const auto companion = impl_->capture_pipeline(
        draw.pipeline, material, uv.location, draw);
    if (companion == 0) return false;
    if (!impl_->cleared) {
        const float zero[4] {};
        const float missing[4] {NAN, NAN, NAN, NAN};
        commands->clear_render_target_view(impl_->views[0], zero);
        commands->clear_render_target_view(impl_->views[1], zero);
        commands->clear_render_target_view(impl_->views[2], missing);
        commands->clear_render_target_view(impl_->views[3], missing);
        impl_->cleared = true;
    }
    const auto replacement = impl_->replacement_table(commands, material, draw);
    const reshade::api::pipeline_layout layout {draw.pipeline_layout};
    const reshade::api::descriptor_table original {draw.source_descriptor_table};
    if (replacement != 0)
        commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
            layout, draw.source_descriptor_param, replacement);
    if (!impl_->execute(commands, draw)) {
        if (replacement != 0)
            commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
                layout, draw.source_descriptor_param, original);
        return false;
    }
    if (replacement != 0) {
        commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
            layout, draw.source_descriptor_param, original);
        ++impl_->stats.replacement_draws;
    }
    commands->bind_pipeline(reshade::api::pipeline_stage::all_graphics, companion);
    commands->bind_render_targets_and_depth_stencil(4, impl_->views.data(),
        reshade::api::resource_view {draw.depth_stencil_view});
    const bool captured = impl_->execute(commands, draw);
    commands->bind_pipeline(reshade::api::pipeline_stage::all_graphics,
                            reshade::api::pipeline {draw.pipeline});
    std::array<reshade::api::resource_view, 8> originals {};
    for (std::uint32_t index = 0; index < draw.render_target_count; ++index)
        originals[index] = reshade::api::resource_view {draw.render_target_views[index]};
    commands->bind_render_targets_and_depth_stencil(draw.render_target_count,
        originals.data(), reshade::api::resource_view {draw.depth_stencil_view});
    if (captured) ++impl_->stats.replayed_draws;
    // The application draw was already emitted above, so consume the event even
    // if a future indirect-command implementation reports capture failure.
    return true;
}
std::optional<SurfaceCaptureFrame> SurfaceCapture::finish_frame(void *command_list) {
    if (impl_ == nullptr || command_list == nullptr || impl_->completion_fence == 0)
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    auto *commands = static_cast<reshade::api::command_list *>(command_list);
    if (impl_->queue == nullptr ||
        commands != impl_->queue->get_immediate_command_list())
        return std::nullopt;
    auto completed = impl_->take_completed_frame();
    if (!impl_->cleared) return completed;

    std::size_t selected = impl_->readback.size();
    for (std::size_t attempt = 0; attempt < impl_->readback.size(); ++attempt) {
        const auto candidate = (impl_->submit_cursor + attempt) % impl_->readback.size();
        if (!impl_->readback[candidate].pending) {
            selected = candidate;
            break;
        }
    }
    if (selected == impl_->readback.size()) {
        ++impl_->stats.dropped_frames;
        impl_->cleared = false;
        return completed;
    }
    auto &slot = impl_->readback[selected];
    for (std::size_t index = 0; index < impl_->targets.size(); ++index) {
        commands->barrier(impl_->targets[index], reshade::api::resource_usage::render_target,
                          reshade::api::resource_usage::copy_source);
        commands->copy_texture_to_buffer(impl_->targets[index], 0, nullptr,
            slot.buffer, impl_->offsets[index], impl_->width, impl_->height);
        commands->barrier(impl_->targets[index], reshade::api::resource_usage::copy_source,
                          reshade::api::resource_usage::render_target);
    }
    impl_->queue->flush_immediate_command_list();
    slot.frame_index = impl_->frame++;
    slot.fence_value = impl_->next_fence_value++;
    slot.pending = true;
    if (!impl_->queue->signal(impl_->completion_fence, slot.fence_value)) {
        // Preserve correctness on an unexpected queue-fence failure. This slow
        // path is preferable to mapping memory still owned by the GPU.
        impl_->queue->wait_idle();
        slot.ready_without_fence = true;
    }
    impl_->submit_cursor = (selected + 1) % impl_->readback.size();
    impl_->cleared = false;
    return completed;
}
capture::CaptureStatistics SurfaceCapture::statistics() const noexcept { if (!impl_) return {}; std::lock_guard lock(impl_->mutex); return impl_->stats; }
std::uint32_t SurfaceCapture::width() const noexcept { if (!impl_) return 0; std::lock_guard lock(impl_->mutex); return impl_->width; }
std::uint32_t SurfaceCapture::height() const noexcept { if (!impl_) return 0; std::lock_guard lock(impl_->mutex); return impl_->height; }
void SurfaceCapture::queue_replacement(
    std::uint64_t material, std::vector<capture::ReplacementMip> mips) {
    if (impl_ == nullptr || material == 0 || mips.empty()) return;
    std::lock_guard lock(impl_->mutex);
    auto &entry = impl_->replacements[material];
    entry.mips = std::move(mips);
    entry.dirty = true;
    entry.rejected_source = {};
}
void SurfaceCapture::clear_replacements() {
    if (impl_ == nullptr) return;
    std::lock_guard lock(impl_->mutex);
    if (impl_->queue != nullptr) impl_->queue->wait_idle();
    impl_->release_replacements();
}

} // namespace neuralpass::vulkan_capture
