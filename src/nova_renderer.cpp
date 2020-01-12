#include "nova_renderer/nova_renderer.hpp"

#include <array>
#include <future>

#pragma warning(push, 0)
#include <glm/ext.hpp>
#include <glm/glm.hpp>
#include <minitrace.h>
#include <spirv_glsl.hpp>
#pragma warning(pop)

#include <glslang/MachineIndependent/Initialize.h>

#include "nova_renderer/constants.hpp"
#include "nova_renderer/frontend/procedural_mesh.hpp"
#include "nova_renderer/frontend/rendergraph.hpp"
#include "nova_renderer/frontend/ui_renderer.hpp"
#include "nova_renderer/loading/shaderpack_loading.hpp"
#include "nova_renderer/memory/block_allocation_strategy.hpp"
#include "nova_renderer/memory/bump_point_allocation_strategy.hpp"
#include "nova_renderer/rhi/command_list.hpp"
#include "nova_renderer/rhi/swapchain.hpp"
#include "nova_renderer/util/logger.hpp"
#include "nova_renderer/util/platform.hpp"

#include "debugging/renderdoc.hpp"
#include "filesystem/shaderpack/render_graph_builder.hpp"
#include "render_objects/uniform_structs.hpp"
// D3D12 MUST be included first because the Vulkan include undefines FAR, yet the D3D12 headers need FAR
// Windows considered harmful
#if defined(NOVA_WINDOWS) && defined(NOVA_D3D12_RHI)
#include "render_engine/dx12/dx12_render_engine.hpp"
#endif

#if defined(NOVA_VULKAN_RHI)
#include "render_engine/vulkan/vulkan_render_engine.hpp"
#endif

using namespace nova::mem;
using namespace operators;
using namespace fmt;

const Bytes global_memory_pool_size = 1_gb;

namespace nova::renderer {
    std::unique_ptr<NovaRenderer> NovaRenderer::instance;

    bool FullMaterialPassName::operator==(const FullMaterialPassName& other) const {
        return material_name == other.material_name && pass_name == other.pass_name;
    }

    std::size_t FullMaterialPassNameHasher::operator()(const FullMaterialPassName& name) const {
        const std::size_t material_name_hash = std::hash<std::string>()(name.material_name);
        const std::size_t pass_name_hash = std::hash<std::string>()(name.pass_name);

        return material_name_hash ^ pass_name_hash;
    }

    NovaRenderer::NovaRenderer(const NovaSettings& settings) : render_settings(settings) {
        create_global_allocators();

        initialize_virtual_filesystem();

        mtr_init("trace.json");

        MTR_META_PROCESS_NAME("NovaRenderer");
        MTR_META_THREAD_NAME("Main");

        MTR_SCOPE("Init", "nova_renderer::nova_renderer");

        window = std::make_unique<NovaWindow>(settings);

        if(settings.debug.renderdoc.enabled) {
            MTR_SCOPE("Init", "LoadRenderdoc");
            auto rd_load_result = load_renderdoc(settings.debug.renderdoc.renderdoc_dll_path);

            rd_load_result
                .map([&](RENDERDOC_API_1_3_0* api) {
                    render_doc = api;

                    render_doc->SetCaptureFilePathTemplate(settings.debug.renderdoc.capture_path);

                    RENDERDOC_InputButton capture_key[] = {eRENDERDOC_Key_F12, eRENDERDOC_Key_PrtScrn};
                    render_doc->SetCaptureKeys(capture_key, 2);

                    render_doc->SetCaptureOptionU32(eRENDERDOC_Option_AllowFullscreen, 1U);
                    render_doc->SetCaptureOptionU32(eRENDERDOC_Option_AllowVSync, 1U);
                    render_doc->SetCaptureOptionU32(eRENDERDOC_Option_VerifyMapWrites, 1U);
                    render_doc->SetCaptureOptionU32(eRENDERDOC_Option_SaveAllInitials, 1U);
                    render_doc->SetCaptureOptionU32(eRENDERDOC_Option_APIValidation, 1U);

                    NOVA_LOG(INFO) << "Loaded RenderDoc successfully";

                    return 0;
                })
                .on_error([](const ntl::NovaError& error) { NOVA_LOG(ERROR) << error.to_string().c_str(); });
        }

        switch(settings.api) {
            case GraphicsApi::D3D12:
#if defined(NOVA_WINDOWS) && defined(NOVA_D3D12_RHI)
            {
                MTR_SCOPE("Init", "InitDirect3D12RenderEngine");
                rhi = std::make_unique<rhi::D3D12RenderEngine>(render_settings, *window, *global_allocator);
            } break;
#endif

#if defined(NOVA_VULKAN_RHI)
            case GraphicsApi::Vulkan: {
                MTR_SCOPE("Init", "InitVulkanRenderEngine");
                rhi = std::make_unique<rhi::VulkanRenderEngine>(render_settings, *window, *global_allocator);
            } break;
#endif

            default: {
                // TODO: Deal with it in a better way, this will crash soon
                NOVA_LOG(FATAL) << "Selected graphics API was not enabled!";
            } break;
        }

        swapchain = rhi->get_swapchain();

        create_global_gpu_pools();

        create_global_sync_objects();

        create_resource_storage();

        create_builtin_render_targets();

        create_uniform_buffers();

        create_renderpass_manager();

        create_builtin_renderpasses();
    }

