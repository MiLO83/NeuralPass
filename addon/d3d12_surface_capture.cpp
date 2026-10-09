#include "d3d12_surface_capture.hpp"

#include <d3dcompiler.h>

#include <algorithm>
#include <array>
#include <cctype>
#include <cmath>
#include <cstring>
#include <limits>
#include <mutex>
#include <sstream>
#include <string>
#include <unordered_map>

namespace neuralpass::d3d12_capture {
namespace {

template <typename T> void release(T *&object) {
    if (object != nullptr) object->Release();
    object = nullptr;
}

struct ShaderBlob {
    std::vector<std::uint8_t> code;
    std::string entry_point;

    [[nodiscard]] reshade::api::shader_desc desc() const {
        return {code.data(), code.size(), entry_point.empty() ? nullptr : entry_point.c_str()};
    }
};

struct PipelineTemplate {
    reshade::api::pipeline_layout layout = {};
    ShaderBlob vertex;
    ShaderBlob hull;
    ShaderBlob domain;
    ShaderBlob geometry;
    std::vector<reshade::api::input_element> input;
    std::vector<std::string> semantics;
    reshade::api::blend_desc blend {};
    reshade::api::rasterizer_desc rasterizer {};
    reshade::api::depth_stencil_desc depth_stencil {};
    reshade::api::primitive_topology topology = reshade::api::primitive_topology::undefined;
    reshade::api::format depth_format = reshade::api::format::unknown;
    std::array<reshade::api::format, 8> render_target_formats {};
    std::uint32_t render_target_count = 0;
    std::uint32_t sample_mask = UINT32_MAX;
    std::uint32_t sample_count = 1;
    std::vector<reshade::api::dynamic_state> dynamic_states;
    bool has_vertex = false;
};

struct PipelineKey {
    std::uint64_t pipeline = 0;
    std::uint64_t material = 0;
    std::string semantic;
    std::uint32_t semantic_index = 0;
    std::uint32_t source_register = 0;
    std::uint32_t source_space = 0;
    std::uint32_t sampler_register = 0;
    std::uint32_t sampler_space = 0;
    bool source_sampleable = false;

    bool operator==(const PipelineKey &) const = default;
};

struct PipelineKeyHash {
    std::size_t operator()(const PipelineKey &key) const noexcept {
        std::size_t value = std::hash<std::uint64_t> {}(key.pipeline);
        value ^= std::hash<std::uint64_t> {}(key.material) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::string> {}(key.semantic) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.semantic_index) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.source_register) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.source_space) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.sampler_register) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<std::uint32_t> {}(key.sampler_space) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        value ^= std::hash<bool> {}(key.source_sampleable) + 0x9e3779b9u +
            (value << 6) + (value >> 2);
        return value;
    }
};

struct ReadbackPlane {
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT footprint {};
    std::uint64_t offset = 0;
};

struct ReadbackSlot {
    ID3D12Resource *buffer = nullptr;
    std::array<ReadbackPlane, 4> planes {};
    std::uint64_t fence_value = 0;
    std::uint64_t frame_index = 0;
};

struct ReplacementTexture {
    std::vector<capture::ReplacementMip> mips;
    ID3D12Resource *source = nullptr;
    ID3D12Resource *texture = nullptr;
    ID3D12Resource *upload = nullptr;
    ID3D12Resource *rejected_source = nullptr;
    reshade::api::resource_view view = {};
    reshade::api::descriptor_table shadow_table = {};
    reshade::api::pipeline_layout shadow_layout = {};
    std::uint32_t shadow_param = 0;
    bool dirty = true;
    bool ready = false;
};

constexpr std::array<DXGI_FORMAT, 4> k_formats {
    DXGI_FORMAT_R32G32B32A32_UINT,
    DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_R32G32B32A32_FLOAT,
    DXGI_FORMAT_R32_FLOAT,
};

void store_shader(ShaderBlob &destination, const reshade::api::shader_desc &source) {
    if (source.code == nullptr || source.code_size == 0) {
        destination = {};
        return;
    }
    const auto *begin = static_cast<const std::uint8_t *>(source.code);
    destination.code.assign(begin, begin + source.code_size);
    destination.entry_point = source.entry_point != nullptr ? source.entry_point : "";
}

D3D12_RESOURCE_DESC texture_desc(std::uint32_t width, std::uint32_t height,
                                 DXGI_FORMAT format) {
    D3D12_RESOURCE_DESC result {};
    result.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    result.Width = width;
    result.Height = height;
    result.DepthOrArraySize = 1;
    result.MipLevels = 1;
    result.Format = format;
    result.SampleDesc.Count = 1;
    result.Layout = D3D12_TEXTURE_LAYOUT_UNKNOWN;
    result.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    return result;
}

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

} // namespace

struct SurfaceCapture::Impl {
    mutable std::recursive_mutex mutex;
    ID3D12Device *device = nullptr;
    ID3D12CommandQueue *native_queue = nullptr;
    reshade::api::device *api_device = nullptr;
    reshade::api::command_queue *api_queue = nullptr;
    ID3D12DescriptorHeap *rtv_heap = nullptr;
    ID3D12CommandSignature *draw_signature = nullptr;
    ID3D12CommandSignature *draw_indexed_signature = nullptr;
    std::array<ID3D12Resource *, 4> targets {};
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 4> target_views {};
    std::array<ReadbackSlot, 3> readback {};
    ID3D12Fence *fence = nullptr;
    std::unordered_map<std::uint64_t, PipelineTemplate> pipelines;
    std::unordered_map<PipelineKey, reshade::api::pipeline, PipelineKeyHash> variants;
    std::unordered_map<std::uint64_t, ReplacementTexture> replacements;
    std::uint32_t width = 0;
    std::uint32_t height = 0;
    std::size_t next_readback = 0;
    std::uint64_t next_fence = 1;
    std::uint64_t pending_signal = 0;
    std::uint64_t next_frame = 1;
    capture::CaptureStatistics statistics;
    bool cleared = false;

