cmake_minimum_required(VERSION 3.22)
project(macnative_android LANGUAGES C CXX ASM)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)
if(NOT FOUNDATION_SOURCE_DIR OR NOT OBOE_SOURCE_DIR)
    message(FATAL_ERROR "Set FOUNDATION_SOURCE_DIR and OBOE_SOURCE_DIR to pinned source checkouts")
endif()
find_package(Git REQUIRED)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/dependencies.json" dependency_lock)
foreach(dependency foundation oboe)
    if(dependency STREQUAL "foundation")
        set(source_dir "${FOUNDATION_SOURCE_DIR}")
    else()
        set(source_dir "${OBOE_SOURCE_DIR}")
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
    add_library(mng_guest_runtime STATIC native/guest_runtime.cpp native/guest_call.S
        third_party/d2r/sha256.c "${android_gate}")
    target_include_directories(mng_guest_runtime PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${FOUNDATION_SOURCE_DIR}/third_party/nlohmann_json/include")
    add_executable(mng_runtime_probe native/guest_probe_main.cpp)
    target_link_libraries(mng_runtime_probe PRIVATE mng_guest_runtime)
    target_link_options(mng_runtime_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    # A separate process may enter SDK startup and stop at the first unsupported
    # service. Its process-exit diagnostics must never be exposed through JNI.
    add_executable(mng_startup_probe native/guest_startup_main.cpp native/guest_runtime.cpp
        native/guest_call.S native/guest_startup_call.S third_party/d2r/sha256.c "${android_gate}")
    target_compile_definitions(mng_startup_probe PRIVATE MNG_STARTUP_PROBE=1)
    target_include_directories(mng_startup_probe PRIVATE "${CMAKE_CURRENT_BINARY_DIR}"
        "${FOUNDATION_SOURCE_DIR}/third_party/nlohmann_json/include")
    target_link_options(mng_startup_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
    add_library(macnative_platform SHARED native/jni.cpp native/audio.cpp)
    target_link_libraries(macnative_platform PRIVATE mng_input mng_guest_runtime spatial::foundation_audio log)
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