    NovaRenderer::~NovaRenderer() { mtr_shutdown(); }

    NovaSettingsAccessManager& NovaRenderer::get_settings() { return render_settings; }

    AllocatorHandle<>* NovaRenderer::get_global_allocator() const { return global_allocator.get(); }

    void NovaRenderer::execute_frame() {
        MTR_SCOPE("RenderLoop", "execute_frame");
        frame_count++;

        AllocatorHandle<>& frame_allocator = frame_allocators[frame_count % NUM_IN_FLIGHT_FRAMES];

        cur_frame_idx = rhi->get_swapchain()->acquire_next_swapchain_image(frame_allocator);

        NOVA_LOG(DEBUG) << "\n***********************\n        FRAME START        \n***********************";

        rhi->reset_fences({frame_fences.at(cur_frame_idx)});

        rhi::CommandList* cmds = rhi->create_command_list(frame_allocator, 0, rhi::QueueType::Graphics);

        // This may or may not work well lmao
        for(auto& [id, proc_mesh] : proc_meshes) {
            proc_mesh.record_commands_to_upload_data(cmds, cur_frame_idx);
        }

        FrameContext ctx = {};
        ctx.frame_count = frame_count;
        ctx.nova = this;
        ctx.allocator = &frame_allocator;
        ctx.swapchain_framebuffer = swapchain->get_framebuffer(cur_frame_idx);
        ctx.swapchain_image = swapchain->get_image(cur_frame_idx);

        const auto& renderpass_order = rendergraph->calculate_renderpass_execution_order();

        for(const auto& renderpass_name : renderpass_order) {
            auto* renderpass = rendergraph->get_renderpass(renderpass_name);
            renderpass->render(*cmds, ctx);
        }

        rhi->submit_command_list(cmds, rhi::QueueType::Graphics, frame_fences.at(cur_frame_idx));

        // Wait for the GPU to finish before presenting. This destroys pipelining and throughput, however at this time I'm not sure how best
        // to say "when GPU finishes this task, CPU should do something"
        rhi->wait_for_fences({frame_fences.at(cur_frame_idx)});

        rhi->get_swapchain()->present(cur_frame_idx);

        mtr_flush();
    }

    void NovaRenderer::set_num_meshes(const uint32_t num_meshes) { meshes.reserve(num_meshes); }