    ~Impl() {
        if (api_queue != nullptr) api_queue->wait_idle();
        release_surfaces();
        release_pipelines();
        release_replacements();
        release(draw_signature);
        release(draw_indexed_signature);
        release(native_queue);
        release(device);
    }

    void release_replacement(ReplacementTexture &entry) {
        if (api_device != nullptr) {
            if (entry.shadow_table != 0)
                api_device->free_descriptor_table(entry.shadow_table);
            if (entry.view != 0) api_device->destroy_resource_view(entry.view);
        }
        release(entry.upload);
        release(entry.texture);
        release(entry.source);
        release(entry.rejected_source);
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
        for (auto &slot : readback) {
            release(slot.buffer);
            slot = {};
        }
        for (auto *&target : targets) release(target);
        release(rtv_heap);
        release(fence);
        width = height = 0;
        pending_signal = 0;
        cleared = false;
    }

    void release_pipelines() {
        if (api_device != nullptr)
            for (const auto &[key, pipeline] : variants) {
                (void)key;
                api_device->destroy_pipeline(pipeline);
            }
        variants.clear();
        pipelines.clear();
    }

    bool create_surfaces(std::uint32_t new_width, std::uint32_t new_height) {
        if (api_queue != nullptr && width != 0) api_queue->wait_idle();
        release_surfaces();
        if (device == nullptr || new_width == 0 || new_height == 0) return false;
        D3D12_DESCRIPTOR_HEAP_DESC heap_desc {};
        heap_desc.Type = D3D12_DESCRIPTOR_HEAP_TYPE_RTV;
        heap_desc.NumDescriptors = 4;
        if (FAILED(device->CreateDescriptorHeap(&heap_desc, IID_PPV_ARGS(&rtv_heap))))
            return false;
        auto handle = rtv_heap->GetCPUDescriptorHandleForHeapStart();
        const auto increment = device->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
        D3D12_HEAP_PROPERTIES properties {};
        properties.Type = D3D12_HEAP_TYPE_DEFAULT;
        for (std::size_t plane = 0; plane < targets.size(); ++plane) {
            const auto desc = texture_desc(new_width, new_height, k_formats[plane]);
            D3D12_CLEAR_VALUE clear {};
            clear.Format = k_formats[plane];
            if (plane == 2 || plane == 3) {
                const auto missing = std::numeric_limits<float>::quiet_NaN();
                clear.Color[0] = clear.Color[1] = clear.Color[2] = clear.Color[3] = missing;
            }
            if (FAILED(device->CreateCommittedResource(&properties, D3D12_HEAP_FLAG_NONE,
                    &desc, D3D12_RESOURCE_STATE_RENDER_TARGET, &clear,
                    IID_PPV_ARGS(&targets[plane])))) {
                release_surfaces();
                return false;
            }
            target_views[plane] = handle;
            device->CreateRenderTargetView(targets[plane], nullptr, handle);
            handle.ptr += increment;
        }
        std::array<D3D12_PLACED_SUBRESOURCE_FOOTPRINT, 4> footprints {};
        std::array<std::uint64_t, 4> offsets {};
        std::uint64_t total = 0;
        for (std::size_t plane = 0; plane < targets.size(); ++plane) {
            const auto desc = targets[plane]->GetDesc();
            total = (total + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1) &
                ~(D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT - 1);
            offsets[plane] = total;
            std::uint64_t plane_size = 0;
            device->GetCopyableFootprints(&desc, 0, 1, 0, &footprints[plane],
                                          nullptr, nullptr, &plane_size);
            footprints[plane].Offset = total;
            total = offsets[plane] + plane_size;
        }
        D3D12_HEAP_PROPERTIES readback_properties {};
        readback_properties.Type = D3D12_HEAP_TYPE_READBACK;
        D3D12_RESOURCE_DESC buffer_desc {};
        buffer_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        buffer_desc.Width = total;
        buffer_desc.Height = 1;
        buffer_desc.DepthOrArraySize = 1;
        buffer_desc.MipLevels = 1;
        buffer_desc.SampleDesc.Count = 1;
        buffer_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        for (auto &slot : readback) {
            if (FAILED(device->CreateCommittedResource(&readback_properties,
                    D3D12_HEAP_FLAG_NONE, &buffer_desc, D3D12_RESOURCE_STATE_COPY_DEST,
                    nullptr, IID_PPV_ARGS(&slot.buffer)))) {
                release_surfaces();
                return false;
            }
            for (std::size_t plane = 0; plane < slot.planes.size(); ++plane)
                slot.planes[plane] = {footprints[plane], offsets[plane]};
        }
        if (FAILED(device->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)))) {
            release_surfaces();
            return false;
        }
        width = new_width;
        height = new_height;
        next_readback = 0;
        next_fence = 1;
        return true;
    }

    reshade::api::pipeline capture_pipeline(std::uint64_t source_pipeline,
                                            const capture::UvInput &uv,
                                            std::uint64_t material_id,
                                            const capture::DrawCommand &draw) {
        PipelineKey key {source_pipeline, material_id, uv.name, uv.index,
            draw.source_register, draw.source_space, draw.sampler_register,
            draw.sampler_space, draw.source_sampleable};
        if (const auto found = variants.find(key); found != variants.end()) return found->second;
        const auto known = pipelines.find(source_pipeline);
        if (known == pipelines.end() || api_device == nullptr || uv.name.empty()) return {};
        const auto &source = known->second;
        if (!source.has_vertex || source.sample_count != 1) return {};
        if (!std::all_of(uv.name.begin(), uv.name.end(), [](unsigned char value) {
                return std::isalnum(value) != 0 || value == '_';
            })) return {};
        std::ostringstream hlsl;
        if (draw.source_sampleable)
            hlsl << "Texture2D<float4> source_texture : register(t"
                 << draw.source_register << ", space" << draw.source_space
                 << "); SamplerState source_sampler : register(s"
                 << draw.sampler_register << ", space" << draw.sampler_space << ");\n";
        hlsl << "struct Input { float4 position : SV_Position; float2 uv : "
             << uv.name << uv.index << "; };\n"
             << "struct Output { uint4 surface : SV_Target0; float4 gradients : SV_Target1; "
                "float4 source : SV_Target2; float depth : SV_Target3; };\n"
             << "Output main(Input input) { Output output; output.surface = uint4("
             << static_cast<std::uint32_t>(material_id) << "u,"
             << static_cast<std::uint32_t>(material_id >> 32)
             << "u,asuint(input.uv.x),asuint(input.uv.y)); float2 dx=ddx(input.uv); "
                "float2 dy=ddy(input.uv); output.gradients=float4(dx.x,dy.x,dx.y,dy.y); ";
        if (draw.source_sampleable)
            hlsl << "output.source=source_texture.SampleGrad(source_sampler,input.uv,dx,dy); ";
        else
            hlsl << "output.source=float4(asfloat(0x7fc00000u),asfloat(0x7fc00000u),"
                    "asfloat(0x7fc00000u),asfloat(0x7fc00000u)); ";
        hlsl << "output.depth=input.position.z; return output; }";
        ID3DBlob *bytecode = nullptr;
        ID3DBlob *errors = nullptr;
        const auto text = hlsl.str();
        const auto compiled = D3DCompile(text.data(), text.size(), "NeuralPassD3D12Capture",
            nullptr, nullptr, "main", "ps_5_1", D3DCOMPILE_OPTIMIZATION_LEVEL3, 0,
            &bytecode, &errors);
        release(errors);
        if (FAILED(compiled) || bytecode == nullptr) { release(bytecode); return {}; }

        auto vertex = source.vertex.desc();
        auto hull = source.hull.desc();
        auto domain = source.domain.desc();
        auto geometry = source.geometry.desc();
        reshade::api::shader_desc pixel {bytecode->GetBufferPointer(), bytecode->GetBufferSize()};
        auto blend = source.blend;
        blend.alpha_to_coverage_enable = false;
        for (std::size_t index = 0; index < 8; ++index) {
            blend.blend_enable[index] = false;
            blend.logic_op_enable[index] = false;
            blend.render_target_write_mask[index] = index < 4 ? 0xF : 0;
        }
        auto depth = source.depth_stencil;
        depth.depth_write_mask = false;
        if (depth.depth_enable) depth.depth_func = reshade::api::compare_op::equal;
        depth.front_stencil_pass_op = reshade::api::stencil_op::keep;
        depth.front_stencil_fail_op = reshade::api::stencil_op::keep;
        depth.front_stencil_depth_fail_op = reshade::api::stencil_op::keep;
        depth.back_stencil_pass_op = reshade::api::stencil_op::keep;
        depth.back_stencil_fail_op = reshade::api::stencil_op::keep;
        depth.back_stencil_depth_fail_op = reshade::api::stencil_op::keep;
        std::array<reshade::api::format, 4> formats {
            reshade::api::format::r32g32b32a32_uint,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32g32b32a32_float,
            reshade::api::format::r32_float,
        };
        const std::uint32_t sample_count = 1;
        std::vector<reshade::api::pipeline_subobject> objects;
        objects.push_back({reshade::api::pipeline_subobject_type::vertex_shader, 1, &vertex});
        if (!source.hull.code.empty())
            objects.push_back({reshade::api::pipeline_subobject_type::hull_shader, 1, &hull});
        if (!source.domain.code.empty())
            objects.push_back({reshade::api::pipeline_subobject_type::domain_shader, 1, &domain});
        if (!source.geometry.code.empty())
            objects.push_back({reshade::api::pipeline_subobject_type::geometry_shader, 1, &geometry});
        objects.push_back({reshade::api::pipeline_subobject_type::pixel_shader, 1, &pixel});
        objects.push_back({reshade::api::pipeline_subobject_type::input_layout,
            static_cast<std::uint32_t>(source.input.size()),
            const_cast<reshade::api::input_element *>(source.input.data())});
        objects.push_back({reshade::api::pipeline_subobject_type::blend_state, 1, &blend});
        objects.push_back({reshade::api::pipeline_subobject_type::rasterizer_state, 1,
                           const_cast<reshade::api::rasterizer_desc *>(&source.rasterizer)});
        objects.push_back({reshade::api::pipeline_subobject_type::depth_stencil_state, 1, &depth});
        objects.push_back({reshade::api::pipeline_subobject_type::primitive_topology, 1,
                           const_cast<reshade::api::primitive_topology *>(&source.topology)});
        objects.push_back({reshade::api::pipeline_subobject_type::depth_stencil_format, 1,
                           const_cast<reshade::api::format *>(&source.depth_format)});
        objects.push_back({reshade::api::pipeline_subobject_type::render_target_formats, 4,
                           formats.data()});
        objects.push_back({reshade::api::pipeline_subobject_type::sample_mask, 1,
                           const_cast<std::uint32_t *>(&source.sample_mask)});
        objects.push_back({reshade::api::pipeline_subobject_type::sample_count, 1,
                           const_cast<std::uint32_t *>(&sample_count)});
        if (!source.dynamic_states.empty())
            objects.push_back({reshade::api::pipeline_subobject_type::dynamic_pipeline_states,
                static_cast<std::uint32_t>(source.dynamic_states.size()),
                const_cast<reshade::api::dynamic_state *>(source.dynamic_states.data())});
        reshade::api::pipeline result = {};
        const bool created = api_device->create_pipeline(source.layout,
            static_cast<std::uint32_t>(objects.size()), objects.data(), &result);
        release(bytecode);
        if (!created) return {};
        variants.emplace(std::move(key), result);
        return result;
    }

    reshade::api::descriptor_table replacement_table(
        ID3D12GraphicsCommandList *commands, std::uint64_t material_id,
        const capture::DrawCommand &draw) {
        const auto found = replacements.find(material_id);
        if (found == replacements.end() || found->second.mips.empty() ||
            api_device == nullptr || draw.api_command_list == nullptr ||
            draw.source_resource == 0 || draw.source_view == 0 ||
            !draw.source_descriptor_isolatable || draw.source_descriptor_table == 0 ||
            draw.pipeline_layout == 0 ||
            static_cast<reshade::api::descriptor_type>(draw.source_descriptor_type) !=
                reshade::api::descriptor_type::shader_resource_view)
            return {};
        auto &entry = found->second;
        auto *source = reinterpret_cast<ID3D12Resource *>(draw.source_resource);
        if (entry.rejected_source == source) return {};
        const reshade::api::resource source_handle {draw.source_resource};
        const reshade::api::resource_view source_view {draw.source_view};
        const auto desc = api_device->get_resource_desc(source_handle);
        const auto view_desc = api_device->get_resource_view_desc(source_view);
        const auto native_desc = source->GetDesc();
        const bool valid = desc.type == reshade::api::resource_type::texture_2d &&
            desc.texture.depth_or_layers == 1 && desc.texture.samples == 1 &&
            view_desc.type == reshade::api::resource_view_type::texture_2d &&
            view_desc.texture.first_layer == 0 && byte_color_format(view_desc.format) &&
            native_desc.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D &&
            native_desc.DepthOrArraySize == 1 && native_desc.SampleDesc.Count == 1;
        if (!valid) {
            release(entry.rejected_source);
            entry.rejected_source = source;
            source->AddRef();
            ++statistics.rejected_replacements;
            return {};
        }

        auto reject = [&]() -> reshade::api::descriptor_table {
            if (api_device != nullptr) {
                if (entry.shadow_table != 0)
                    api_device->free_descriptor_table(entry.shadow_table);
                if (entry.view != 0) api_device->destroy_resource_view(entry.view);
            }
            entry.shadow_table = {};
            entry.view = {};
            release(entry.upload);
            release(entry.texture);
            release(entry.source);
            release(entry.rejected_source);
            entry.rejected_source = source;
            source->AddRef();
            entry.ready = false;
            ++statistics.rejected_replacements;
            return {};
        };

        if (entry.source != source) {
            if (api_queue != nullptr) api_queue->wait_idle();
            if (entry.shadow_table != 0)
                api_device->free_descriptor_table(entry.shadow_table);
            if (entry.view != 0) api_device->destroy_resource_view(entry.view);
            entry.shadow_table = {};
            entry.view = {};
            release(entry.upload);
            release(entry.texture);
            release(entry.source);
            release(entry.rejected_source);

            D3D12_HEAP_PROPERTIES default_heap {};
            default_heap.Type = D3D12_HEAP_TYPE_DEFAULT;
            if (FAILED(device->CreateCommittedResource(&default_heap, D3D12_HEAP_FLAG_NONE,
                    &native_desc, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                    IID_PPV_ARGS(&entry.texture))))
                return reject();
            if (!api_device->create_resource_view(
                    reshade::api::resource {
                        reinterpret_cast<std::uint64_t>(entry.texture)},
                    reshade::api::resource_usage::shader_resource, view_desc,
                    &entry.view))
                return reject();

            std::uint64_t upload_size = 0;
            device->GetCopyableFootprints(&native_desc, 0, native_desc.MipLevels, 0,
                                          nullptr, nullptr, nullptr, &upload_size);
            D3D12_HEAP_PROPERTIES upload_heap {};
            upload_heap.Type = D3D12_HEAP_TYPE_UPLOAD;
            D3D12_RESOURCE_DESC upload_desc {};
            upload_desc.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            upload_desc.Width = upload_size;
            upload_desc.Height = 1;
            upload_desc.DepthOrArraySize = 1;
            upload_desc.MipLevels = 1;
            upload_desc.SampleDesc.Count = 1;
            upload_desc.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (upload_size == 0 || FAILED(device->CreateCommittedResource(&upload_heap,
                    D3D12_HEAP_FLAG_NONE, &upload_desc,
                    D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                    IID_PPV_ARGS(&entry.upload))))
                return reject();
            entry.source = source;
            source->AddRef();
            entry.dirty = true;
            entry.ready = false;
        }

        if (entry.dirty) {
            const auto old_usage = static_cast<reshade::api::resource_usage>(draw.source_usage);
            if ((old_usage & reshade::api::resource_usage::shader_resource) ==
                    reshade::api::resource_usage::undefined)
                return reject();
            auto *api_commands = static_cast<reshade::api::command_list *>(
                draw.api_command_list);
            const reshade::api::resource replacement_handle {
                reinterpret_cast<std::uint64_t>(entry.texture)};
            if (entry.ready)
                api_commands->barrier(replacement_handle,
                                      reshade::api::resource_usage::shader_resource_pixel,
                                      reshade::api::resource_usage::copy_dest);
            api_commands->barrier(source_handle, old_usage,
                                  reshade::api::resource_usage::copy_source);
            commands->CopyResource(entry.texture, source);
            api_commands->barrier(source_handle, reshade::api::resource_usage::copy_source,
                                  old_usage);

            std::vector<D3D12_PLACED_SUBRESOURCE_FOOTPRINT> footprints(
                native_desc.MipLevels);
            device->GetCopyableFootprints(&native_desc, 0, native_desc.MipLevels, 0,
                                          footprints.data(), nullptr, nullptr, nullptr);
            std::uint8_t *mapped = nullptr;
            if (FAILED(entry.upload->Map(0, nullptr,
                    reinterpret_cast<void **>(&mapped)))) {
                entry.ready = false;
                ++statistics.rejected_replacements;
                return {};
            }
            const bool bgra = bgra_format(view_desc.format);
            const auto first_level = view_desc.texture.first_level;
            const auto view_levels = view_desc.texture.levels == UINT32_MAX
                ? native_desc.MipLevels - first_level : view_desc.texture.levels;
            const auto levels = std::min<std::size_t>(
                std::min<std::uint32_t>(view_levels, native_desc.MipLevels - first_level),
                entry.mips.size());
            for (std::size_t level = 0; level < levels; ++level) {
                const auto &mip = entry.mips[level];
                if (mip.width == 0 || mip.height == 0 ||
                    mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4 ||
                    mip.coverage.size() != static_cast<std::size_t>(mip.width) * mip.height)
                    continue;
                const auto subresource = first_level + static_cast<std::uint32_t>(level);
                const auto mip_width = std::max<UINT>(
                    1, static_cast<UINT>(native_desc.Width) >> subresource);
                const auto mip_height = std::max<UINT>(1, native_desc.Height >> subresource);
                auto &footprint = footprints[subresource];
                for (UINT y = 0; y < mip_height; ++y) {
                    auto *row = mapped + footprint.Offset +
                        static_cast<std::size_t>(y) * footprint.Footprint.RowPitch;
                    const auto ay = std::min(mip.height - 1, y * mip.height / mip_height);
                    for (UINT x = 0; x < mip_width; ++x) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / mip_width);
                        const auto *rgba = mip.rgba.data() +
                            (static_cast<std::size_t>(ay) * mip.width + ax) * 4;
                        auto *destination = row + static_cast<std::size_t>(x) * 4;
                        destination[0] = rgba[bgra ? 2 : 0];
                        destination[1] = rgba[1];
                        destination[2] = rgba[bgra ? 0 : 2];
                        destination[3] = rgba[3];
                    }
                }
            }
            entry.upload->Unmap(0, nullptr);

            for (std::size_t level = 0; level < levels; ++level) {
                const auto &mip = entry.mips[level];
                if (mip.width == 0 || mip.height == 0 ||
                    mip.rgba.size() != static_cast<std::size_t>(mip.width) * mip.height * 4 ||
                    mip.coverage.size() != static_cast<std::size_t>(mip.width) * mip.height)
                    continue;
                const auto subresource = first_level + static_cast<std::uint32_t>(level);
                const auto mip_width = std::max<UINT>(
                    1, static_cast<UINT>(native_desc.Width) >> subresource);
                const auto mip_height = std::max<UINT>(1, native_desc.Height >> subresource);
                D3D12_TEXTURE_COPY_LOCATION source_location {};
                source_location.pResource = entry.upload;
                source_location.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                source_location.PlacedFootprint = footprints[subresource];
                D3D12_TEXTURE_COPY_LOCATION destination_location {};
                destination_location.pResource = entry.texture;
                destination_location.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                destination_location.SubresourceIndex = subresource;
                for (UINT y = 0; y < mip_height; ++y) {
                    const auto ay = std::min(mip.height - 1, y * mip.height / mip_height);
                    UINT x = 0;
                    while (x < mip_width) {
                        const auto ax = std::min(mip.width - 1, x * mip.width / mip_width);
                        if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + ax] == 0) {
                            ++x;
                            continue;
                        }
                        const auto begin = x++;
                        while (x < mip_width) {
                            const auto sample_x = std::min(mip.width - 1,
                                x * mip.width / mip_width);
                            if (mip.coverage[static_cast<std::size_t>(ay) * mip.width + sample_x] == 0)
                                break;
                            ++x;
                        }
                        const D3D12_BOX box {begin, y, 0, x, y + 1, 1};
                        commands->CopyTextureRegion(&destination_location, begin, y, 0,
                                                    &source_location, &box);
                    }
                }
            }
            api_commands->barrier(replacement_handle,
                                  reshade::api::resource_usage::copy_dest,
                                  reshade::api::resource_usage::shader_resource_pixel);
            entry.dirty = false;
            entry.ready = true;
        }
        if (!entry.ready) return {};

        const reshade::api::pipeline_layout layout {draw.pipeline_layout};
        if (entry.shadow_table == 0 || entry.shadow_layout != layout ||
            entry.shadow_param != draw.source_descriptor_param) {
            if (entry.shadow_table != 0)
                api_device->free_descriptor_table(entry.shadow_table);
            entry.shadow_table = {};
            if (!api_device->allocate_descriptor_table(
                    layout, draw.source_descriptor_param, &entry.shadow_table)) {
                ++statistics.rejected_replacements;
                return {};
            }
            entry.shadow_layout = layout;
            entry.shadow_param = draw.source_descriptor_param;
        }
        const reshade::api::descriptor_table original {draw.source_descriptor_table};
        std::array<reshade::api::descriptor_table_copy, 32> copies {};
        for (std::uint32_t index = 0; index < draw.source_table_range_count; ++index) {
            const auto &range = draw.source_table_ranges[index];
            copies[index] = {original, range.binding, 0, entry.shadow_table,
                             range.binding, 0, range.count};
        }
        api_device->copy_descriptor_tables(draw.source_table_range_count, copies.data());
        const reshade::api::descriptor_table_update update {
            entry.shadow_table, draw.source_descriptor_binding,
            draw.source_descriptor_array_offset, 1,
            reshade::api::descriptor_type::shader_resource_view, &entry.view};
        api_device->update_descriptor_tables(1, &update);
        return entry.shadow_table;
    }

    bool create_signatures() {
        if (draw_signature != nullptr && draw_indexed_signature != nullptr) return true;
        D3D12_INDIRECT_ARGUMENT_DESC argument {};
        D3D12_COMMAND_SIGNATURE_DESC signature {};
        signature.NumArgumentDescs = 1;
        signature.pArgumentDescs = &argument;
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW;
        signature.ByteStride = sizeof(D3D12_DRAW_ARGUMENTS);
        if (FAILED(device->CreateCommandSignature(&signature, nullptr,
                                                  IID_PPV_ARGS(&draw_signature))))
            return false;
        argument.Type = D3D12_INDIRECT_ARGUMENT_TYPE_DRAW_INDEXED;
        signature.ByteStride = sizeof(D3D12_DRAW_INDEXED_ARGUMENTS);
        if (FAILED(device->CreateCommandSignature(&signature, nullptr,
                                                  IID_PPV_ARGS(&draw_indexed_signature)))) {
            release(draw_signature);
            return false;
        }
        return true;
    }

    bool execute(ID3D12GraphicsCommandList *commands, const capture::DrawCommand &draw) {
        switch (draw.kind) {
        case capture::DrawKind::direct:
            commands->DrawInstanced(draw.vertex_or_index_count, draw.instance_count,
                                    draw.first_vertex_or_index, draw.first_instance);
            return true;
        case capture::DrawKind::indexed:
            commands->DrawIndexedInstanced(draw.vertex_or_index_count, draw.instance_count,
                draw.first_vertex_or_index, draw.vertex_offset, draw.first_instance);
            return true;
        case capture::DrawKind::indirect: {
            auto *arguments = static_cast<ID3D12Resource *>(draw.argument_buffer);
            if (arguments == nullptr || draw_signature == nullptr) return false;
            commands->ExecuteIndirect(draw_signature, draw.draw_count, arguments,
                                      draw.argument_offset, nullptr, 0);
            return true;
        }
        case capture::DrawKind::indexed_indirect: {
            auto *arguments = static_cast<ID3D12Resource *>(draw.argument_buffer);
            if (arguments == nullptr || draw_indexed_signature == nullptr) return false;
            commands->ExecuteIndirect(draw_indexed_signature, draw.draw_count, arguments,
                                      draw.argument_offset, nullptr, 0);
            return true;
        }
        }
        return false;
    }

    void clear(ID3D12GraphicsCommandList *commands) {
        const float zero[4] {};
        const float missing[4] {
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN(),
            std::numeric_limits<float>::quiet_NaN()};
        commands->ClearRenderTargetView(target_views[0], zero, 0, nullptr);
        commands->ClearRenderTargetView(target_views[1], zero, 0, nullptr);
        commands->ClearRenderTargetView(target_views[2], missing, 0, nullptr);
        commands->ClearRenderTargetView(target_views[3], missing, 0, nullptr);
        cleared = true;
    }
};

