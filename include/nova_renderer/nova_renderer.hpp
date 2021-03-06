#pragma once

#include <rx/core/log.h>
#include <unordered_map>
#include  <optional>
#include <rx/core/ptr.h>

#include "nova_renderer/camera.hpp"
#include "nova_renderer/constants.hpp"
#include "nova_renderer/filesystem/virtual_filesystem.hpp"
#include "nova_renderer/nova_settings.hpp"
#include "nova_renderer/per_frame_device_array.hpp"
#include "nova_renderer/procedural_mesh.hpp"
#include "nova_renderer/renderables.hpp"
#include "nova_renderer/renderdoc_app.h"
#include "nova_renderer/rendergraph.hpp"
#include "nova_renderer/resource_loader.hpp"
#include "nova_renderer/rhi/forward_decls.hpp"
#include "nova_renderer/rhi/render_device.hpp"
#include "nova_renderer/util/container_accessor.hpp"

#include "../../src/renderer/material_data_buffer.hpp"

namespace rx {
    namespace memory {
        struct bump_point_allocator;
    }
} // namespace rx

void init_rex();
void rex_fini();

namespace spirv_cross {
    class CompilerGLSL;
    struct Resource;
} // namespace spirv_cross

namespace nova::renderer {
    using LogHandles = std::vector<rx::log::queue_event::handle>;

    /*!
     * \brief Registers a log message writing function
     *
     * This function removes any previously registered logging handler, replacing it with the provided function
     *
     * If you don't call this function, Nova will send all log messages to `stdout`
     *
     * You may manually unregister your handlers by calling `LogHandles::clear()`, but you don't need to. This function is intentionally not
     * marked `[[nodiscard]]` because doing things with the handles is completely optional
     */
    template <typename LogHandlerFunc>
    LogHandles& set_logging_handler(LogHandlerFunc&& log_handler);

    class UiRenderpass;

    namespace rhi {
        class Swapchain;
    }

#pragma region Runtime optimized data
    struct Mesh {
        rhi::RhiBuffer* vertex_buffer = nullptr;
        rhi::RhiBuffer* index_buffer = nullptr;

        uint32_t num_indices = 0;
        size_t num_vertex_attributes{};
    };
#pragma endregion

    using ProceduralMeshAccessor = MapAccessor<MeshId, ProceduralMesh>;

    /*!
     * \brief Main class for Nova. Owns all of Nova's resources and provides a way to access them
     * This class exists as a singleton so it's always available
     */
    class NovaRenderer {
    public:
        /*!
         * \brief Initializes the Nova Renderer
         */
        explicit NovaRenderer(const NovaSettings& settings);

        NovaRenderer(NovaRenderer&& other) noexcept = delete;
        NovaRenderer& operator=(NovaRenderer&& other) noexcept = delete;

        NovaRenderer(const NovaRenderer& other) = delete;
        NovaRenderer& operator=(const NovaRenderer& other) = delete;

        ~NovaRenderer();

        /*!
         * \brief Loads the renderpack with the given name
         *
         * This method will first try to load from the `renderpacks/` folder. If Nova can't find the renderpack there, it will try to load
         * it from the `shaderpacks/` directory (mimicking Optifine shaders). If the renderpack isn't found there, it'll try to load it from
         * the `resourcepacks/` directory (mimicking Bedrock shaders)
         *
         * Loading a renderpack will cause a stall in the GPU. Nova will have to wait for all in-flight frames to finish, then replace the
         * current renderpack with the new one, then start rendering. Replacing the renderpack might also require reloading all chunks, if
         * the new renderpack has different geometry filters then the current one
         *
         * \param renderpack_name The name of the renderpack to load
         */
        void load_renderpack(const std::string& renderpack_name);

        /*!
         * \brief Gives Nova a function to use to render UI
         *
         * This function will be executed inside the builtin UI renderpass. This renderpass takes the output of the 3D renderer, adds the UI
         * on top of it, and writes that all to the backbuffer
         *
         * The first parameter to the function is the command list it must record UI rendering into, and the second parameter is the
         * rendering context for the current frame
         *
         * Before calling the UI render function, Nova records commands to begin a renderpass with one RGBA8 color attachment and one D24S8
         * depth/stencil attachment. After calling this function, Nova records commands to end that same renderpass. This allows the host
         * application to only care about rendering the UI, instead of worrying about any pass scheduling concerns
         *
         * \return A pointer to the newly created renderpass
         */
        template <typename RenderpassType, typename... Args>
        RenderpassType* create_ui_renderpass(Args&&... args);

