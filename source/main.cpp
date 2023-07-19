#include <stdexcept>
#include <expected>
#include <ranges>

#include <SDL.h>
#include <SDL_vulkan.h>
#include <SDL_syswm.h>

#ifdef SDL_ENABLE_SYSWM_WINDOWS
#define VK_USE_PLATFORM_WIN32_KHR
#endif
#ifdef SDL_ENABLE_SYSWM_WAYLAND
#define VK_USE_PLATFORM_WAYLAND_KHR
#endif
#ifdef SDL_ENABLE_SYSWM_X11
#define VK_USE_PLATFORM_XLIB_KHR
#endif
#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0
#define VULKAN_HPP_NO_DEFAULT_DISPATCHER

#include <vulkan/vulkan_raii.hpp>

class Noncopyable {
public:
    Noncopyable() = default;

    Noncopyable(const Noncopyable &) = delete;

    const Noncopyable &operator=(const Noncopyable &) = delete;
};

class SDLException : private std::runtime_error {
    const int code;
public:
    explicit SDLException(const char *message, const int code = 0) : runtime_error(message), code{code} {}
};

class SDL : Noncopyable {
public:
    explicit SDL(SDL_InitFlags init_flags) {
        if (const auto error_code = SDL_Init(init_flags))
            throw SDLException{SDL_GetError(), error_code};
    }

    [[nodiscard]] auto get_platform() const {
        return std::string_view{SDL_GetPlatform()};
    }

    ~SDL() {
        SDL_Quit();
    }
};

class VulkanLibrary : Noncopyable {
public:
    explicit VulkanLibrary(const char *path = nullptr) {
        if (const auto error_code = SDL_Vulkan_LoadLibrary(path))
            throw SDLException{SDL_GetError(), error_code};

    }

    ~VulkanLibrary() {
        SDL_Vulkan_UnloadLibrary();
    }

    [[nodiscard]] auto get_instance_proc_addr() const {
        if (const auto get_instance_proc_addr = reinterpret_cast<PFN_vkGetInstanceProcAddr>(SDL_Vulkan_GetVkGetInstanceProcAddr()))
            return get_instance_proc_addr;
        else
            throw SDLException{"Couldn't load vkGetInstanceProcAddr function from the vulkan dynamic library"};
    }

    [[nodiscard]] auto get_instance_extensions() const {
        uint32_t count;
        if (!SDL_Vulkan_GetInstanceExtensions(&count, nullptr))
            throw SDLException{"Couldn't get vulkan instance extensions count"};
        std::vector<const char *> extensions(count);
        if (!SDL_Vulkan_GetInstanceExtensions(&count, extensions.data()))
            throw SDLException{"Couldn't get vulkan instance extensions"};
        return extensions;
    }
};

enum class PresentationSupportQueryError {
    Failure,
    Unknown,
    Unimplemented,
};

class Window : Noncopyable {
    SDL_Window *handle;
public:
    Window(const char *title, const int width, const int height,
           const SDL_WindowFlags flags = static_cast<SDL_WindowFlags>(0)) : handle{
            SDL_CreateWindow(title, width, height, flags)} {
        if (!handle)
            throw SDLException{SDL_GetError()};
    }

    ~Window() {
        SDL_DestroyWindow(handle);
    }

    void show() const {
        SDL_ShowWindow(handle);
    }

    [[nodiscard]] auto create_surface(const vk::raii::Instance &instance) const {
        vk::SurfaceKHR::NativeType surface_handle;
        if (!SDL_Vulkan_CreateSurface(handle, *instance, &surface_handle))
            throw SDLException{SDL_GetError()};
        return vk::raii::SurfaceKHR{instance, surface_handle};
    }

    [[nodiscard]] std::expected<bool, PresentationSupportQueryError>
    get_physical_device_presentation_support(const vk::raii::PhysicalDevice &physical_device,
                                             uint32_t queue_family_index) const {
        SDL_SysWMinfo info;
        if (const auto return_code = SDL_GetWindowWMInfo(handle, &info, SDL_SYSWM_CURRENT_VERSION))
            throw SDLException{SDL_GetError(), return_code};
        // https://registry.khronos.org/vulkan/specs/1.3-extensions/html/chap34.html#_querying_for_wsi_support
        switch (info.subsystem) {
            case SDL_SYSWM_TYPE::SDL_SYSWM_ANDROID:
            case SDL_SYSWM_TYPE::SDL_SYSWM_COCOA:
            case SDL_SYSWM_TYPE::SDL_SYSWM_UIKIT:
                return true;
#ifdef VK_USE_PLATFORM_WIN32_KHR
            case SDL_SYSWM_TYPE::SDL_SYSWM_WINDOWS:
                return static_cast<bool>(physical_device.getDispatcher()->vkGetPhysicalDeviceWin32PresentationSupportKHR(
                        *physical_device,
                        queue_family_index));
#endif
#ifdef VK_USE_PLATFORM_WAYLAND_KHR
                case SDL_SYSWM_TYPE::SDL_SYSWM_WAYLAND:
                    return static_cast<bool>(physical_device.getDispatcher()->vkGetPhysicalDeviceWaylandPresentationSupportKHR(
                            *physical_device,
                            queue_family_index, info.info.wl.display));
#endif
#ifdef VK_USE_PLATFORM_XLIB_KHR
                case SDL_SYSWM_TYPE::SDL_SYSWM_X11: {
                    XWindowAttributes attrib;
                    const auto x11 = info.info.x11;
                    if(X11_XGetWindowAttributes(x11.display, x11.window, &attrib)) {
                        const auto visual = attrib.visual;
                        const auto visual_id = X11_XVisualIDFromVisual(visual);
                        return static_cast<bool>(physical_device.getDispatcher()->vkGetPhysicalDeviceXlibPresentationSupportKHR(
                                *physical_device,
                                queue_family_index, x11.display, visual_id));
                    }
                    return std::unexpected{PresentationSupportQueryError::Failure};
                }
#endif
            case SDL_SYSWM_TYPE::SDL_SYSWM_UNKNOWN:
                return std::unexpected{PresentationSupportQueryError::Unknown};
            default:
                return std::unexpected{PresentationSupportQueryError::Unimplemented};
        }
    }
};