SurfaceCapture::SurfaceCapture() : impl_(new Impl) {}
SurfaceCapture::~SurfaceCapture() { reset(); }

void SurfaceCapture::attach(reshade::api::device *device,
                            reshade::api::command_queue *queue) {
    if (impl_ == nullptr) impl_ = new Impl;
    std::lock_guard lock(impl_->mutex);
    impl_->api_device = device;
    impl_->api_queue = queue;
    attach_native_queue(queue != nullptr
        ? reinterpret_cast<ID3D12CommandQueue *>(queue->get_native()) : nullptr);
}

void SurfaceCapture::attach_native_queue(ID3D12CommandQueue *queue) {
    if (impl_ == nullptr) impl_ = new Impl;
    std::lock_guard lock(impl_->mutex);
    if (impl_->native_queue == queue) return;
    release(impl_->native_queue);
    impl_->native_queue = queue;
    if (impl_->native_queue != nullptr) impl_->native_queue->AddRef();
}

bool SurfaceCapture::signal_submitted() {
    if (impl_ == nullptr) return false;
    std::lock_guard lock(impl_->mutex);
    if (impl_->native_queue == nullptr || impl_->fence == nullptr ||
        impl_->pending_signal == 0) return false;
    const auto value = impl_->pending_signal;
    if (FAILED(impl_->native_queue->Signal(impl_->fence, value))) return false;
    impl_->pending_signal = 0;
    return true;
}