        [[nodiscard]] const std::vector<MaterialPass>& get_material_passes_for_pipeline(const std::string& pipeline);

        [[nodiscard]] std::optional<RenderpassMetadata> get_renderpass_metadata(const std::string& renderpass_name) const;

        /*!
         * \brief Executes a single frame
         */
        void execute_frame();

        [[nodiscard]] NovaSettingsAccessManager& get_settings();

        [[nodiscard]] rx::memory::allocator& get_global_allocator() const;

#pragma region Meshes
        /*!
         * \brief Tells Nova how many meshes you expect to have in your scene
         *
         * Allows the Nova Renderer to preallocate space for your meshes
         *
         * \param num_meshes The number of meshes you expect to have
         */
        void set_num_meshes(uint32_t num_meshes);

        /*!
         * \brief Creates a new mesh and uploads its data to the GPU, returning the ID of the newly created mesh
         *
         * \param mesh_data The mesh's initial data
         */
        [[nodiscard]] MeshId create_mesh(const MeshData& mesh_data);

        /*!
         * \brief Creates a procedural mesh, returning both its mesh id and
         */
        [[nodiscard]] ProceduralMeshAccessor create_procedural_mesh(uint64_t vertex_size, uint64_t index_size);

        [[nodiscard]] std::optional<Mesh> get_mesh(MeshId mesh);

        /*!
         * \brief Destroys the mesh with the provided ID, freeing up whatever VRAM it was using
         *
         * In debug builds, this method checks that no renderables are using the mesh
         *
         * \param mesh_to_destroy The handle of the mesh you want to destroy
         */
        void destroy_mesh(MeshId mesh_to_destroy);
#pragma endregion

#pragma region Resources
        [[nodiscard]] rhi::RhiSampler* get_point_sampler() const;
#pragma endregion

#pragma region Materials
        /*!
         * \brief Creates a new material of the specified type
         *
         * \return A pointer to the new material, or nullptr if the material can't be created for whatever reason
         */
        template <typename MaterialType>
        [[nodiscard]] std::pair<uint32_t, MaterialType*> create_material();

        /*!
         * \brief Gets the pipeline with the provided name
         *
         * \param pipeline_name Name of the pipeline to find
         *
         * \return The pipeline if it exists, or nullptr if it does not
         */
        [[nodiscard]] Pipeline* find_pipeline(const std::string& pipeline_name);

#pragma endregion

        [[nodiscard]] RenderableId add_renderable_for_material(const FullMaterialPassName& material_name,
                                                               const StaticMeshRenderableCreateInfo& create_info);

        /*!
         * \brief Updates a renderable's information
         *
         * \param renderable The renderable to update
         * \param update_data The new data for the renderable
         */
        void update_renderable(RenderableId renderable, const StaticMeshRenderableUpdateData& update_data);

        [[nodiscard]] CameraAccessor create_camera(const CameraCreateInfo& create_info);

        [[nodiscard]] rhi::RenderDevice& get_device() const;

        [[nodiscard]] NovaWindow& get_window() const;

        [[nodiscard]] DeviceResources& get_resource_manager() const;

    private:
        NovaSettingsAccessManager settings;

        std::unique_ptr<rhi::RenderDevice> device;
        std::unique_ptr<NovaWindow> window;
        rhi::Swapchain* swapchain;

        RENDERDOC_API_1_3_0* render_doc;

        rhi::RhiSampler* point_sampler;

        MeshId fullscreen_triangle_id;

        std::unique_ptr<DeviceResources> device_resources;

        rhi::RhiDescriptorPool* global_descriptor_pool;

        void* staging_buffer_memory_ptr;

#pragma region Initialization
        void create_global_allocators();

        static void initialize_virtual_filesystem();

        void create_global_sync_objects();

        void create_global_samplers();

