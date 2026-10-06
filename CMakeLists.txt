cmake_minimum_required(VERSION 3.25)
project(randy-vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no extra runtime DLL next to the game

# Native Randy: our implementations of randy31_orig's code (proxy/native, docs/native.md), shared with the tests.
add_library(randy_native STATIC proxy/native/stb_image.cpp proxy/native/hook.cpp proxy/native/gui.cpp proxy/native/native.cpp proxy/native/cat_skin.cpp proxy/native/cat_anim.cpp proxy/native/scene.cpp
            proxy/native/vc10.cpp proxy/native/orig_api.gen.cpp proxy/native/cat_render.cpp proxy/native/cat_mesh.cpp
            proxy/native/cat_query.cpp proxy/native/device.cpp proxy/native/state_blob.cpp proxy/native/dynamic_vb.cpp proxy/native/randy.cpp proxy/native/surface.cpp proxy/native/resource.cpp proxy/native/serialize.cpp proxy/native/texture.cpp proxy/native/material.cpp proxy/native/viewport.cpp proxy/native/refframe.cpp proxy/native/visual.cpp proxy/native/camera.cpp proxy/native/mesh.cpp proxy/native/mesh_data.cpp proxy/native/occluder.cpp proxy/native/cat_life.cpp proxy/native/cat_pick.cpp proxy/native/cat_data.cpp proxy/native/cat_anim_data.cpp proxy/native/bvolume.cpp proxy/native/dxerror.cpp proxy/native/devices.cpp proxy/native/randy_init.cpp proxy/native/debugger.cpp proxy/native/gone_trap.cpp proxy/native/keyframe.cpp proxy/native/sprite.cpp proxy/native/shadowlands.cpp proxy/native/color.cpp proxy/native/pixfmt.cpp proxy/native/lbitmap.cpp proxy/native/helpers.cpp proxy/native/texture_stream.cpp proxy/native/faf.cpp proxy/native/rshadow.cpp proxy/native/connector.cpp proxy/native/cat_status.cpp
            proxy/native/rgrid.cpp proxy/native/timer.cpp proxy/native/funcom.cpp proxy/native/findobject.cpp proxy/native/d3dx.cpp)
target_include_directories(randy_native PUBLIC proxy third_party)
target_link_libraries(randy_native PUBLIC rvk)
target_compile_definitions(randy_native PRIVATE _CRT_SECURE_NO_WARNINGS)

# Drop-in randy31.dll. Exports not implemented here forward to the original, renamed randy31_orig.dll.
add_library(randy31 SHARED proxy/dllmain.cpp proxy/trace.cpp proxy/ddraw/com_trace.cpp proxy/ddraw/com_trace_detail.cpp
            proxy/ddraw/rvk_ddraw.cpp proxy/ddraw/rvk_surface.cpp proxy/ddraw/rvk_device.cpp proxy/ddraw/rvk_settings.cpp proxy/ddraw/rvk_particles.cpp
            proxy/ddraw/rvk_materials.cpp proxy/exports.gen.cpp proxy/randy31.def)
target_include_directories(randy31 PRIVATE proxy third_party)
target_compile_definitions(randy31 PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(randy31 PRIVATE randy_native rvk user32)
set_target_properties(randy31 PROPERTIES OUTPUT_NAME randy31 PREFIX "")
target_link_options(randy31 PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/randy31.map")   # for profiling: function addresses

# Wine-run check that every proxy export resolves to the original's address.
add_executable(forward_check tests/forward_check.cpp)

# ---- rvk: Vulkan renderer with a D3D7-shaped front end ----
set(VULKAN_SDK_DIR "$ENV{HOME}/repos/vulkan-sdk/1.4.321.1/x86_64" CACHE PATH "Vulkan SDK (headers, VMA, glslc)")
find_program(GLSLC glslc HINTS "${VULKAN_SDK_DIR}/bin" REQUIRED)

set(RVK_SHADER_OUT "${CMAKE_CURRENT_BINARY_DIR}/rvk-shaders")
set(RVK_SHADER_INCS)
foreach(shader ffp.vert ffp.tesc ffp.tese ffp.frag ffp_glow.frag ffp_nocut.frag ffp_glow_nocut.frag prepass_cutout.frag shadow.vert shadow.frag fullscreen.vert tonemap.frag bloom_down.frag bloom_up.frag ao.frag ao_blur.frag gi.frag gi_blur.frag volume.frag volume_blur.frag ssr.frag contact.frag taa.frag sharpen.frag motion_blur.frag motion_tilemax.frag motion_neighbourmax.frag motion_object.frag dof_composite.frag dof_focus.frag dof_prefilter.frag dof_tiles.frag dof_gather.frag dof_final.frag ui_composite.frag grass.vert grass.frag grass_depth.frag particles.comp skin.comp)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/${shader}")
    set(out "${RVK_SHADER_OUT}/${shader}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RVK_SHADER_OUT}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o "${out}" "${src}"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/constants.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/lighting.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/ffp_main.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/motion_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/occlusion.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/dof_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/frame_lights.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/sway.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/ffp_varyings.glsl"
        COMMENT "glslc ${shader}")
    list(APPEND RVK_SHADER_INCS "${out}")
endforeach()

add_library(rvk STATIC rvk/vk.cpp rvk/vma.cpp rvk/device.cpp rvk/resources.cpp rvk/draw.cpp rvk/diag.cpp rvk/shadow.cpp rvk/pointshadow.cpp rvk/hdr.cpp rvk/particles.cpp rvk/grading.cpp rvk/profile.cpp rvk/skin.cpp rvk/skin_gpu.cpp rvk/static_gpu.cpp rvk/prepass.cpp rvk/threaded.cpp rvk/interface.cpp rvk/grass.cpp ${RVK_SHADER_INCS})
target_include_directories(rvk PUBLIC rvk PRIVATE "${RVK_SHADER_OUT}")
# Vulkan headers and VMA come from the SDK; /imsvc keeps their warnings quiet.
target_compile_options(rvk PUBLIC "/imsvc${VULKAN_SDK_DIR}/include")
target_compile_options(rvk PRIVATE /W3)
target_compile_definitions(rvk PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(rvk PUBLIC user32 synchronization)

add_executable(rvk_demo tests/rvk_demo.cpp)
target_link_libraries(rvk_demo PRIVATE rvk)

# Native replacements against the original code they replace (run with tools/native-check.sh).
add_executable(native_check tests/native_check.cpp)
target_link_libraries(native_check PRIVATE randy_native)

# Boots the real randy31 renderer outside the game (run with tools/randy-harness.sh).
add_executable(randy_harness tests/randy_harness.cpp)
target_compile_definitions(randy_harness PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(randy_harness PRIVATE user32)