using QueueFamily = std::pair<vk::raii::PhysicalDevice, size_t>;

class Swapchain : Noncopyable {
    vk::raii::SwapchainKHR handle;
};

class Surface : Noncopyable {
    const vk::raii::SurfaceKHR handle;
    std::optional<Swapchain> swapchain;
    const QueueFamily &queue_family;
public:
    static auto create_swapchain(const vk::raii::Device &device, const QueueFamily &queue_family,
                                 const vk::raii::SurfaceKHR &surface,
                                 const std::optional<vk::SwapchainKHR> old_swapchain) {
        const auto &[physical_device, queue_family_index] = queue_family;

        const auto surface_capabilities{physical_device.getSurfaceCapabilitiesKHR(*surface)};
        const auto surface_format{[&]() {
            const auto surface_formats{physical_device.getSurfaceFormatsKHR(*surface)};
            for (const auto &surface_format: surface_formats) {
                if (surface_format.format == vk::Format::eB8G8R8A8Srgb &&
                    surface_format.colorSpace == vk::ColorSpaceKHR::eSrgbNonlinear) {
                    return surface_format;
                }
            }
            return surface_formats.front();
        }()};

        vk::SwapchainCreateInfoKHR swapchain_create_info{};
        swapchain_create_info.surface = *surface;
        swapchain_create_info.clipped = true;
        swapchain_create_info.compositeAlpha = vk::CompositeAlphaFlagBitsKHR::eOpaque;
        swapchain_create_info.preTransform = surface_capabilities.currentTransform;
        swapchain_create_info.imageSharingMode = vk::SharingMode::eExclusive;
        swapchain_create_info.setQueueFamilyIndices(queue_family_index);
        swapchain_create_info.presentMode = vk::PresentModeKHR::eFifo;
        swapchain_create_info.imageExtent = surface_capabilities.currentExtent;
        swapchain_create_info.minImageCount = std::clamp(surface_capabilities.minImageCount + 1,
                                                         surface_capabilities.minImageCount,
                                                         surface_capabilities.maxImageCount);
        swapchain_create_info.imageUsage = vk::ImageUsageFlagBits::eColorAttachment;
        swapchain_create_info.imageArrayLayers = 1;
        swapchain_create_info.imageFormat = surface_format.format;
        swapchain_create_info.imageColorSpace = surface_format.colorSpace;
        if (old_swapchain.has_value())
            swapchain_create_info.oldSwapchain = *old_swapchain;
        return vk::raii::SwapchainKHR{device, swapchain_create_info};
    }

    Surface(const Window &window, const vk::raii::Instance &instance, const vk::raii::Device &device,
            const QueueFamily &queue_family) : handle(
            window.create_surface(instance)), queue_family{queue_family} {
        create_swapchain(device, queue_family, handle, {});
    }
};