    MeshId NovaRenderer::create_mesh(const MeshData& mesh_data) {
        rhi::BufferCreateInfo vertex_buffer_create_info;
        vertex_buffer_create_info.buffer_usage = rhi::BufferUsage::VertexBuffer;
        vertex_buffer_create_info.size = mesh_data.vertex_data.size() * sizeof(FullVertex);

        rhi::Buffer* vertex_buffer = rhi->create_buffer(vertex_buffer_create_info, *mesh_memory, *global_allocator);

        // TODO: Try to get staging buffers from a pool

        {
            rhi::BufferCreateInfo staging_vertex_buffer_create_info = vertex_buffer_create_info;
            staging_vertex_buffer_create_info.buffer_usage = rhi::BufferUsage::StagingBuffer;

            rhi::Buffer* staging_vertex_buffer = rhi->create_buffer(staging_vertex_buffer_create_info,
                                                                    *staging_buffer_memory,
                                                                    *global_allocator);
            rhi->write_data_to_buffer(mesh_data.vertex_data.data(),
                                      mesh_data.vertex_data.size() * sizeof(FullVertex),
                                      0,
                                      staging_vertex_buffer);

            rhi::CommandList* vertex_upload_cmds = rhi->create_command_list(*global_allocator, 0, rhi::QueueType::Transfer);
            vertex_upload_cmds->copy_buffer(vertex_buffer, 0, staging_vertex_buffer, 0, vertex_buffer_create_info.size);

            rhi::ResourceBarrier vertex_barrier = {};
            vertex_barrier.resource_to_barrier = vertex_buffer;
            vertex_barrier.old_state = rhi::ResourceState::CopyDestination;
            vertex_barrier.new_state = rhi::ResourceState::Common;
            vertex_barrier.access_before_barrier = rhi::ResourceAccess::CopyWrite;
            vertex_barrier.access_after_barrier = rhi::ResourceAccess::VertexAttributeRead;
            vertex_barrier.buffer_memory_barrier.offset = 0;
            vertex_barrier.buffer_memory_barrier.size = vertex_buffer->size;

            vertex_upload_cmds->resource_barriers(rhi::PipelineStage::Transfer, rhi::PipelineStage::VertexInput, {vertex_barrier});

            rhi->submit_command_list(vertex_upload_cmds, rhi::QueueType::Transfer);

            // TODO: Barrier on the mesh's first usage
        }

        rhi::BufferCreateInfo index_buffer_create_info;
        index_buffer_create_info.buffer_usage = rhi::BufferUsage::IndexBuffer;
        index_buffer_create_info.size = mesh_data.indices.size() * sizeof(uint32_t);

        rhi::Buffer* index_buffer = rhi->create_buffer(index_buffer_create_info, *mesh_memory, *global_allocator);

        {
            rhi::BufferCreateInfo staging_index_buffer_create_info = index_buffer_create_info;
            staging_index_buffer_create_info.buffer_usage = rhi::BufferUsage::StagingBuffer;
            rhi::Buffer* staging_index_buffer = rhi->create_buffer(staging_index_buffer_create_info,
                                                                   *staging_buffer_memory,
                                                                   *global_allocator);
            rhi->write_data_to_buffer(mesh_data.indices.data(), mesh_data.indices.size() * sizeof(uint32_t), 0, staging_index_buffer);

            rhi::CommandList* indices_upload_cmds = rhi->create_command_list(*global_allocator, 0, rhi::QueueType::Transfer);
            indices_upload_cmds->copy_buffer(index_buffer, 0, staging_index_buffer, 0, index_buffer_create_info.size);

            rhi::ResourceBarrier index_barrier = {};
            index_barrier.resource_to_barrier = index_buffer;
            index_barrier.old_state = rhi::ResourceState::CopyDestination;
            index_barrier.new_state = rhi::ResourceState::Common;
            index_barrier.access_before_barrier = rhi::ResourceAccess::CopyWrite;
            index_barrier.access_after_barrier = rhi::ResourceAccess::IndexRead;
            index_barrier.buffer_memory_barrier.offset = 0;
            index_barrier.buffer_memory_barrier.size = index_buffer->size;

            indices_upload_cmds->resource_barriers(rhi::PipelineStage::Transfer, rhi::PipelineStage::VertexInput, {index_barrier});

            rhi->submit_command_list(indices_upload_cmds, rhi::QueueType::Transfer);

            // TODO: Barrier on the mesh's first usage
        }

        // TODO: Clean up staging buffers

        Mesh mesh;
        mesh.vertex_buffer = vertex_buffer;
        mesh.index_buffer = index_buffer;
        mesh.num_indices = static_cast<uint32_t>(mesh_data.indices.size());

        MeshId new_mesh_id = next_mesh_id;
        next_mesh_id++;
        meshes.emplace(new_mesh_id, mesh);

        return new_mesh_id;
    }