void SurfaceCapture::register_pipeline(reshade::api::pipeline_layout layout,
                                       std::uint32_t subobject_count,
                                       const reshade::api::pipeline_subobject *subobjects,
                                       reshade::api::pipeline pipeline) {
    if (impl_ == nullptr || subobjects == nullptr || pipeline == 0) return;
    std::lock_guard lock(impl_->mutex);
    PipelineTemplate result;
    result.layout = layout;
    for (std::uint32_t index = 0; index < subobject_count; ++index) {
        const auto &object = subobjects[index];
        switch (object.type) {
        case reshade::api::pipeline_subobject_type::vertex_shader:
            if (object.count != 0) {
                store_shader(result.vertex, *static_cast<const reshade::api::shader_desc *>(object.data));
                result.has_vertex = true;
            }
            break;
        case reshade::api::pipeline_subobject_type::hull_shader:
            if (object.count != 0) store_shader(result.hull, *static_cast<const reshade::api::shader_desc *>(object.data));
            break;
        case reshade::api::pipeline_subobject_type::domain_shader:
            if (object.count != 0) store_shader(result.domain, *static_cast<const reshade::api::shader_desc *>(object.data));
            break;
        case reshade::api::pipeline_subobject_type::geometry_shader:
            if (object.count != 0) store_shader(result.geometry, *static_cast<const reshade::api::shader_desc *>(object.data));
            break;
        case reshade::api::pipeline_subobject_type::input_layout: {
            const auto *input = static_cast<const reshade::api::input_element *>(object.data);
            if (object.count != 0) result.input.assign(input, input + object.count);
            result.semantics.reserve(object.count);
            for (std::uint32_t element = 0; element < object.count; ++element)
                result.semantics.emplace_back(input[element].semantic != nullptr ? input[element].semantic : "");
            for (std::uint32_t element = 0; element < object.count; ++element)
                result.input[element].semantic = result.semantics[element].empty()
                    ? nullptr : result.semantics[element].c_str();
            break;
        }
        case reshade::api::pipeline_subobject_type::stream_output_state:
            // Deliberately omit stream output from the companion PSO: replaying
            // application SO writes would duplicate side effects.
            break;
        case reshade::api::pipeline_subobject_type::blend_state:
            result.blend = *static_cast<const reshade::api::blend_desc *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::rasterizer_state:
            result.rasterizer = *static_cast<const reshade::api::rasterizer_desc *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::depth_stencil_state:
            result.depth_stencil = *static_cast<const reshade::api::depth_stencil_desc *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::primitive_topology:
            result.topology = *static_cast<const reshade::api::primitive_topology *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::depth_stencil_format:
            result.depth_format = *static_cast<const reshade::api::format *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::render_target_formats:
            result.render_target_count = std::min<std::uint32_t>(object.count, 8);
            std::copy_n(static_cast<const reshade::api::format *>(object.data),
                        result.render_target_count, result.render_target_formats.begin());
            break;
        case reshade::api::pipeline_subobject_type::sample_mask:
            result.sample_mask = *static_cast<const std::uint32_t *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::sample_count:
            result.sample_count = *static_cast<const std::uint32_t *>(object.data);
            break;
        case reshade::api::pipeline_subobject_type::dynamic_pipeline_states: {
            if (object.count != 0) {
                const auto *states = static_cast<const reshade::api::dynamic_state *>(object.data);
                result.dynamic_states.assign(states, states + object.count);
            }
            break;
        }
        default:
            break;
        }
    }
    impl_->pipelines.insert_or_assign(pipeline.handle, std::move(result));
}

