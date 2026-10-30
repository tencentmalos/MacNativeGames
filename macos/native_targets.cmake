# Build the original MacNative loader/services/Metal backend directly. Private
# generated title metadata stays outside the checkout; no copied runtime fork.
if(NOT MNG_BOTW_CONFIG_DIR)
    return()
endif()
get_filename_component(native_root "${CMAKE_CURRENT_LIST_DIR}/../games/totk" ABSOLUTE)
foreach(required runtime_tables.h romfs_config.h aoc_config.h)
    if(NOT EXISTS "${MNG_BOTW_CONFIG_DIR}/${required}")
        message(FATAL_ERROR "BOTW generated metadata missing: ${required}")
    endif()
endforeach()
get_filename_component(config_parent "${MNG_BOTW_CONFIG_DIR}/.." ABSOLUTE)
file(READ "${native_root}/third_party/d2r_gfx/gfx/nvn/dmsh_chain.h" shader_chain_source)
string(REGEX MATCH "DMSH_CHAIN \"([^\"]+)\"" shader_chain_match "${shader_chain_source}")
set(shader_chain "${CMAKE_MATCH_1}")
if(NOT shader_chain)
    message(FATAL_ERROR "Unable to resolve native Metal shader chain")
endif()
file(SHA256 "${native_root}/prebuilt/macos-arm64/totk_xlate" translator_sha)
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/mng-metal-assets.json"
    "{\"chain\":\"${shader_chain}\",\"translator_sha256\":\"${translator_sha}\",\"game\":\"botw\",\"version\":\"botw160\"}\n")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${native_root}/third_party/d2r_gfx/gfx/nvn/dmsh_chain.h"
    "${native_root}/prebuilt/macos-arm64/totk_xlate")

find_path(OPUS_INCLUDE_DIR opus.h PATH_SUFFIXES opus HINTS /opt/homebrew/opt/opus/include REQUIRED)
find_library(OPUS_LIBRARY NAMES opus HINTS /opt/homebrew/opt/opus/lib REQUIRED)
find_path(VPX_INCLUDE_DIR vpx/vpx_decoder.h HINTS /opt/homebrew/opt/libvpx/include REQUIRED)
find_library(VPX_LIBRARY NAMES vpx HINTS /opt/homebrew/opt/libvpx/lib REQUIRED)
set(runtime_sources loader.c game_patches_botw.c aoc_romfs.c settings_service.c
    world_survey.c game_slot.c services.c romfs.c threads.c gate.S hle_stack.S hle_stack.c
    thread_enter.S frontend.m account.c input.c hid_feedback.c motion_fusion.c
    autoload.c gamepad.m gamepad_feedback.m memory_probe.c shared_font.c ipc.c
    audio_renderer.c audio_mixer.c audio_reverb.c audio_effects.c fs_host.c
    opus_codec.c movie_host.c time_host.c applet_host.c swkbd_host.c)
list(TRANSFORM runtime_sources PREPEND "${native_root}/runtime/")
set(mac_frameworks Metal MetalFX Foundation QuartzCore ImageIO CoreGraphics CoreText
    UniformTypeIdentifiers Cocoa CoreVideo CoreFoundation AudioToolbox GameController CoreHaptics IOKit)
function(mng_native_target target backend)
    target_compile_definitions(${target} PRIVATE GAME_BOTW=1 GAME_BOTW160=1
        MNG_LITEP_RUNTIME=1 MNG_PORT_HOST=1 "MNG_HOST_BACKEND=\"${backend}\"")
    target_compile_options(${target} PRIVATE -O2 -g -fno-strict-aliasing
        "$<$<COMPILE_LANGUAGE:OBJC>:-fobjc-arc>" -Wno-deprecated-declarations)
    target_include_directories(${target} PRIVATE "${native_root}/runtime"
        "${native_root}/third_party/d2r_gfx/gfx/nvn/include"
        "${MNG_BOTW_CONFIG_DIR}" "${config_parent}" "${OPUS_INCLUDE_DIR}" "${VPX_INCLUDE_DIR}")
    target_link_libraries(${target} PRIVATE mng_porting mng_profiler "${OPUS_LIBRARY}" "${VPX_LIBRARY}")
    foreach(framework IN LISTS mac_frameworks)
        target_link_libraries(${target} PRIVATE "-framework ${framework}")
    endforeach()
endfunction()
file(GLOB metal_sources CONFIGURE_DEPENDS "${native_root}/third_party/d2r_gfx/gfx/nvn/*.c"
    "${native_root}/third_party/d2r_gfx/gfx/nvn/*.m" "${native_root}/third_party/d2r_gfx/gfx/nvn/*.S"
    "${native_root}/third_party/d2r_gfx/gfx/tex/*.c" "${native_root}/third_party/d2r_gfx/gfx/tex/*.m")
add_executable(mng_botw_metal ${runtime_sources} ${metal_sources} host_diagnostics.cpp
    "${native_root}/runtime/gfx_bridge.c" "${native_root}/runtime/gfx_probe.c"
    "${native_root}/runtime/vpool_probe.m" "${native_root}/runtime/gpu_path_probe.m")
mng_native_target(mng_botw_metal metal)
add_executable(mng_botw_vulkan ${runtime_sources} host_diagnostics.cpp vulkan_platform.mm
    vulkan_runtime.cpp missing_nvn.S "${native_root}/third_party/d2r_gfx/gfx/nvn/fps_overlay.c" ../porting/nvn_vulkan/host_graphics.cpp
    ../porting/nvn_vulkan/host_present.cpp)
target_compile_definitions(mng_botw_vulkan PRIVATE MNG_BACKEND_VULKAN=1)
target_compile_options(mng_botw_vulkan PRIVATE "$<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>")
target_link_libraries(mng_botw_vulkan PRIVATE mng_nvn_vulkan mng_overlay_vulkan)
mng_native_target(mng_botw_vulkan vulkan)