    ProceduralMeshAccessor NovaRenderer::create_procedural_mesh(const uint64_t vertex_size, const uint64_t index_size) {
        MeshId our_id = next_mesh_id;
        next_mesh_id++;

        proc_meshes.emplace(our_id, ProceduralMesh(vertex_size, index_size, rhi.get()));

        return ProceduralMeshAccessor(&proc_meshes, our_id);
    }

    void NovaRenderer::load_renderpack(const std::string& renderpack_name) {
        MTR_SCOPE("ShaderpackLoading", "load_shaderpack");
        glslang::InitializeProcess();

        const shaderpack::RenderpackData data = shaderpack::load_shaderpack_data(fs::path(renderpack_name));

        if(shaderpack_loaded) {
            destroy_dynamic_resources();

            destroy_renderpasses();
            NOVA_LOG(DEBUG) << "Resources from old shaderpacks destroyed";
        }

        create_dynamic_textures(data.resources.render_targets);
        NOVA_LOG(DEBUG) << "Dynamic textures created";

        create_render_passes(data.graph_data.passes, data.pipelines);

        NOVA_LOG(DEBUG) << "Created render passes";

        create_pipelines_and_materials(data.pipelines, data.materials);

        NOVA_LOG(DEBUG) << "Created pipelines and materials";

        shaderpack_loaded = true;

        NOVA_LOG(INFO) << "Shaderpack " << renderpack_name << " loaded successfully";
    }

    const std::vector<MaterialPass>& NovaRenderer::get_material_passes_for_pipeline(rhi::Pipeline* const pipeline) {
        return passes_by_pipeline.at(pipeline);
    }

    std::optional<RenderpassMetadata> NovaRenderer::get_renderpass_metadata(const std::string& renderpass_name) const {
        return rendergraph->get_metadata_for_renderpass(renderpass_name);
    }

    void NovaRenderer::create_dynamic_textures(const std::pmr::vector<shaderpack::TextureCreateInfo>& texture_create_infos) {
        for(const shaderpack::TextureCreateInfo& create_info : texture_create_infos) {
            const auto size = create_info.format.get_size_in_pixels(rhi->get_swapchain()->get_size());

            const auto render_target = device_resources->create_render_target(create_info.name,
                                                                              size.x,
                                                                              size.y,
                                                                              to_rhi_pixel_format(create_info.format.pixel_format),
                                                                              *renderpack_allocator);

            dynamic_texture_infos.emplace(create_info.name, create_info);
        }
    }

    void NovaRenderer::create_render_passes(const std::pmr::vector<shaderpack::RenderPassCreateInfo>& pass_create_infos,
                                            const std::pmr::vector<shaderpack::PipelineCreateInfo>& pipelines) const {

        rhi->set_num_renderpasses(static_cast<uint32_t>(pass_create_infos.size()));

        for(const shaderpack::RenderPassCreateInfo& create_info : pass_create_infos) {
            std::unique_ptr<Renderpass> renderpass = std::make_unique<Renderpass>(create_info.name);
            if(auto* pass = rendergraph->add_renderpass(std::move(renderpass), create_info, *device_resources); pass != nullptr) {
                for(const auto& pipeline : pipelines) {
                    if(pipeline.pass == create_info.name) {
                        pass->pipeline_names.emplace_back(pipeline.name);
                    }
                }
            } else {
                NOVA_LOG(ERROR) << "Could not create renderpass " << create_info.name;
            }
        }
    }

