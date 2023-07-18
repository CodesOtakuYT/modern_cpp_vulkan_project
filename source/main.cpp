#include <SDL.h>
#include <SDL_vulkan.h>
#include <stdexcept>
#include <SDL_platform_defines.h>

#define VULKAN_HPP_ENABLE_DYNAMIC_LOADER_TOOL 0

#ifdef __WINDOWS__
#define VK_USE_PLATFORM_WIN32_KHR
#endif

#include <vulkan/vulkan_raii.hpp>
#include <SDL_syswm.h>

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

    [[nodiscard]] auto get_code() const noexcept {
        return code;
    }
};

class SDL : Noncopyable {
public:
    explicit SDL(SDL_InitFlags init_flags) {
        if (const auto error_code = SDL_Init(init_flags))
            throw SDLException{SDL_GetError(), error_code};
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

#pragma clang diagnostic push
#pragma ide diagnostic ignored "readability-convert-member-functions-to-static"

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

#pragma clang diagnostic pop
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

    [[nodiscard]] auto create_surface(const vk::raii::Instance &instance) const {
        vk::SurfaceKHR::NativeType surface_handle;
        if (!SDL_Vulkan_CreateSurface(handle, *instance, &surface_handle))
            throw SDLException{SDL_GetError()};
        return vk::raii::SurfaceKHR{instance, surface_handle};
    }

    [[nodiscard]] auto get_physical_device_presentation_support(const vk::raii::PhysicalDevice &physical_device,
                                                                uint32_t queue_family_index) const {
        SDL_SysWMinfo info;
        if (const auto return_code = SDL_GetWindowWMInfo(handle, &info, SDL_SYSWM_CURRENT_VERSION))
            throw SDLException{SDL_GetError(), return_code};
        switch (info.subsystem) {
            case SDL_SYSWM_TYPE::SDL_SYSWM_WINDOWS:
                return static_cast<bool>(physical_device.getDispatcher()->vkGetPhysicalDeviceWin32PresentationSupportKHR(
                        *physical_device,
                        queue_family_index));
            default:
                SDL_LogWarn(SDL_LogCategory::SDL_LOG_CATEGORY_APPLICATION,
                            "Physical device presentation support for the current platform is not implemented, returning true anyway");
                return true;
        }
    }

    [[nodiscard]] auto get_handle() const noexcept {
        return handle;
    }
};

using QueueFamily = std::pair<vk::raii::PhysicalDevice, size_t>;

auto main(int argc, char **argv) -> int {
    const SDL sdl{SDL_InitFlags::SDL_INIT_VIDEO};
    const VulkanLibrary vulkan_library{};

    vk::raii::Context context{vulkan_library.get_instance_proc_addr()};

    vk::ApplicationInfo application_info{};
    application_info.apiVersion = VK_API_VERSION_1_3;

    vk::InstanceCreateInfo create_info{};
    auto extensions = vulkan_library.get_instance_extensions();
    if (SDL_GetPlatform() == std::string_view{"macOS"}) {
        create_info.flags |= vk::InstanceCreateFlagBits::eEnumeratePortabilityKHR;
        extensions.emplace_back(VK_KHR_PORTABILITY_ENUMERATION_EXTENSION_NAME);
    }
    create_info.setPEnabledExtensionNames(extensions);
    create_info.pApplicationInfo = &application_info;
    const vk::raii::Instance instance{context, create_info};

    const Window window{"Salam", 800, 600, SDL_WindowFlags::SDL_WINDOW_VULKAN};
    const auto surface = window.create_surface(instance);

    std::optional<QueueFamily> queue_family{};
    for (const auto &physical_device: instance.enumeratePhysicalDevices()) {
        const auto queue_families_properties = physical_device.getQueueFamilyProperties();
        for (std::size_t queue_family_index{};
             queue_family_index != queue_families_properties.size(); ++queue_family_index) {
            const auto queue_family_properties{queue_families_properties[queue_family_index]};
            const auto does_support_graphics{queue_family_properties.queueFlags & vk::QueueFlagBits::eGraphics};
            const auto does_support_presentation{
                    window.get_physical_device_presentation_support(physical_device, queue_family_index)};

            if (does_support_graphics && does_support_presentation)
                queue_family = {physical_device, queue_family_index};
        }
    }

    if (!queue_family.has_value()) throw std::runtime_error("Couldn't find a suitable queue family");
    const auto &[physical_device, queue_family_index] = *queue_family;
    vk::DeviceCreateInfo info{};
    vk::DeviceQueueCreateInfo queue_create_infos{};
    const auto queue_priorities{0.0f};
    queue_create_infos.setQueuePriorities(queue_priorities);
    queue_create_infos.queueFamilyIndex = queue_family_index;
    info.setQueueCreateInfos(queue_create_infos);

    const vk::raii::Device device{physical_device, info};
    const vk::raii::Queue queue{device, queue_family_index, 0};

    bool should_close{};
    while (!should_close) {
        for (SDL_Event event; SDL_PollEvent(&event);) {
            switch (event.type) {
                case SDL_EventType::SDL_EVENT_QUIT:
                    should_close = true;
            }
        }
    }

    return 0;
};