auto main(const int, const char *const *const) -> int {
    const SDL sdl{SDL_InitFlags::SDL_INIT_VIDEO};
    const VulkanLibrary vulkan_library{};

    const vk::raii::Context context{vulkan_library.get_instance_proc_addr()};
    for (const auto &layer: context.enumerateInstanceLayerProperties()) {
        SDL_Log("%s", layer.layerName.data());
    }

    vk::ApplicationInfo application_info{};
    application_info.apiVersion = VK_API_VERSION_1_3;

    vk::InstanceCreateInfo create_info{};
    auto extensions = vulkan_library.get_instance_extensions();
#ifndef NDEBUG
    extensions.emplace_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
#endif
    if (sdl.get_platform() == "macOS") {
        create_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    create_info.setPEnabledExtensionNames(extensions);
    create_info.pApplicationInfo = &application_info;

#ifdef NDEBUG
    vk::StructureChain<vk::InstanceCreateInfo> instance_structure_chain{create_info};
#else
    vk::DebugUtilsMessengerCreateInfoEXT debug_utils_messenger_create_info{};
    debug_utils_messenger_create_info.messageSeverity =
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eError | vk::DebugUtilsMessageSeverityFlagBitsEXT::eWarning |
            vk::DebugUtilsMessageSeverityFlagBitsEXT::eInfo | vk::DebugUtilsMessageSeverityFlagBitsEXT::eVerbose;
    debug_utils_messenger_create_info.messageType =
            vk::DebugUtilsMessageTypeFlagBitsEXT::eGeneral | vk::DebugUtilsMessageTypeFlagBitsEXT::eValidation |
            vk::DebugUtilsMessageTypeFlagBitsEXT::ePerformance |
            vk::DebugUtilsMessageTypeFlagBitsEXT::eDeviceAddressBinding;
    debug_utils_messenger_create_info.pfnUserCallback = [](
            const VkDebugUtilsMessageSeverityFlagBitsEXT message_severity,
            const VkDebugUtilsMessageTypeFlagsEXT message_type,
            const VkDebugUtilsMessengerCallbackDataEXT *const pCallbackData,
            void *) -> VkBool32 {
        const auto log{[](VkDebugUtilsMessageSeverityFlagBitsEXT message_severity) {
            switch (message_severity) {
                case VkDebugUtilsMessageSeverityFlagBitsEXT::VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT:
                    return SDL_LogError;
                case VK_DEBUG_UTILS_MESSAGE_SEVERITY_VERBOSE_BIT_EXT:
                    return SDL_LogVerbose;
                case VK_DEBUG_UTILS_MESSAGE_SEVERITY_INFO_BIT_EXT:
                    return SDL_LogInfo;
                case VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT:
                    return SDL_LogWarn;
                default:
                    SDL_TriggerBreakpoint();
                    std::unreachable();
            }
        }(message_severity)};
        const auto message_type_string{vk::to_string(static_cast<vk::DebugUtilsMessageTypeFlagsEXT>(message_type))};
        log(SDL_LogCategory::SDL_LOG_CATEGORY_APPLICATION, "{%s} %s: %s", pCallbackData->pMessageIdName,
            message_type_string.data(),
            pCallbackData->pMessage);
        return vk::False;
    };
    vk::StructureChain<vk::InstanceCreateInfo, vk::DebugUtilsMessengerCreateInfoEXT> instance_structure_chain{
            create_info, debug_utils_messenger_create_info};
#endif

    const vk::raii::Instance instance{context, instance_structure_chain.get<vk::InstanceCreateInfo>()};

    const Window window{"Salam", 800, 600, static_cast<const SDL_WindowFlags>(SDL_WindowFlags::SDL_WINDOW_VULKAN |
                                                                              SDL_WindowFlags::SDL_WINDOW_HIDDEN)};

    std::optional<QueueFamily> queue_family{};
    for (const auto &physical_device: instance.enumeratePhysicalDevices()) {
        for (const auto &[queue_family_index, queue_family_properties]: std::views::zip(std::views::iota(0),
                                                                                        physical_device.getQueueFamilyProperties())) {
            const auto does_support_graphics{queue_family_properties.queueFlags & vk::QueueFlagBits::eGraphics};
            const auto does_support_presentation{
                    window.get_physical_device_presentation_support(physical_device, queue_family_index)};

            if (does_support_graphics && does_support_presentation.value_or(true))
                queue_family = {physical_device, queue_family_index};
        }
    }

    if (!queue_family.has_value()) throw std::runtime_error("Couldn't find a suitable queue family");
    const auto &[physical_device, queue_family_index] = *queue_family;
    vk::DeviceCreateInfo info{};
    vk::DeviceQueueCreateInfo queue_create_infos{};
    const auto queue_priorities = {0.0f};
    queue_create_infos.setQueuePriorities(queue_priorities);
    queue_create_infos.queueFamilyIndex = queue_family_index;
    info.setQueueCreateInfos(queue_create_infos);
    const auto device_extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    info.setPEnabledExtensionNames(device_extensions);

    const vk::raii::Device device{physical_device, info};
    const vk::raii::Queue queue{device, queue_family_index, 0};

    std::optional<Surface> surface{};
    const auto create_surface = [&]() {
        surface.emplace(window, instance, device, *queue_family);
    };
#ifndef __ANDROID__
    create_surface();
#endif
    window.show();

    for (auto should_close{false}; !should_close;) {
        for (SDL_Event event; SDL_PollEvent(&event);) {
            switch (event.type) {
                case SDL_EventType::SDL_EVENT_QUIT:
                    should_close = true;
                    break;
                case SDL_EventType::SDL_EVENT_DID_ENTER_FOREGROUND:
                    create_surface();
                    break;
                case SDL_EventType::SDL_EVENT_WILL_ENTER_BACKGROUND:
                    surface.reset();
                    break;
            }
        }
    }

    return 0;
}