    void NovaRenderer::create_pipelines_and_materials(const std::pmr::vector<shaderpack::PipelineCreateInfo>& pipeline_create_infos,
                                                      const std::pmr::vector<shaderpack::MaterialData>& materials) {
        uint32_t total_num_descriptors = 0;
        for(const shaderpack::MaterialData& material_data : materials) {
            for(const shaderpack::MaterialPass& material_pass : material_data.passes) {
                total_num_descriptors += static_cast<uint32_t>(material_pass.bindings.size());
            }
        }

        if(total_num_descriptors > 0) {
            global_descriptor_pool = rhi->create_descriptor_pool(total_num_descriptors, 5, total_num_descriptors, *renderpack_allocator);
        }

        for(const auto& pipeline_create_info : pipeline_create_infos) {
            if(pipeline_storage->create_pipeline(pipeline_create_info)) {
                auto pipeline = pipeline_storage->get_pipeline(pipeline_create_info.name);
                create_materials_for_pipeline(*pipeline, materials, pipeline_create_info.name);
            }
        }
    }

    void NovaRenderer::create_materials_for_pipeline(const Pipeline& pipeline,
                                                     const std::pmr::vector<shaderpack::MaterialData>& materials,
                                                     const std::string& pipeline_name) {

        // Determine the pipeline layout so the material can create descriptors for the pipeline

        MaterialPassKey template_key = {};
        template_key.pipeline_name = pipeline_name;

        // Large overestimate, but that's fine
        std::vector<MaterialPass> passes;
        passes.reserve(materials.size());

        for(const shaderpack::MaterialData& material_data : materials) {
            for(const shaderpack::MaterialPass& pass_data : material_data.passes) {
                if(pass_data.pipeline == pipeline_name) {
                    MaterialPass pass = {};
                    pass.pipeline_interface = pipeline.pipeline_interface;

                    pass.descriptor_sets = rhi->create_descriptor_sets(pipeline.pipeline_interface,
                                                                       global_descriptor_pool,
                                                                       *renderpack_allocator);

                    bind_data_to_material_descriptor_sets(pass, pass_data.bindings, pipeline.pipeline_interface->bindings);

                    FullMaterialPassName full_pass_name = {pass_data.material_name, pass_data.name};

                    MaterialPassMetadata pass_metadata{};
                    pass_metadata.data = pass_data;
                    material_metadatas.emplace(full_pass_name, pass_metadata);

                    MaterialPassKey key = template_key;
                    key.material_pass_index = static_cast<uint32_t>(passes.size());

                    material_pass_keys.emplace(full_pass_name, key);

                    passes.push_back(pass);
                }
            }
        }

        passes.shrink_to_fit();

        passes_by_pipeline.emplace(pipeline.pipeline, passes);
    }

    void NovaRenderer::bind_data_to_material_descriptor_sets(
        const MaterialPass& material,
        const std::unordered_map<std::string, std::string>& bindings,
        const std::unordered_map<std::string, rhi::ResourceBindingDescription>& descriptor_descriptions) {

        std::pmr::vector<rhi::DescriptorSetWrite> writes;
        writes.reserve(bindings.size());

        for(const auto& [descriptor_name, resource_name] : bindings) {
            const rhi::ResourceBindingDescription& binding_desc = descriptor_descriptions.at(descriptor_name);
            rhi::DescriptorSet* descriptor_set = material.descriptor_sets.at(binding_desc.set);

            rhi::DescriptorSetWrite write = {};
            write.set = descriptor_set;
            write.binding = binding_desc.binding;
            write.resources.emplace_back();
            rhi::DescriptorResourceInfo& resource_info = write.resources[0];

            if(const auto dyn_tex = device_resources->get_render_target(resource_name); dyn_tex) {
                rhi::Image* image = (*dyn_tex)->image;

                resource_info.image_info.image = image;
                resource_info.image_info.sampler = point_sampler;
                resource_info.image_info.format = dynamic_texture_infos.at(resource_name).format;

                write.type = rhi::DescriptorType::CombinedImageSampler;

                writes.push_back(write);

            } else if(const auto builtin_buffer_itr = builtin_buffers.find(resource_name); builtin_buffer_itr != builtin_buffers.end()) {
                rhi::Buffer* buffer = builtin_buffer_itr->second;

                resource_info.buffer_info.buffer = buffer;
                write.type = rhi::DescriptorType::UniformBuffer;

                writes.push_back(write);

            } else {
                NOVA_LOG(ERROR) << "Resource " << resource_name.c_str() << " is not known to Nova";
            }
        }

        rhi->update_descriptor_sets(writes);
    }