        void create_resource_storage();

        void create_builtin_render_targets();

        void create_builtin_uniform_buffers();

        void create_builtin_meshes();

        void create_renderpass_manager();

        // MUST be called when the swapchain size changes
        void create_builtin_renderpasses();

        void create_builtin_pipelines();
#pragma endregion

#pragma region Renderpack
        struct PipelineReturn {
            Pipeline pipeline;
            PipelineMetadata metadata;
        };

        bool renderpacks_loaded = false;

        std::mutex renderpacks_loading_mutex;

        std::optional<renderpack::RenderpackData> loaded_renderpack;

        std::unique_ptr<Rendergraph> rendergraph;
#pragma endregion

#pragma region Rendergraph
        std::unordered_map<std::string, rhi::RhiImage*> builtin_images;
        std::unordered_map<std::string, renderer::Renderpass*> builtin_renderpasses;

        std::unordered_map<std::string, renderpack::TextureCreateInfo> dynamic_texture_infos;

        void create_dynamic_textures(const std::vector<renderpack::TextureCreateInfo>& texture_create_infos);

        void create_render_passes(const std::vector<renderpack::RenderPassCreateInfo>& pass_create_infos,
                                  const std::vector<renderpack::PipelineData>& pipelines) const;

        void destroy_dynamic_resources();

        void destroy_renderpasses();
#pragma endregion

#pragma region Rendering pipelines
        /*!
         * \brief Map from pipeline name to all the material passes that use that pipeline
         */
        std::unordered_map<std::string, std::vector<MaterialPass>> passes_by_pipeline;

        std::unordered_map<FullMaterialPassName, MaterialPassMetadata> material_metadatas;

        void create_pipelines_and_materials(const std::vector<renderpack::PipelineData>& pipeline_create_infos,
                                            const std::vector<renderpack::MaterialData>& materials);

        void create_materials_for_pipeline(const renderer::Pipeline& pipeline,
                                           const std::vector<renderpack::MaterialData>& materials,
                                           const std::string& pipeline_name);

        void destroy_pipelines();

        void destroy_materials();
#pragma endregion

#pragma region Meshes
        MeshId next_mesh_id = 0;

        std::unordered_map<MeshId, Mesh> meshes;
        std::unordered_map<MeshId, ProceduralMesh> proc_meshes;
#pragma endregion

#pragma region Rendering
        uint64_t frame_count = 0;
        uint8_t cur_frame_idx = 0;

        std::vector<std::string> builtin_buffer_names;
        uint32_t cur_model_matrix_index = 0;

        std::vector<rhi::RhiFence*> frame_fences;

        std::unordered_map<FullMaterialPassName, MaterialPassKey> material_pass_keys;
        std::unordered_map<std::string, Pipeline> pipelines;

        std::unique_ptr<MaterialDataBuffer> material_buffer;
        std::vector<BufferResourceAccessor> material_device_buffers;

        struct RenderableKey {
            std::string pipeline_name{};
            uint32_t material_pass_idx{};
            RenderableType type{};
            uint32_t batch_idx{};
            uint32_t renderable_idx{};
        };

        std::unordered_map<RenderableId, RenderableKey> renderable_keys;

        std::vector<Camera> cameras;
        std::unique_ptr<PerFrameDeviceArray<CameraUboData>> camera_data;

        void update_camera_matrix_buffer(uint32_t frame_idx);

        std::vector<rhi::RhiImage*> get_all_images();
#pragma endregion
    };

    template <typename RenderpassType, typename... Args>
    RenderpassType* NovaRenderer::create_ui_renderpass(Args&&... args) {
        return rendergraph->create_renderpass<RenderpassType>(*device_resources, std::forward<Args>(args)...);
    }

    template <typename MaterialType>
    std::pair<uint32_t, MaterialType*> NovaRenderer::create_material() {
        // We need to return the index so that we can send the index to the shader
        // WE NEED TO RETURN THE INDEX, NOT JUST A POINTER TO THE MATERIAL
        const auto idx = material_buffer->get_next_free_index<MaterialType>();
        return {idx, &material_buffer->at<MaterialType>(idx)};
    }
} // namespace nova::renderer