void SurfaceCapture::unregister_pipeline(reshade::api::pipeline pipeline) {
    if (impl_ == nullptr) return;
    std::lock_guard lock(impl_->mutex);
    impl_->pipelines.erase(pipeline.handle);
    for (auto iterator = impl_->variants.begin(); iterator != impl_->variants.end();) {
        if (iterator->first.pipeline != pipeline.handle) { ++iterator; continue; }
        if (impl_->api_device != nullptr) impl_->api_device->destroy_pipeline(iterator->second);
        iterator = impl_->variants.erase(iterator);
    }
}

void SurfaceCapture::register_prebuilt_variant(
    std::uint64_t source_pipeline, const capture::UvInput &uv,
    std::uint64_t material_id, const capture::DrawCommand &draw,
    std::uint64_t capture_pipeline) {
    if (impl_ == nullptr || source_pipeline == 0 || material_id == 0 ||
        capture_pipeline == 0) return;
    std::lock_guard lock(impl_->mutex);
    PipelineKey key {source_pipeline, material_id, uv.name, uv.index,
        draw.source_register, draw.source_space, draw.sampler_register,
        draw.sampler_space, draw.source_sampleable};
    impl_->variants.insert_or_assign(
        std::move(key), reshade::api::pipeline {capture_pipeline});
}