    void NovaRenderer::destroy_dynamic_resources() {
        if(loaded_renderpack) {
            for(const auto& tex_data : loaded_renderpack->resources.render_targets) {
                device_resources->destroy_render_target(tex_data.name, *renderpack_allocator);
            }
            NOVA_LOG(DEBUG) << "Deleted all dynamic textures from renderpack " << loaded_renderpack->name;
        }
    }

    void NovaRenderer::destroy_renderpasses() {
        for(const auto& renderpass : loaded_renderpack->graph_data.passes) {
            rendergraph->destroy_renderpass(renderpass.name);
        }
    }

    rhi::Buffer* NovaRenderer::get_builtin_buffer(const std::string& buffer_name) const { return builtin_buffers.at(buffer_name); }

    rhi::Sampler* NovaRenderer::get_point_sampler() const { return point_sampler; }

    RenderableId NovaRenderer::add_renderable_for_material(const FullMaterialPassName& material_name,
                                                           const StaticMeshRenderableData& renderable) {
        const RenderableId id = next_renderable_id.load();
        next_renderable_id.fetch_add(1);

        const auto pos = material_pass_keys.find(material_name);
        if(pos == material_pass_keys.end()) {
            NOVA_LOG(ERROR) << "No material named " << material_name.material_name << " for pass " << material_name.pass_name;
            return std::numeric_limits<uint64_t>::max();
        }

        MaterialPass material = {};

        StaticMeshRenderCommand command = make_render_command(renderable, id);

        if(const auto itr = meshes.find(renderable.mesh); itr != meshes.end()) {
            const Mesh& mesh = itr->second;

            if(renderable.is_static) {
                bool need_to_add_batch = true;

                for(MeshBatch<StaticMeshRenderCommand>& batch : material.static_mesh_draws) {
                    if(batch.vertex_buffer == mesh.vertex_buffer) {
                        batch.commands.emplace_back(command);

                        need_to_add_batch = false;
                        break;
                    }
                }

                if(need_to_add_batch) {
                    MeshBatch<StaticMeshRenderCommand> batch;
                    batch.vertex_buffer = mesh.vertex_buffer;
                    batch.index_buffer = mesh.index_buffer;
                    batch.commands.emplace_back(command);

                    material.static_mesh_draws.emplace_back(batch);
                }
            }

        } else if(const auto proc_itr = proc_meshes.find(renderable.mesh); proc_itr != proc_meshes.end()) {
            if(renderable.is_static) {
                bool need_to_add_batch = false;

                for(ProceduralMeshBatch<StaticMeshRenderCommand>& batch : material.static_procedural_mesh_draws) {
                    if(batch.mesh.get_key() == renderable.mesh) {
                        batch.commands.emplace_back(command);

                        need_to_add_batch = false;
                        break;
                    }
                }

                if(need_to_add_batch) {
                    ProceduralMeshBatch<StaticMeshRenderCommand> batch(&proc_meshes, renderable.mesh);
                    batch.commands.emplace_back(command);

                    material.static_procedural_mesh_draws.emplace_back(batch);
                }
            }
        } else {
            NOVA_LOG(ERROR) << "Could not find a mesh with ID " << renderable.mesh;
        }

        // Figure out where to put the renderable
        const MaterialPassKey& pass_key = pos->second;
        const auto pipeline = pipeline_storage->get_pipeline(pass_key.pipeline_name);
        if(pipeline) {
            auto& passes = passes_by_pipeline[pipeline->pipeline];
            passes.emplace_back(material);

        } else {
            NOVA_LOG(ERROR) << "Could not get place the new renderable in the appropriate draw command list";
        }

        return id;
    }

