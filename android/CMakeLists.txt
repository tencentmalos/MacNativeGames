cmake_minimum_required(VERSION 3.22)
project(macnative_android LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT FOUNDATION_SOURCE_DIR OR NOT OBOE_SOURCE_DIR)
    message(FATAL_ERROR "Set FOUNDATION_SOURCE_DIR and OBOE_SOURCE_DIR to pinned source checkouts")
endif()
find_package(Git REQUIRED)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.json" dependency_lock)
set(pinned_dependencies foundation oboe)
if(ANDROID)
    list(APPEND pinned_dependencies volk adrenotools)
endif()
foreach(dependency IN LISTS pinned_dependencies)
    if(dependency STREQUAL "foundation")
        set(source_dir "${FOUNDATION_SOURCE_DIR}")
    elseif(dependency STREQUAL "oboe")
        set(source_dir "${OBOE_SOURCE_DIR}")
    elseif(dependency STREQUAL "volk")
        set(source_dir "${VOLK_SOURCE_DIR}")
    else()
        set(source_dir "${ADRENOTOOLS_SOURCE_DIR}")
    endif()
    string(JSON expected GET "${dependency_lock}" "${dependency}" commit)
    execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${source_dir}" rev-parse HEAD
        OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE result)
    if(NOT result EQUAL 0 OR NOT actual STREQUAL expected)
        message(FATAL_ERROR "${dependency}: checkout does not match dependencies.json")
    endif()
endforeach()
# Standalone modules: do not import Foundation's renderer or emulator services.
if(ANDROID)
    set(VOLK_PULL_IN_VULKAN OFF)
    set(VOLK_STATIC_DEFINES VK_USE_PLATFORM_ANDROID_KHR)
    add_subdirectory(${VOLK_SOURCE_DIR} ${CMAKE_BINARY_DIR}/volk)
    set_target_properties(volk PROPERTIES C_VISIBILITY_PRESET hidden)
    add_subdirectory(${ADRENOTOOLS_SOURCE_DIR} ${CMAKE_BINARY_DIR}/adrenotools)
    add_library(mng_vulkan_loader STATIC native/vulkan_driver.cpp)
    target_link_libraries(mng_vulkan_loader PUBLIC volk PRIVATE adrenotools log)
    target_include_directories(mng_vulkan_loader PUBLIC native)
    target_compile_options(mng_vulkan_loader PUBLIC "$<$<COMPILE_LANGUAGE:C,CXX>:-include${VOLK_SOURCE_DIR}/volk.h>")
    target_compile_definitions(mng_vulkan_loader PUBLIC VK_NO_PROTOTYPES VK_ENABLE_BETA_EXTENSIONS VK_USE_PLATFORM_ANDROID_KHR)
    target_compile_options(mng_vulkan_loader PRIVATE -fvisibility=hidden)
    add_dependencies(mng_vulkan_loader main_hook file_redirect_hook gsl_alloc_hook hook_impl)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../porting ${CMAKE_BINARY_DIR}/porting)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../porting/profiler ${CMAKE_BINARY_DIR}/profiler)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../porting/nvn_vulkan ${CMAKE_BINARY_DIR}/nvn-vulkan)
    add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../porting/overlay ${CMAKE_BINARY_DIR}/overlay)
    add_subdirectory(${OBOE_SOURCE_DIR} ${CMAKE_BINARY_DIR}/oboe EXCLUDE_FROM_ALL)
