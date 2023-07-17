cmake_minimum_required(VERSION 3.25)
project(modern_cpp_vulkan_project)

add_subdirectory(source)
add_subdirectory(thirdparty)
target_link_libraries(source PRIVATE thirdparty)