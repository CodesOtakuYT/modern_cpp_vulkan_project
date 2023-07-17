add_library(thirdparty INTERFACE)

add_subdirectory(Vulkan-Headers)
add_subdirectory(SDL)

target_link_libraries(thirdparty INTERFACE Vulkan::Headers SDL3::SDL3)