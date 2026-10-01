cmake_minimum_required(VERSION 3.25)
project(randy-vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no extra runtime DLL next to the game

# Drop-in randy31.dll. Exports not implemented here forward to the original, renamed randy31_orig.dll.
add_library(randy31 SHARED proxy/dllmain.cpp proxy/trace.cpp proxy/randy31.def)
set_target_properties(randy31 PROPERTIES OUTPUT_NAME randy31 PREFIX "")

# Wine-run check that every proxy export resolves to the original's address.
add_executable(forward_check tests/forward_check.cpp)

# ---- rvk: Vulkan renderer with a D3D7-shaped front end ----
set(VULKAN_SDK_DIR "$ENV{HOME}/repos/vulkan-sdk/1.4.321.1/x86_64" CACHE PATH "Vulkan SDK (headers, VMA, glslc)")
find_program(GLSLC glslc HINTS "${VULKAN_SDK_DIR}/bin" REQUIRED)

set(RVK_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/rvk-shaders")
set(RVK_SHADER_INCS)
foreach(stage vert frag)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/ffp.${stage}")
    set(out "${RVK_SHADER_OUT}/ffp.${stage}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RVK_SHADER_OUT}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o "${out}" "${src}"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/constants.glsl"
        COMMENT "glslc ffp.${stage}")
    list(APPEND RVK_SHADER_INCS "${out}")
endforeach()

add_library(rvk STATIC rvk/vk.cpp rvk/vma.cpp rvk/device.cpp rvk/resources.cpp rvk/draw.cpp ${RVK_SHADER_INCS})
target_include_directories(rvk PUBLIC rvk PRIVATE "${RVK_SHADER_OUT}")
# Vulkan headers and VMA come from the SDK; /imsvc keeps their warnings quiet.
target_compile_options(rvk PUBLIC "/imsvc${VULKAN_SDK_DIR}/include")
target_compile_options(rvk PRIVATE /W3)
target_compile_definitions(rvk PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(rvk PUBLIC user32)

add_executable(rvk_demo tests/rvk_demo.cpp)
target_link_libraries(rvk_demo PRIVATE rvk)