endif()
add_subdirectory(${FOUNDATION_SOURCE_DIR}/modules/audio ${CMAKE_BINARY_DIR}/foundation-audio)
add_subdirectory(${FOUNDATION_SOURCE_DIR}/modules/input ${CMAKE_BINARY_DIR}/foundation-input)
add_library(mng_input STATIC native/input.cpp)
target_link_libraries(mng_input PUBLIC spatial::foundation_input)
if(ANDROID)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(runtime_dir "${CMAKE_CURRENT_SOURCE_DIR}/../games/totk/runtime")
    set(android_gate "${CMAKE_CURRENT_BINARY_DIR}/asm/gate.S")
    add_custom_command(OUTPUT "${android_gate}"
        COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py"
            "${runtime_dir}/gate.S" "${android_gate}" --bionic-tls
        DEPENDS "${runtime_dir}/gate.S" "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py" VERBATIM)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../games/botw/assets/expected-v1.6.0.json" expected_botw)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../games/totk/assets/expected-v1.0.0.json" expected_totk)
    file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../games/totk/android/expected-modules.json" expected_android)
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/expected_modules.h"
        "static const char* mng_expected_botw=R\"mng(${expected_botw})mng\";\nstatic const char* mng_expected_totk=R\"mng(${expected_totk})mng\";\nstatic const char* mng_expected_android=R\"mng(${expected_android})mng\";\n")
    set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
        "${CMAKE_CURRENT_SOURCE_DIR}/../games/totk/android/expected-modules.json")
    add_library(mng_guest_runtime STATIC native/guest_runtime.cpp native/guest_call.S "${android_gate}")
    target_link_libraries(mng_guest_runtime PUBLIC mng_porting mng_profiler)
    target_include_directories(mng_guest_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${FOUNDATION_SOURCE_DIR}/third_party/nlohmann_json/include")
    add_executable(mng_runtime_probe native/guest_probe_main.cpp)
    target_link_libraries(mng_runtime_probe PRIVATE mng_guest_runtime)
    target_link_options(mng_runtime_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    # The standalone probe and dedicated APK :game process share startup. The
    # launcher process never hosts guest threads or process-exit diagnostics.
    set(startup_asm)
    foreach(stem hle_stack thread_enter)
        set(output "${CMAKE_CURRENT_BINARY_DIR}/asm/${stem}.S")
        add_custom_command(OUTPUT "${output}"
            COMMAND "${Python3_EXECUTABLE}" "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py"
                "${runtime_dir}/${stem}.S" "${output}" --bionic-tls
            DEPENDS "${runtime_dir}/${stem}.S" "${CMAKE_CURRENT_SOURCE_DIR}/tools/asm_elf.py" VERBATIM)
        list(APPEND startup_asm "${output}")
    endforeach()
    add_executable(mng_startup_probe native/guest_startup_main.cpp native/guest_runtime.cpp
        native/guest_call.S native/guest_startup_call.S "${android_gate}" ${startup_asm}
        "${runtime_dir}/threads.c" "${runtime_dir}/ipc.c" "${runtime_dir}/hle_stack.c")
    target_link_libraries(mng_startup_probe PRIVATE mng_porting mng_profiler)
    target_compile_definitions(mng_startup_probe PRIVATE MNG_LITEP_RUNTIME=1)
    target_compile_options(mng_startup_probe PRIVATE -fvisibility=hidden)
    target_compile_definitions(mng_startup_probe PRIVATE MNG_STARTUP_PROBE=1 GAME_BOTW=1 GAME_BOTW160=1)
    target_include_directories(mng_startup_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${runtime_dir}/../third_party/d2r_gfx/gfx/nvn/include"
        "${FOUNDATION_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_options(mng_startup_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    if(MNG_BOTW_ASSET_CONFIG_DIR)
        target_sources(mng_startup_probe PRIVATE native/host_platform.c native/host_input.cpp native/audio.cpp native/host_graphics.cpp native/host_frontend.cpp native/host_present.cpp native/vulkan_platform.cpp native/missing_nvn.S
            "${runtime_dir}/autoload.c"
            "${runtime_dir}/input.c" "${runtime_dir}/hid_feedback.c" "${runtime_dir}/shared_font.c"
            "${runtime_dir}/audio_renderer.c" "${runtime_dir}/audio_mixer.c"
            "${runtime_dir}/audio_effects.c" "${runtime_dir}/audio_reverb.c"
            "${runtime_dir}/services.c" "${runtime_dir}/fs_host.c" "${runtime_dir}/romfs.c"
            "${runtime_dir}/aoc_romfs.c" "${runtime_dir}/account.c" "${runtime_dir}/applet_host.c"
            "${runtime_dir}/settings_service.c" "${runtime_dir}/time_host.c")
        target_link_libraries(mng_startup_probe PRIVATE mng_botw_patches mng_nvn_vulkan mng_input mng_overlay_vulkan spatial::foundation_audio log android)
        target_compile_definitions(mng_startup_probe PRIVATE MNG_GAME_SERVICES=1)
        target_include_directories(mng_startup_probe PRIVATE "${runtime_dir}" "${MNG_BOTW_ASSET_CONFIG_DIR}" "${MNG_BOTW_ASSET_CONFIG_DIR}/.."
            "${CMAKE_CURRENT_SOURCE_DIR}/third_party/d2r")
    endif()
    add_library(macnative_platform SHARED native/jni.cpp native/audio.cpp native/game_jni.cpp native/overlay_jni.cpp native/profiler_jni.cpp)
    target_link_libraries(macnative_platform PRIVATE mng_input mng_guest_runtime mng_nvn_vulkan mng_overlay_vulkan spatial::foundation_audio log)
    if(MNG_BOTW_ASSET_CONFIG_DIR)
        # Reuse the exact standalone implementation in the dedicated :game
        # process. The normal module-only build keeps its original target.
        get_target_property(game_sources mng_startup_probe SOURCES)
        list(REMOVE_ITEM game_sources native/guest_startup_main.cpp native/audio.cpp)
        add_library(mng_game_runtime OBJECT ${game_sources})
        get_target_property(game_includes mng_startup_probe INCLUDE_DIRECTORIES)
        target_include_directories(mng_game_runtime PRIVATE ${game_includes})
        target_compile_options(mng_game_runtime PRIVATE -O2 -fvisibility=hidden)
        target_compile_definitions(mng_game_runtime PRIVATE MNG_STARTUP_PROBE=1 MNG_GAME_SERVICES=1 GAME_BOTW=1 GAME_BOTW160=1 MNG_LITEP_RUNTIME=1)
        target_link_libraries(mng_game_runtime PRIVATE mng_botw_patches mng_porting mng_input mng_nvn_vulkan mng_overlay_vulkan spatial::foundation_audio)
        target_sources(macnative_platform PRIVATE $<TARGET_OBJECTS:mng_game_runtime>)
        get_target_property(platform_links macnative_platform LINK_LIBRARIES)
        list(REMOVE_ITEM platform_links mng_guest_runtime)
        set_property(TARGET macnative_platform PROPERTY LINK_LIBRARIES "${platform_links}")
        target_link_libraries(macnative_platform PRIVATE mng_botw_patches mng_porting mng_overlay_vulkan android)
        target_compile_definitions(macnative_platform PRIVATE MNG_GAME_SERVICES=1)
    endif()
    target_link_options(macnative_platform PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    # Compile the real Zelda mixer against the Android endpoint. Linking the
    # full Horizon runtime is a separate stage; no fake service stubs here.
    add_library(mng_guest_audio OBJECT ../games/totk/runtime/audio_mixer.c)
    target_compile_options(mng_guest_audio PRIVATE -std=gnu11)
    add_dependencies(macnative_platform mng_guest_audio)
endif()
enable_testing()
add_executable(mng_input_tests native/input_test.cpp)
target_link_libraries(mng_input_tests PRIVATE mng_input)
add_test(NAME mng_input_tests COMMAND mng_input_tests)
if(ANDROID)
    add_executable(mng_profiler_tests ../porting/profiler/profiler_test.cpp ../porting/profiler/profiler_test.S "${CMAKE_CURRENT_BINARY_DIR}/asm/hle_stack.S")
    target_compile_definitions(mng_profiler_tests PRIVATE MNG_LITEP_RUNTIME=1)
    target_link_libraries(mng_profiler_tests PRIVATE mng_profiler)
endif()

add_executable(mng_audio_queue_tests native/audio_queue_test.cpp)
target_link_libraries(mng_audio_queue_tests PRIVATE spatial::foundation_audio)