    rhi::RenderEngine& NovaRenderer::get_engine() const { return *rhi; }

    NovaWindow& NovaRenderer::get_window() const { return *window; }

    DeviceResources& NovaRenderer::get_resource_manager() const { return *device_resources; }

    PipelineStorage& NovaRenderer::get_pipeline_storage() const { return *pipeline_storage; }

    NovaRenderer* NovaRenderer::get_instance() { return instance.get(); }

    NovaRenderer* NovaRenderer::initialize(const NovaSettings& settings) {
        return (instance = std::make_unique<NovaRenderer>(settings)).get();
    }

    void NovaRenderer::deinitialize() { instance.reset(); }

    void NovaRenderer::create_global_allocators() {
        global_allocator = std::make_unique<AllocatorHandle<>>(std::pmr::new_delete_resource());

        renderpack_allocator = std::unique_ptr<AllocatorHandle<>>(global_allocator->create_suballocator());

        frame_allocators.reserve(NUM_IN_FLIGHT_FRAMES);
        for(size_t i = 0; i < NUM_IN_FLIGHT_FRAMES; i++) {
            void* ptr = global_allocator->allocate(PER_FRAME_MEMORY_SIZE.b_count());
            auto* mem = global_allocator->new_other_object<std::pmr::monotonic_buffer_resource>(ptr, PER_FRAME_MEMORY_SIZE.b_count());
            frame_allocators.emplace_back(mem);
        }
    }

    void NovaRenderer::initialize_virtual_filesystem() {
        // The host application MUST register its data directory before initializing Nova

        const auto vfs = filesystem::VirtualFilesystem::get_instance();

        const auto renderpacks_directory = vfs->get_folder_accessor(RENDERPACK_DIRECTORY);

        vfs->add_resource_root(renderpacks_directory);
    }

    void NovaRenderer::create_global_gpu_pools() {
        const uint64_t mesh_memory_size = 512000000;
        ntl::Result<rhi::DeviceMemory*> memory_result = rhi->allocate_device_memory(mesh_memory_size,
                                                                                    rhi::MemoryUsage::DeviceOnly,
                                                                                    rhi::ObjectType::Buffer,
                                                                                    *global_allocator);
        const ntl::Result<DeviceMemoryResource*> mesh_memory_result = memory_result.map([&](rhi::DeviceMemory* memory) {
            auto* allocator = global_allocator->new_other_object<BlockAllocationStrategy>(global_allocator.get(),
                                                                                          Bytes(mesh_memory_size),
                                                                                          64_b);
            return global_allocator->new_other_object<DeviceMemoryResource>(memory, allocator);
        });

        if(mesh_memory_result) {
            mesh_memory = std::make_unique<DeviceMemoryResource>(*mesh_memory_result.value);

        } else {
            NOVA_LOG(ERROR) << "Could not create mesh memory pool: " << mesh_memory_result.error.to_string().c_str();
        }

        // Assume 65k things, plus we need space for the builtin ubos
        const uint64_t ubo_memory_size = sizeof(PerFrameUniforms) + sizeof(glm::mat4) * 0xFFFF;
        const ntl::Result<DeviceMemoryResource*>
            ubo_memory_result = rhi->allocate_device_memory(ubo_memory_size,
                                                            rhi::MemoryUsage::DeviceOnly,
                                                            rhi::ObjectType::Buffer,
                                                            *global_allocator)
                                    .map([=](rhi::DeviceMemory* memory) {
                                        auto* allocator = global_allocator
                                                              ->new_other_object<BumpPointAllocationStrategy>(Bytes(ubo_memory_size),
                                                                                                              Bytes(sizeof(glm::mat4)));
                                        return global_allocator->new_other_object<DeviceMemoryResource>(memory, allocator);
                                    });

        if(ubo_memory_result) {
            ubo_memory = std::make_unique<DeviceMemoryResource>(*ubo_memory_result.value);

        } else {
            NOVA_LOG(ERROR) << "Could not create mesh memory pool: " << ubo_memory_result.error.to_string().c_str();
        }

        // Staging buffers will be pooled, so we don't need a _ton_ of memory for them
        const Bytes staging_memory_size = 256_kb;
        const ntl::Result<DeviceMemoryResource*>
            staging_memory_result = rhi->allocate_device_memory(staging_memory_size.b_count(),
                                                                rhi::MemoryUsage::StagingBuffer,
                                                                rhi::ObjectType::Buffer,
                                                                *global_allocator)
                                        .map([=](rhi::DeviceMemory* memory) {
                                            auto* allocator = global_allocator
                                                                  ->new_other_object<BumpPointAllocationStrategy>(staging_memory_size,
                                                                                                                  64_b);
                                            return global_allocator->new_other_object<DeviceMemoryResource>(memory, allocator);
                                        });

        if(staging_memory_result) {
            staging_buffer_memory = std::make_unique<DeviceMemoryResource>(*staging_memory_result.value);

        } else {
            NOVA_LOG(ERROR) << "Could not create staging buffer memory pool: " << staging_memory_result.error.to_string();
        }
    }