capture::GraphicsBackend SurfaceCapture::backend() const noexcept {
    return capture::GraphicsBackend::d3d12;
}

capture::CaptureCapabilities SurfaceCapture::capabilities() const noexcept {
    return {
        .direct_draws = true,
        .indexed_draws = true,
        .indirect_draws = true,
        .replacement_textures = true,
        .asynchronous_readback = true,
        .shader_coverage_preserved = false,
    };
}

bool SurfaceCapture::initialize(void *native_device, std::uint32_t width,
                                std::uint32_t height) {
    if (impl_ == nullptr) impl_ = new Impl;
    std::lock_guard lock(impl_->mutex);
    auto *device = static_cast<ID3D12Device *>(native_device);
    if (device == nullptr) return false;
    if (impl_->device != device) {
        release(impl_->draw_signature);
        release(impl_->draw_indexed_signature);
        release(impl_->device);
        impl_->device = device;
        impl_->device->AddRef();
    }
    return impl_->create_signatures() && impl_->create_surfaces(width, height);
}

void SurfaceCapture::reset() {
    delete impl_;
    impl_ = nullptr;
}

bool SurfaceCapture::replay(void *native_command_list, const capture::UvInput &uv,
                            std::uint64_t material_id,
                            const capture::DrawCommand &draw, int) {
    if (impl_ == nullptr || native_command_list == nullptr || material_id == 0 ||
        !uv.valid() || !draw.target_compatible || draw.inside_render_pass ||
        draw.pipeline == 0 || draw.render_target_count == 0)
        return false;
    std::lock_guard lock(impl_->mutex);
    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list);
    if (commands->GetType() != D3D12_COMMAND_LIST_TYPE_DIRECT) return false;
    const auto capture_pipeline = impl_->capture_pipeline(
        draw.pipeline, uv, material_id, draw);
    if (capture_pipeline == 0) return false;
    if (!impl_->cleared) impl_->clear(commands);

    // Clone the complete bounded table and change only the selected SRV. The
    // application-owned table is never mutated, and is rebound before capture.
    const auto replacement_table = impl_->replacement_table(
        commands, material_id, draw);
    auto *api_commands = static_cast<reshade::api::command_list *>(draw.api_command_list);
    const reshade::api::pipeline_layout layout {draw.pipeline_layout};
    const reshade::api::descriptor_table original_table {draw.source_descriptor_table};
    if (replacement_table != 0)
        api_commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
            layout, draw.source_descriptor_param, replacement_table);

    // The application draw is issued once here; returning true suppresses the
    // corresponding ReShade wrapper call.
    if (!impl_->execute(commands, draw)) {
        if (replacement_table != 0)
            api_commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
                layout, draw.source_descriptor_param, original_table);
        return false;
    }
    if (replacement_table != 0) {
        api_commands->bind_descriptor_table(reshade::api::shader_stage::pixel,
            layout, draw.source_descriptor_param, original_table);
        ++impl_->statistics.replacement_draws;
    }
    commands->SetPipelineState(reinterpret_cast<ID3D12PipelineState *>(capture_pipeline.handle));
    D3D12_CPU_DESCRIPTOR_HANDLE capture_depth {
        static_cast<SIZE_T>(draw.depth_stencil_view)};
    commands->OMSetRenderTargets(static_cast<UINT>(impl_->target_views.size()),
        impl_->target_views.data(), FALSE,
        draw.depth_stencil_view != 0 ? &capture_depth : nullptr);
    if (!impl_->execute(commands, draw)) return false;
    ++impl_->statistics.replayed_draws;
    commands->SetPipelineState(
        reinterpret_cast<ID3D12PipelineState *>(draw.pipeline));
    std::array<D3D12_CPU_DESCRIPTOR_HANDLE, 8> original_targets {};
    for (std::uint32_t index = 0; index < draw.render_target_count; ++index)
        original_targets[index].ptr = static_cast<SIZE_T>(draw.render_target_views[index]);
    D3D12_CPU_DESCRIPTOR_HANDLE original_depth {
        static_cast<SIZE_T>(draw.depth_stencil_view)};
    commands->OMSetRenderTargets(draw.render_target_count,
        original_targets.data(), FALSE,
        draw.depth_stencil_view != 0 ? &original_depth : nullptr);
    return true;
}

