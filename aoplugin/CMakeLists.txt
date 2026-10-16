# ao-vk's AOPlugins plugin (ao-vk.dll): the Renderer tab in the options window. A build of its own - randy31.dll's
# build doesn't know about it. See README.md.
cmake_minimum_required(VERSION 3.24)
project(ao-vk-aoplugin LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: nothing to install next to it

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 4)
    message(FATAL_ERROR "ao-vk.dll is loaded by the 32-bit client: build it for x86 (CMakePresets.json)")
endif()

add_library(ao-vk SHARED ao_vk_plugin.cpp view_distance.cpp camp_on_disconnect.cpp)
set_target_properties(ao-vk PROPERTIES PREFIX "")
target_include_directories(ao-vk PRIVATE include)
target_compile_definitions(ao-vk PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX)
if(MSVC)
    target_compile_options(ao-vk PRIVATE /W4 /permissive- /EHsc)
endif()
