if(NOT ANDROID AND NOT APPLE)
    message(FATAL_ERROR "The native Vulkan adapter supports Android and macOS")
endif()
if(NOT D2R_SOURCE_DIR)
    message(FATAL_ERROR "Set D2R_SOURCE_DIR to the pinned D2R Vulkan source checkout")
endif()
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../android/dependencies.json" lock)
string(JSON expected GET "${lock}" d2r_vulkan commit)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${D2R_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
    message(FATAL_ERROR "D2R Vulkan source must match dependencies.json")
endif()
# Read-only, MIT-licensed renderer. Do not link the D2R host, patches or HLE.
add_subdirectory("${D2R_SOURCE_DIR}/dm/gfx/vulkan" "${CMAKE_CURRENT_BINARY_DIR}/engine")
# Every engine, presenter and probe call shares one selected loader. Never mix
# system Vulkan imports with custom-driver dispatchable handles.
add_subdirectory(../jobs "${CMAKE_CURRENT_BINARY_DIR}/host-jobs")
set(engine_links mng_vulkan_loader mng_profiler mng_host_jobs)
if(ANDROID)
    list(APPEND engine_links log)
endif()
set_property(TARGET dm_vulkan_engine PROPERTY LINK_LIBRARIES "${engine_links}")
set_property(TARGET dm_vulkan_engine PROPERTY INTERFACE_LINK_LIBRARIES "${engine_links}")
find_package(Python3 REQUIRED COMPONENTS Interpreter)
set(engine_original "${D2R_SOURCE_DIR}/dm/gfx/vulkan/engine/xbg_game.cpp")
set(engine_adapted "${CMAKE_CURRENT_BINARY_DIR}/adapted/xbg_game.cpp")
execute_process(COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/prepare_engine.py"
    "${engine_original}" "${engine_adapted}" COMMAND_ERROR_IS_FATAL ANY)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare_engine.py" "${engine_original}"
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare_warm_jobs.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare_preparation.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/prepare_live_flush.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/live_ranges.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/preparation_plan.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/preparation_plan_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/preparation_key_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/warm_jobs_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/runtime_policy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/translation_options.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/memory_policy.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/image_storage_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_archive.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_archive_engine.inc"
    "${D2R_SOURCE_DIR}/dm/gfx/vulkan/engine/xbg_texture.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/host_thread.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/host_clock.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/host_import.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/target_stats_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/guest_clock_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/read_preflight_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/read_hazard_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/read_range_cache_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/feedback_snapshot_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/fragment_barrier_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/draw_plan_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/gpu_clock.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/serial_handoff.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/ordered_executor.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/../runtime/submission_inputs.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/ordered_submission_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/image_storage_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_variants.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/border_shader.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/border_shader_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/attachment_alias_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/linear_texture.h"
    "${D2R_SOURCE_DIR}/dm/gfx/nvn/nvn_tex.c"
    "${D2R_SOURCE_DIR}/dm/gfx/tex/nvn_deswizzle.c"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_probe_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/texture_source_probe_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/render_record_engine.inc"
    "${CMAKE_CURRENT_SOURCE_DIR}/shader_variants_engine.inc")
get_target_property(engine_sources dm_vulkan_engine SOURCES)
list(REMOVE_ITEM engine_sources "${engine_original}" "${D2R_SOURCE_DIR}/dm/gfx/vulkan/engine/xbg_shader_key.c" "${D2R_SOURCE_DIR}/dm/gfx/vulkan/engine/xbg_texture.c")
list(APPEND engine_sources "${engine_adapted}" "${CMAKE_CURRENT_BINARY_DIR}/adapted/xbg_shader_key.c" "${CMAKE_CURRENT_BINARY_DIR}/adapted/xbg_texture.c")
set_source_files_properties("${CMAKE_CURRENT_BINARY_DIR}/adapted/xbg_texture.c" TARGET_DIRECTORY dm_vulkan_engine PROPERTIES COMPILE_OPTIONS -fsigned-char)
set_property(TARGET dm_vulkan_engine PROPERTY SOURCES "${engine_sources}")

add_library(mng_nvn_vulkan STATIC nvn_vulkan.cpp vulkan_self_test.cpp render_record_vulkan.cpp
    "${CMAKE_CURRENT_BINARY_DIR}/adapted/nvn_tex.c"
    "${CMAKE_CURRENT_BINARY_DIR}/adapted/nvn_deswizzle.c")
target_include_directories(mng_nvn_vulkan PUBLIC . PRIVATE
    "${D2R_SOURCE_DIR}/dm/gfx/nvn/include" "${D2R_SOURCE_DIR}/dm/gfx/nvn")
target_compile_options(mng_nvn_vulkan PRIVATE -O2 -fno-strict-aliasing)
target_link_libraries(mng_nvn_vulkan PUBLIC dm_vulkan_engine mng_porting)
add_executable(mng_vulkan_probe vulkan_probe.cpp)
target_link_libraries(mng_vulkan_probe PRIVATE mng_nvn_vulkan)
target_include_directories(mng_vulkan_probe PRIVATE "${D2R_SOURCE_DIR}/dm/gfx/nvn/include" "${D2R_SOURCE_DIR}/dm/gfx/nvn")
if(ANDROID)
    target_link_options(mng_vulkan_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
elseif(APPLE)
    # Darwin requires explicitly allowed unresolved optional imports even when
    # the object marks them weak. Keep the list narrow; every use checks null.
    foreach(optional dm_bright_k dm_prof_add nvn_dirty_now nvn_dirty_protect
        nvn_dirty_since nvn_dirty_slot_stats nvn_dirty_tracked nvn_pool_flags
        mng_android_wait_display_tick mng_game_frame_step mng_game_present_period
        runtime_thread_current_handle)
        target_link_options(mng_nvn_vulkan INTERFACE "-Wl,-U,_${optional}")
    endforeach()
endif()

add_executable(mng_memory_policy_probe ../tests/memory_policy_test.cpp)
target_link_libraries(mng_memory_policy_probe PRIVATE mng_vulkan_loader)
target_compile_options(mng_memory_policy_probe PRIVATE -UNDEBUG)
add_test(NAME mng_memory_policy_probe COMMAND mng_memory_policy_probe)