std::optional<SurfaceCaptureFrame> SurfaceCapture::finish_frame(void *native_command_list) {
    if (impl_ == nullptr || native_command_list == nullptr || impl_->fence == nullptr)
        return std::nullopt;
    std::lock_guard lock(impl_->mutex);
    auto *commands = static_cast<ID3D12GraphicsCommandList *>(native_command_list);
    std::optional<SurfaceCaptureFrame> result;
    const auto completed = impl_->fence->GetCompletedValue();
    for (auto &slot : impl_->readback) {
        if (slot.fence_value == 0 || slot.fence_value > completed) continue;
        void *mapped = nullptr;
        D3D12_RANGE range {0, static_cast<SIZE_T>(slot.buffer->GetDesc().Width)};
        if (SUCCEEDED(slot.buffer->Map(0, &range, &mapped))) {
            SurfaceCaptureFrame frame(impl_->width, impl_->height, slot.frame_index);
            for (std::uint32_t y = 0; y < impl_->height; ++y) {
                const auto *identity = static_cast<const std::uint8_t *>(mapped) +
                    slot.planes[0].offset + static_cast<std::size_t>(y) *
                    slot.planes[0].footprint.Footprint.RowPitch;
                const auto *gradient = static_cast<const std::uint8_t *>(mapped) +
                    slot.planes[1].offset + static_cast<std::size_t>(y) *
                    slot.planes[1].footprint.Footprint.RowPitch;
                const auto *source = static_cast<const std::uint8_t *>(mapped) +
                    slot.planes[2].offset + static_cast<std::size_t>(y) *
                    slot.planes[2].footprint.Footprint.RowPitch;
                const auto *depth = static_cast<const std::uint8_t *>(mapped) +
                    slot.planes[3].offset + static_cast<std::size_t>(y) *
                    slot.planes[3].footprint.Footprint.RowPitch;
                for (std::uint32_t x = 0; x < impl_->width; ++x) {
                    const auto *encoded = reinterpret_cast<const std::uint32_t *>(identity) + x * 4;
                    const auto *derivatives = reinterpret_cast<const float *>(gradient) + x * 4;
                    const auto *sample = reinterpret_cast<const float *>(source) + x * 4;
                    auto &pixel = frame.pixels().at(x, y);
                    pixel.material_id = static_cast<std::uint64_t>(encoded[0]) |
                        (static_cast<std::uint64_t>(encoded[1]) << 32);
                    std::memcpy(&pixel.u, encoded + 2, sizeof(float));
                    std::memcpy(&pixel.v, encoded + 3, sizeof(float));
                    pixel.confidence = pixel.material_id == 0 ? 0.0f : 1.0f;
                    pixel.du_dx = derivatives[0]; pixel.du_dy = derivatives[1];
                    pixel.dv_dx = derivatives[2]; pixel.dv_dy = derivatives[3];
                    pixel.source_r = sample[0]; pixel.source_g = sample[1];
                    pixel.source_b = sample[2]; pixel.source_a = sample[3];
                    pixel.framebuffer_depth = reinterpret_cast<const float *>(depth)[x];
                    pixel.hit_depth = pixel.framebuffer_depth;
                }
            }
            D3D12_RANGE written {0, 0};
            slot.buffer->Unmap(0, &written);
            result = std::move(frame);
        }
        slot.fence_value = 0;
    }

    auto &slot = impl_->readback[impl_->next_readback];
    if (slot.fence_value != 0) {
        ++impl_->statistics.dropped_frames;
        return result;
    }
    std::array<D3D12_RESOURCE_BARRIER, 4> barriers {};
    for (std::size_t plane = 0; plane < barriers.size(); ++plane) {
        barriers[plane].Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        barriers[plane].Transition.pResource = impl_->targets[plane];
        barriers[plane].Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        barriers[plane].Transition.StateBefore = D3D12_RESOURCE_STATE_RENDER_TARGET;
        barriers[plane].Transition.StateAfter = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    for (std::size_t plane = 0; plane < impl_->targets.size(); ++plane) {
        D3D12_TEXTURE_COPY_LOCATION source {};
        source.pResource = impl_->targets[plane];
        source.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
        D3D12_TEXTURE_COPY_LOCATION destination {};
        destination.pResource = slot.buffer;
        destination.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
        destination.PlacedFootprint = slot.planes[plane].footprint;
        commands->CopyTextureRegion(&destination, 0, 0, 0, &source, nullptr);
        std::swap(barriers[plane].Transition.StateBefore, barriers[plane].Transition.StateAfter);
    }
    commands->ResourceBarrier(static_cast<UINT>(barriers.size()), barriers.data());
    impl_->clear(commands);
    slot.frame_index = impl_->next_frame++;
    slot.fence_value = impl_->next_fence++;
    impl_->pending_signal = std::max(impl_->pending_signal, slot.fence_value);
    impl_->next_readback = (impl_->next_readback + 1) % impl_->readback.size();
    if (impl_->api_queue != nullptr) {
        impl_->api_queue->flush_immediate_command_list();
        if (!signal_submitted()) {
            slot.fence_value = 0;
            impl_->pending_signal = 0;
            ++impl_->statistics.dropped_frames;
        }
    } else if (impl_->native_queue == nullptr) {
        ++impl_->statistics.dropped_frames;
    }
    return result;
}

capture::CaptureStatistics SurfaceCapture::statistics() const noexcept {
    if (impl_ == nullptr) return {};
    std::lock_guard lock(impl_->mutex);
    return impl_->statistics;
}
std::uint32_t SurfaceCapture::width() const noexcept {
    if (impl_ == nullptr) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->width;
}
std::uint32_t SurfaceCapture::height() const noexcept {
    if (impl_ == nullptr) return 0;
    std::lock_guard lock(impl_->mutex);
    return impl_->height;
}

void SurfaceCapture::queue_replacement(std::uint64_t material_id,
                                       std::vector<capture::ReplacementMip> mips) {
    if (impl_ != nullptr && material_id != 0 && !mips.empty())
    {
        std::lock_guard lock(impl_->mutex);
        auto &entry = impl_->replacements[material_id];
        entry.mips = std::move(mips);
        entry.dirty = true;
    }
}
void SurfaceCapture::clear_replacements() {
    if (impl_ != nullptr) {
        std::lock_guard lock(impl_->mutex);
        if (impl_->api_queue != nullptr) impl_->api_queue->wait_idle();
        impl_->release_replacements();
    }
}

} // namespace neuralpass::d3d12_capture
