cmake_minimum_required(VERSION 3.25)
project(randy-vk LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded")   # static CRT: no extra runtime DLL next to the game

# Native Randy: our implementations of randy31_orig's code (proxy/native, docs/native.md), shared with the tests.
add_library(randy_native STATIC proxy/native/stb_image.cpp proxy/native/hook.cpp proxy/native/crash.cpp proxy/native/gui.cpp proxy/native/rings.cpp proxy/native/ground.cpp proxy/native/zone.cpp proxy/native/native.cpp proxy/native/cat_skin.cpp proxy/native/cat_anim.cpp proxy/native/scene.cpp proxy/native/popin.cpp
            proxy/native/vc10.cpp proxy/native/orig_api.gen.cpp proxy/native/cat_render.cpp proxy/native/cat_mesh.cpp
            proxy/native/cat_query.cpp proxy/native/device.cpp proxy/native/state_blob.cpp proxy/native/dynamic_vb.cpp proxy/native/randy.cpp proxy/native/surface.cpp proxy/native/resource.cpp proxy/native/serialize.cpp proxy/native/texture.cpp proxy/native/material.cpp proxy/native/viewport.cpp proxy/native/refframe.cpp proxy/native/visual.cpp proxy/native/camera.cpp proxy/native/mesh.cpp proxy/native/mesh_data.cpp proxy/native/occluder.cpp proxy/native/cat_life.cpp proxy/native/cat_pick.cpp proxy/native/cat_data.cpp proxy/native/cat_anim_data.cpp proxy/native/bvolume.cpp proxy/native/dxerror.cpp proxy/native/devices.cpp proxy/native/randy_init.cpp proxy/native/debugger.cpp proxy/native/gone_trap.cpp proxy/native/keyframe.cpp proxy/native/sprite.cpp proxy/native/shadowlands.cpp proxy/native/color.cpp proxy/native/pixfmt.cpp proxy/native/lbitmap.cpp proxy/native/helpers.cpp proxy/native/texture_stream.cpp proxy/native/faf.cpp proxy/native/rshadow.cpp proxy/native/connector.cpp proxy/native/cat_status.cpp
            proxy/native/rgrid.cpp proxy/native/timer.cpp proxy/native/funcom.cpp proxy/native/findobject.cpp proxy/native/d3dx.cpp)
target_include_directories(randy_native PUBLIC proxy third_party)
target_link_libraries(randy_native PUBLIC rvk)
target_compile_definitions(randy_native PRIVATE _CRT_SECURE_NO_WARNINGS)

# Drop-in randy31.dll. Exports not implemented here forward to the original, renamed randy31_orig.dll.
add_library(randy31 SHARED proxy/dllmain.cpp proxy/trace.cpp proxy/ddraw/com_trace.cpp proxy/ddraw/com_trace_detail.cpp
            proxy/ddraw/rvk_ddraw.cpp proxy/ddraw/rvk_surface.cpp proxy/ddraw/rvk_device.cpp proxy/ddraw/rvk_settings.cpp proxy/ddraw/rvk_particles.cpp proxy/ddraw/rvk_weather.cpp proxy/ddraw/rvk_smoke.cpp proxy/ddraw/rvk_fx.cpp proxy/ddraw/rvk_time.cpp proxy/ddraw/rvk_statels.cpp proxy/ddraw/rvk_ground.cpp proxy/ddraw/rvk_freecam.cpp proxy/ddraw/rvk_input.cpp proxy/ddraw/rvk_console.cpp
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
foreach(shader ffp.vert ffp.tesc ffp.tese ffp.frag ffp_glow.frag ffp_nocut.frag ffp_glow_nocut.frag ffp_lite.frag ffp_lite_nocut.frag prepass_cutout.frag shadow.vert shadow.frag fullscreen.vert tonemap.frag bloom_down.frag bloom_up.frag ao.frag ao_blur.frag gi.frag gi_blur.frag volume.frag volume_blur.frag sand_fog.frag sand_fog_blur.frag ssr.frag gbuffer.frag sssr_resolve.frag sssr_env.frag cacao_pack.frag brix_pack.frag contact.frag taa.frag depth_up.frag motion_fill.frag reactive_mask.frag sharpen.frag fxaa.frag motion_blur.frag motion_tilemax.frag motion_neighbourmax.frag motion_object.frag dof_composite.frag dof_focus.frag dof_prefilter.frag dof_tiles.frag dof_gather.frag dof_final.frag ui_composite.frag handover.frag portal_open.frag grass.vert grass.frag grass_shadow.vert particles.comp fx.comp skin.comp water.vert water.frag water_surface.vert water_surface.tesc water_surface.tese water_spray.vert water_spray.frag water_sim.comp rain.vert rain.frag splash.vert splash.frag bolt.vert bolt.frag debris.vert debris.frag smoke_sim.comp smoke.vert smoke.frag smoke_up.frag cloud.vert cloud.frag cloud_up.frag cloud_shadow.frag cloud_noise.comp cloud_layer_noise.comp cloud_weather.frag cloud_layer.frag cloud_resolve.frag sun.frag sky_glow.frag atmos_transmittance.frag atmos_multi.frag atmos_view.frag sun_vis.comp scene_luma.comp rt_debug.comp rt_ao.comp rt_shadow.comp rt_shadow_blur.comp rt_reflect.comp smoke_seen.comp terrain_sun.frag sand_prints.frag env_accum.frag env_filter.frag)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/${shader}")
    set(out "${RVK_SHADER_OUT}/${shader}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RVK_SHADER_OUT}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num -o "${out}" "${src}"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/constants.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/lighting.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/ffp_main.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/terrain.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/terrain_rock.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/parallax.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/motion_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/occlusion.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/dof_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/frame_lights.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/sway.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/ffp_varyings.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/grass_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/wind.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/water_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/smoke_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/rain_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/clouds_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/cloud_shadow_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/cloud_light.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/sky_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/atmos_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/dither.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/rt_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/env_common.glsl"
        COMMENT "glslc ${shader}")
    list(APPEND RVK_SHADER_INCS "${out}")
endforeach()

# Variants built from another shader with a define: output name, source, define.
foreach(entry "ffp_ng.frag|ffp.frag|RVK_NO_GROUND" "ffp_glow_ng.frag|ffp_glow.frag|RVK_NO_GROUND" "ffp_nocut_ng.frag|ffp_nocut.frag|RVK_NO_GROUND" "ffp_glow_nocut_ng.frag|ffp_glow_nocut.frag|RVK_NO_GROUND" "ffp_nf.frag|ffp.frag|RVK_NO_FRAME_LIGHTS" "ffp_glow_nf.frag|ffp_glow.frag|RVK_NO_FRAME_LIGHTS" "ffp_nocut_nf.frag|ffp_nocut.frag|RVK_NO_FRAME_LIGHTS" "ffp_glow_nocut_nf.frag|ffp_glow_nocut.frag|RVK_NO_FRAME_LIGHTS" "leaf.vert|ffp.vert|RVK_LEAF" "leaf_shadow.vert|shadow.vert|RVK_LEAF" "smoke_half.frag|smoke.frag|RVK_SMOKE_HALF" "cloud_half.frag|cloud.frag|RVK_CLOUD_HALF" "cloud_layer_shadow.frag|cloud_layer.frag|RVK_CLOUD_SHADOW" "cloud_layer_sunray.frag|cloud_layer.frag|RVK_CLOUD_SUNRAY" "rain_near.frag|rain.frag|RVK_RAIN_NEAR" "portal_mark.frag|portal_open.frag|RVK_PORTAL_MARK" "fullscreen_far.vert|fullscreen.vert|RVK_FAR")
    string(REPLACE "|" ";" variant "${entry}")
    list(GET variant 0 name)
    list(GET variant 1 shader)
    list(GET variant 2 define)
    get_filename_component(stage "${name}" LAST_EXT)
    string(SUBSTRING "${stage}" 1 -1 stage)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/${shader}")
    set(out "${RVK_SHADER_OUT}/${name}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RVK_SHADER_OUT}"
        COMMAND ${GLSLC} --target-env=vulkan1.3 -O -mfmt=num -fshader-stage=${stage} -D${define} -o "${out}" "${src}"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/constants.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/lighting.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/frame_lights.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/sway.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/wind.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/smoke_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/clouds_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/cloud_light.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/sky_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/atmos_common.glsl"
                "${CMAKE_CURRENT_SOURCE_DIR}/rvk/shaders/rain_common.glsl"
        COMMENT "glslc ${name}")
    list(APPEND RVK_SHADER_INCS "${out}")
endforeach()

# The cliffs' rock (rvk terrain.cpp, rvk/assets/terrain): JPEGs embedded as byte arrays.
set(RVK_ASSET_INCS)
foreach(entry "cliff_side_diff_1k.jpg|kRockAlbedo" "cliff_side_nor_gl_1k.jpg|kRockNormal" "cliff_side_arm_1k.jpg|kRockOrm")
    string(REPLACE "|" ";" asset "${entry}")
    list(GET asset 0 file)
    list(GET asset 1 name)
    set(src "${CMAKE_CURRENT_SOURCE_DIR}/rvk/assets/terrain/${file}")
    set(out "${RVK_SHADER_OUT}/${file}.inc")
    add_custom_command(OUTPUT "${out}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${RVK_SHADER_OUT}"
        COMMAND ${CMAKE_COMMAND} -DIN=${src} -DOUT=${out} -DNAME=${name} -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        DEPENDS "${src}" "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_file.cmake"
        COMMENT "embed ${file}")
    list(APPEND RVK_ASSET_INCS "${out}")
endforeach()

# FSR 2.2 (third_party/fsr2: AMD's FidelityFX Super Resolution 2, MIT; rvk/fsr2.cpp): its compute passes compiled by
# tools/gen_fsr2_shaders.py in place of AMD's Windows shader tool; its sources built quietly, our Vulkan loader's
# functions forced in (rvk/fsr2_vk_shim.h).
find_program(SPIRV_CROSS spirv-cross HINTS "${VULKAN_SDK_DIR}/bin" REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
file(GLOB FSR2_SHADER_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr2/shaders/*")
set(FSR2_SHADERS_CPP "${RVK_SHADER_OUT}/fsr2_shaders.gen.cpp")
add_custom_command(OUTPUT "${FSR2_SHADERS_CPP}"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fsr2_shaders.py" ${GLSLC} ${SPIRV_CROSS}
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/fsr2" "${FSR2_SHADERS_CPP}"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_fsr2_shaders.py" ${FSR2_SHADER_SOURCES}
    COMMENT "FSR 2 shaders")
set(FSR2_SOURCES third_party/fsr2/ffx_fsr2.cpp third_party/fsr2/ffx_assert.cpp third_party/fsr2/vk/ffx_fsr2_vk.cpp
    "${FSR2_SHADERS_CPP}")
set_source_files_properties(${FSR2_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;/FI${CMAKE_CURRENT_SOURCE_DIR}/rvk/fsr2_vk_shim.h")

# FSR 3.1 and the other effects (third_party/ffx-sdk: the FidelityFX SDK 1.1.4, MIT; rvk/fsr3.cpp; docs/fsr3-plan.md,
# docs/amd-fx-plan.md): its
# passes' permutation headers made by tools/gen_ffx_shaders.py (in place of AMD's Windows tool); built quietly, our
# Vulkan loader's functions forced in, as FSR 2's.
set(FFX_SDK "${CMAKE_CURRENT_SOURCE_DIR}/third_party/ffx-sdk")
set(FFX_HEADERS_DIR "${RVK_SHADER_OUT}/ffx")
file(GLOB_RECURSE FFX_SHADER_SOURCES "${FFX_SDK}/include/FidelityFX/gpu/*" "${FFX_SDK}/src/backends/vk/shaders/*")
add_custom_command(OUTPUT "${FFX_HEADERS_DIR}/ffx_shaders.stamp.h"
    COMMAND ${Python3_EXECUTABLE} "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ffx_shaders.py" ${GLSLC} ${SPIRV_CROSS}
            "${FFX_SDK}" "${FFX_HEADERS_DIR}"
    COMMAND ${CMAKE_COMMAND} -E touch "${FFX_HEADERS_DIR}/ffx_shaders.stamp.h"
    DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/tools/gen_ffx_shaders.py" ${FFX_SHADER_SOURCES}
    COMMENT "FidelityFX SDK shaders")
set(FFX_SOURCES
    third_party/ffx-sdk/src/components/fsr3/ffx_fsr3.cpp
    third_party/ffx-sdk/src/components/fsr3upscaler/ffx_fsr3upscaler.cpp
    third_party/ffx-sdk/src/components/frameinterpolation/ffx_frameinterpolation.cpp
    third_party/ffx-sdk/src/components/opticalflow/ffx_opticalflow.cpp
    third_party/ffx-sdk/src/components/sssr/ffx_sssr.cpp
    third_party/ffx-sdk/src/components/denoiser/ffx_denoiser.cpp
    third_party/ffx-sdk/src/components/spd/ffx_spd.cpp
    third_party/ffx-sdk/src/components/cacao/ffx_cacao.cpp
    third_party/ffx-sdk/src/components/vrs/ffx_vrs.cpp
    third_party/ffx-sdk/src/components/brixelizer/ffx_brixelizer.cpp
    third_party/ffx-sdk/src/components/brixelizer/ffx_brixelizer_raw.cpp
    third_party/ffx-sdk/src/components/brixelizergi/ffx_brixelizergi.cpp
    third_party/ffx-sdk/src/components/parallelsort/ffx_parallelsort.cpp
    third_party/ffx-sdk/src/shared/ffx_assert.cpp
    third_party/ffx-sdk/src/shared/ffx_breadcrumbs_list.cpp
    third_party/ffx-sdk/src/shared/ffx_message.cpp
    third_party/ffx-sdk/src/shared/ffx_object_management.cpp
    third_party/ffx-sdk/src/backends/shared/ffx_shader_blobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_fsr3upscaler_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_frameinterpolation_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_opticalflow_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_sssr_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_denoiser_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_spd_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_cacao_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_vrs_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_brixelizer_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_brixelizergi_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/shared/blob_accessors/ffx_parallelsort_shaderblobs.cpp
    third_party/ffx-sdk/src/backends/vk/ffx_vk.cpp
    third_party/ffx-sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK.cpp
    third_party/ffx-sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_DebugPacing.cpp
    third_party/ffx-sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_Helpers.cpp
    third_party/ffx-sdk/src/backends/vk/FrameInterpolationSwapchain/FrameInterpolationSwapchainVK_UiComposition.cpp)
set_source_files_properties(${FFX_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-w;/FI${CMAKE_CURRENT_SOURCE_DIR}/rvk/ffx_vk_shim.h"
    COMPILE_DEFINITIONS "FFX_FSR3UPSCALER;FFX_FI;FFX_OF;FFX_SSSR;FFX_DENOISER;FFX_SPD;FFX_CACAO;FFX_VRS;FFX_BRIXELIZER;FFX_BRIXELIZER_GI;FFX_PARALLEL_SORT;_SILENCE_ALL_CXX17_DEPRECATION_WARNINGS"
    INCLUDE_DIRECTORIES "${FFX_SDK}/include;${FFX_SDK}/src/shared;${FFX_SDK}/src/backends/shared;${FFX_SDK}/src/components;${FFX_HEADERS_DIR}"
    OBJECT_DEPENDS "${FFX_HEADERS_DIR}/ffx_shaders.stamp.h")
set_source_files_properties(rvk/fsr3.cpp rvk/framegen.cpp rvk/sssr.cpp rvk/cacao.cpp rvk/vrs.cpp rvk/brixelizer.cpp PROPERTIES COMPILE_OPTIONS "/imsvc${FFX_SDK}/include")   # (its headers: AMD's warnings)

add_library(rvk STATIC rvk/vk.cpp rvk/dynemu.cpp rvk/vma.cpp rvk/device.cpp rvk/resources.cpp rvk/draw.cpp rvk/objects.cpp rvk/diag.cpp rvk/shadow.cpp rvk/pointshadow.cpp rvk/hdr.cpp rvk/particles.cpp rvk/fx.cpp rvk/fx_def.cpp rvk/grading.cpp rvk/profile.cpp rvk/skin.cpp rvk/skin_gpu.cpp rvk/static_gpu.cpp rvk/prepass.cpp rvk/threaded.cpp rvk/interface.cpp rvk/handover.cpp rvk/portal_diag.cpp rvk/portal_capture.cpp rvk/portal_file.cpp rvk/portal_links.cpp rvk/portal.cpp rvk/portal_street.cpp rvk/grass.cpp rvk/leaves.cpp rvk/water.cpp rvk/water_surface.cpp rvk/rain.cpp rvk/smoke.cpp rvk/clouds.cpp rvk/meshids.cpp rvk/statels.cpp rvk/night.cpp rvk/darkness.cpp rvk/light_rig.cpp rvk/knobs.cpp rvk/sky.cpp rvk/atmosphere.cpp rvk/mist.cpp rvk/sun.cpp rvk/terrain_sun.cpp rvk/terrain.cpp rvk/native_ground.cpp rvk/ground_clutter.cpp rvk/ground_pebbles.cpp rvk/wind.cpp rvk/sand_map.cpp rvk/sand_prints.cpp rvk/fsr2.cpp rvk/fsr3.cpp rvk/framegen.cpp rvk/sssr.cpp rvk/cacao.cpp rvk/vrs.cpp rvk/brixelizer.cpp rvk/raytrace.cpp ${FSR2_SOURCES} ${FFX_SOURCES} "${FFX_HEADERS_DIR}/ffx_shaders.stamp.h" ${RVK_SHADER_INCS} ${RVK_ASSET_INCS})
target_include_directories(rvk PUBLIC rvk PRIVATE "${RVK_SHADER_OUT}" third_party third_party/fsr2)   # (stb: terrain.cpp decodes the embedded rock)
# Vulkan headers and VMA come from the SDK; /imsvc keeps their warnings quiet.
target_compile_options(rvk PUBLIC "/imsvc${VULKAN_SDK_DIR}/include")
target_compile_options(rvk PRIVATE /W3)
target_compile_definitions(rvk PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(rvk PUBLIC user32 synchronization)

add_executable(rvk_demo tests/rvk_demo.cpp proxy/native/stb_image.cpp)   # (stb: --pbr-maps, rvk/material_maps.h)
target_compile_definitions(rvk_demo PRIVATE _CRT_SECURE_NO_WARNINGS)
target_include_directories(rvk_demo PRIVATE third_party)
target_link_libraries(rvk_demo PRIVATE rvk)
target_link_options(rvk_demo PRIVATE "/MAP:${CMAKE_CURRENT_BINARY_DIR}/rvk_demo.map")   # for profiling (tools/profile-demo.sh)

# Native replacements against the original code they replace (run with tools/native-check.sh).
add_executable(native_check tests/native_check.cpp)
target_link_libraries(native_check PRIVATE randy_native)

# Crash and hang reports in the cases that matter (run with tools/crash-check.sh).
add_executable(crash_check tests/crash_check.cpp)
target_link_libraries(crash_check PRIVATE randy_native)
target_link_options(crash_check PRIVATE /LARGEADDRESSAWARE)   # (4 GB, as the game: --addr measures its address space)

# Boots the real randy31 renderer outside the game (run with tools/randy-harness.sh).
add_executable(randy_harness tests/randy_harness.cpp)
target_compile_definitions(randy_harness PRIVATE _CRT_SECURE_NO_WARNINGS)
target_link_libraries(randy_harness PRIVATE user32)
