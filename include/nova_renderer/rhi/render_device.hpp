#pragma once

#include "nova_renderer/nova_settings.hpp"
#include "nova_renderer/renderpack_data.hpp"
#include "nova_renderer/rhi/command_list.hpp"
#include "nova_renderer/rhi/rhi_types.hpp"
#include "nova_renderer/util/result.hpp"
#include "nova_renderer/window.hpp"

#include "resource_binder.hpp"

namespace nova::renderer {
    struct FrameContext;
    struct RhiGraphicsPipelineState;
    struct DeviceMemoryResource;
} // namespace nova::renderer

namespace nova::renderer::rhi {

    /*!
     * \brief All the GPU architectures that Nova cares about, at whatever granularity is most useful
     */
    enum class DeviceArchitecture {
        unknown,

        /*!
         * \brief The GPU was made by AMD
         */
        amd,

        /*!
         * \brief The GPU was made by Nvidia
         */
        nvidia,

        /*!
         * \brief The GPU was made by Intel
         */
        intel,
    };

    /*!
     * \brief Information about hte capabilities and limits of the device we're running on
     */
    struct DeviceInfo {
        DeviceArchitecture architecture = DeviceArchitecture::unknown;

        mem::Bytes max_texture_size = 0;

        bool is_uma = false;

        bool supports_raytracing = false;
        bool supports_mesh_shaders = false;
    };

#define NUM_THREADS 1

    /*!
     * \brief Interface to a logical device which can render to an operating system window
     */
    class RenderDevice {
    public:
        DeviceInfo info;

        NovaSettingsAccessManager& settings;

        RenderDevice(RenderDevice&& other) = delete;
        RenderDevice& operator=(RenderDevice&& other) noexcept = delete;

        RenderDevice(const RenderDevice& other) = delete;
        RenderDevice& operator=(const RenderDevice& other) = delete;

        /*!
         * \brief Needed to make destructor of subclasses called
         */
        virtual ~RenderDevice() = default;

        virtual void set_num_renderpasses(uint32_t num_renderpasses) = 0;

        /*!
         * \brief Creates a renderpass from the provided data
         *
         * Renderpasses are created 100% upfront, meaning that the caller can't change anything about a renderpass
         * after it's been created
         *
         * \param data The data to create a renderpass from
         * \param framebuffer_size The size in pixels of the framebuffer that the renderpass will write to
         * \param allocator The allocator to allocate the renderpass from
         *
         * \return The newly created renderpass
         */
        [[nodiscard]] virtual ntl::Result<RhiRenderpass*> create_renderpass(const renderpack::RenderPassCreateInfo& data,
                                                                            const glm::uvec2& framebuffer_size) = 0;

        [[nodiscard]] virtual RhiFramebuffer* create_framebuffer(const RhiRenderpass* renderpass,
                                                                 const std::vector<RhiImage*>& color_attachments,
                                                                 const std::optional<RhiImage*> depth_attachment,
                                                                 const glm::uvec2& framebuffer_size) = 0;

        /*!
         * \brief Creates a new surface pipeline
         *
         * Surface pipelines render objects using Nova's material system. The backend does a little work to set them up so they're 100%
         * compatible with the material system. They currently can't access any resources outside of the material system, and _have_ to use
         * the standard pipeline layout
         */
        [[nodiscard]] virtual std::unique_ptr<RhiPipeline> create_surface_pipeline(const RhiGraphicsPipelineState& pipeline_state) = 0;

        /*!
         * \brief Creates a global pipeline
         *
         * Global pipelines are pipelines that aren't tied to any specific objects in the world. Global pipelines typically read render
         * targets to do something like post processing
         */
        [[nodiscard]] virtual std::unique_ptr<RhiPipeline> create_global_pipeline(const RhiGraphicsPipelineState& pipeline_state) = 0;

        [[nodiscard]] virtual std::unique_ptr<RhiResourceBinder> create_resource_binder_for_pipeline(const RhiPipeline& pipeline) = 0;

        /*!
         * \brief Creates a buffer with undefined contents
         */
        [[nodiscard]] virtual RhiBuffer* create_buffer(const RhiBufferCreateInfo& info) = 0;

        /*!
         * \brief Writes data to a buffer
         *
         * This method always writes the data from byte 0 to byte num_bytes. It does not let you use an offset for either reading from
         * the data or writing to the buffer
         *
         * The CPU must be able to write directly to the buffer for this method to work. If the buffer is device local, this method will
         * fail in a horrible way
         *
         * \param data The data to upload
         * \param num_bytes The number of bytes to write
         * \param buffer The buffer to write to
         */
        virtual void write_data_to_buffer(const void* data, mem::Bytes num_bytes, const RhiBuffer* buffer) = 0;

        /*!
         * \brief Creates a new Sampler object
         */
        [[nodiscard]] virtual RhiSampler* create_sampler(const RhiSamplerCreateInfo& create_info) = 0;