    void NovaRenderer::create_global_sync_objects() {
        const std::pmr::vector<rhi::Fence*>& fences = rhi->create_fences(*global_allocator, NUM_IN_FLIGHT_FRAMES, true);
        for(uint32_t i = 0; i < NUM_IN_FLIGHT_FRAMES; i++) {
            frame_fences[i] = fences.at(i);
        }
    }

    void NovaRenderer::create_resource_storage() {
        device_resources = std::make_unique<DeviceResources>(*this);

        pipeline_storage = std::make_unique<PipelineStorage>(*this, *global_allocator->create_suballocator());
    }

    void NovaRenderer::create_builtin_render_targets() const {
        const auto& swapchain_size = rhi->get_swapchain()->get_size();
        const auto scene_output = device_resources->create_render_target(SCENE_OUTPUT_RT_NAME,
                                                                         swapchain_size.x,
                                                                         swapchain_size.y,
                                                                         rhi::PixelFormat::Rgba8,
                                                                         *global_allocator,
                                                                         true);

        if(!scene_output) {
            NOVA_LOG(ERROR) << "Could not create scene output render target " << SCENE_OUTPUT_RT_NAME;
        }
    }

    void NovaRenderer::create_uniform_buffers() {
        // Buffer for per-frame uniform data
        rhi::BufferCreateInfo per_frame_data_create_info;
        per_frame_data_create_info.size = sizeof(PerFrameUniforms);
        per_frame_data_create_info.buffer_usage = rhi::BufferUsage::UniformBuffer;

        auto* per_frame_data_buffer = rhi->create_buffer(per_frame_data_create_info, *ubo_memory, *global_allocator);
        builtin_buffers.emplace(PER_FRAME_DATA_NAME, per_frame_data_buffer);

        // Buffer for each drawcall's model matrix
        rhi::BufferCreateInfo model_matrix_buffer_create_info;
        model_matrix_buffer_create_info.size = sizeof(glm::mat4) * 0xFFFF;
        model_matrix_buffer_create_info.buffer_usage = rhi::BufferUsage::UniformBuffer;

        auto* model_matrix_buffer = rhi->create_buffer(model_matrix_buffer_create_info, *ubo_memory, *global_allocator);
        builtin_buffers.emplace(MODEL_MATRIX_BUFFER_NAME, model_matrix_buffer);
    }

    void NovaRenderer::create_renderpass_manager() {
        rendergraph = std::make_unique<Rendergraph>(*global_allocator->create_suballocator(), *rhi);
    }

    void NovaRenderer::create_builtin_renderpasses() const {
        // UI render pass
        {
            std::unique_ptr<Renderpass> ui_renderpass = std::make_unique<NullUiRenderpass>();
            ui_renderpass->is_builtin = true;

            if(!rendergraph->add_renderpass(std::move(ui_renderpass), NullUiRenderpass::get_create_info(), *device_resources)) {
                NOVA_LOG(ERROR) << "Could not create null UI renderpass";
            }
        }
    }
} // namespace nova::renderer