        /*!
         * \brief Creates an empty image
         *
         * The image will start out in the Undefined layout. You must transition it to whatever layout you want to use
         */
        [[nodiscard]] virtual RhiImage* create_image(const renderpack::TextureCreateInfo& info) = 0;

        [[nodiscard]] virtual RhiSemaphore* create_semaphore() = 0;

        [[nodiscard]] virtual std::vector<RhiSemaphore*> create_semaphores(uint32_t num_semaphores) = 0;

        [[nodiscard]] virtual RhiFence* create_fence(bool signaled) = 0;

        [[nodiscard]] virtual std::vector<RhiFence*> create_fences(uint32_t num_fences, bool signaled) = 0;

        /*!
         * \blocks the fence until all fences are signaled
         *
         * Fences are waited on for an infinite time
         *
         * \param fences All the fences to wait for
         */
        virtual void wait_for_fences(std::vector<RhiFence*> fences) = 0;

        virtual void reset_fences(const std::vector<RhiFence*>& fences) = 0;

        /*!
         * \brief Clean up any GPU objects a Renderpass may own
         *
         * While Renderpasses are per-renderpack objects, and their CPU memory will be cleaned up when a new renderpack is loaded, we still
         * need to clean up their GPU objects
         */
        virtual void destroy_renderpass(RhiRenderpass* pass) = 0;

        /*!
         * \brief Clean up any GPU objects a Framebuffer may own
         *
         * While Framebuffers are per-renderpack objects, and their CPU memory will be cleaned up when a new renderpack is loaded, we still
         * need to clean up their GPU objects
         */
        virtual void destroy_framebuffer(RhiFramebuffer* framebuffer) = 0;

        /*!
         * \brief Clean up any GPU objects an Image may own
         *
         * While Images are per-renderpack objects, and their CPU memory will be cleaned up when a new renderpack is loaded, we still need
         * to clean up their GPU objects
         */
        virtual void destroy_texture(RhiImage* resource) = 0;

        /*!
         * \brief Clean up any GPU objects a Semaphores may own
         *
         * While Semaphores are per-renderpack objects, and their CPU memory will be cleaned up when a new renderpack is loaded, we still
         * need to clean up their GPU objects
         */
        virtual void destroy_semaphores(std::vector<RhiSemaphore*>& semaphores) = 0;

        /*!
         * \brief Clean up any GPU objects a Fence may own
         *
         * While Fence are per-renderpack objects, and their CPU memory will be cleaned up when a new renderpack is loaded, we still need to
         * clean up their GPU objects
         */
        virtual void destroy_fences(const std::vector<RhiFence*>& fences) = 0;

        [[nodiscard]] Swapchain* get_swapchain() const;

        /*!
         * \brief Allocates a new command list that can be used from the provided thread and has the desired type
         *
         * Ownership of the command list is given to the caller. You can record your commands into it, then submit it
         * to a queue. Submitting it gives ownership back to the render engine, and recording commands into a
         * submitted command list is not supported
         *
         * There is one command list pool per swapchain image per thread. All the pools for one swapchain image are
         * reset at the beginning of a frame that renders to that swapchain image. This means that any command list
         * allocated in one frame will not be valid in the next frame. DO NOT hold on to command lists
         *
         * Command lists allocated by this method are returned ready to record commands into - the caller doesn't need
         * to begin the command list
         */
        virtual RhiRenderCommandList* create_command_list(uint32_t thread_idx,
                                                          QueueType needed_queue_type,
                                                          RhiRenderCommandList::Level level) = 0;

        virtual void submit_command_list(RhiRenderCommandList* cmds,
                                         QueueType queue,
                                         RhiFence* fence_to_signal = nullptr,
                                         const std::vector<RhiSemaphore*>& wait_semaphores = {},
                                         const std::vector<RhiSemaphore*>& signal_semaphores = {}) = 0;

        /*!
         * \brief Performs any work that's needed to end the provided frame
         */
        virtual void end_frame(FrameContext& ctx) = 0;

    protected:
        NovaWindow& window;

        glm::uvec2 swapchain_size = {};
        Swapchain* swapchain = nullptr;

        /*!
         * \brief Initializes the engine
         * \param settings The settings passed to nova
         * \param window The OS window that we'll be rendering to
         *
         * Intentionally does nothing. This constructor serves mostly to ensure that concrete render engines have a
         * constructor that takes in some settings
         *
         * \attention Called by the various render engine implementations
         */
        RenderDevice(NovaSettingsAccessManager& settings,
                     NovaWindow& window);
    };

    /*!
     * \brief Creates a new API-agnostic render device
     *
     * Right now we only support creating Vulkan render devices, but in the future we might support devices for different APIs, or different
     * types of hardware
     */
    std::unique_ptr<RenderDevice> create_render_device(NovaSettingsAccessManager& settings, NovaWindow& window);
} // namespace nova::renderer::rhi
