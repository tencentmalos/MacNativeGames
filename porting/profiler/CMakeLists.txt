# One Foundation SDK owner shared by the frontend, guest services and renderer.
add_subdirectory(${FOUNDATION_SOURCE_DIR}/modules/profiler_ring ${CMAKE_CURRENT_BINARY_DIR}/foundation-ring)
# Keep profiling representative in debuggable native ports. This policy belongs
# to the shared host integration, including the streaming codec; do not change
# Foundation sources or turn off assertions/debug information to get speed.
foreach(profile_target foundation_profiler_ring profiler-sdk lz4)
    if(MSVC)
        target_compile_options(${profile_target} PRIVATE "$<$<CONFIG:Debug>:/O2>")
    else()
        target_compile_options(${profile_target} PRIVATE "$<$<CONFIG:Debug>:-O2>")
    endif()
endforeach()
set(bridge_dir ${FOUNDATION_SOURCE_DIR}/basic/modules/implements/profiler/private/spatial/profiler)
add_subdirectory(${FOUNDATION_SOURCE_DIR}/modules/debugbus ${CMAKE_CURRENT_BINARY_DIR}/foundation-debugbus)
set(guest_tags_asm ${CMAKE_CURRENT_SOURCE_DIR}/guest_tags.S)
if(ANDROID)
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    set(guest_tags_asm ${CMAKE_CURRENT_BINARY_DIR}/guest_tags_elf.S)
    add_custom_command(OUTPUT ${guest_tags_asm}
        COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/../../android/tools/asm_elf.py
            ${CMAKE_CURRENT_SOURCE_DIR}/guest_tags.S ${guest_tags_asm}
        DEPENDS guest_tags.S ../../android/tools/asm_elf.py VERBATIM)
endif()
add_library(mng_profiler STATIC debugbus.cpp profiler.cpp guest_tags.cpp ${guest_tags_asm} ${bridge_dir}/RingVulkanGpuBridge.cpp)
target_include_directories(mng_profiler PUBLIC ${CMAKE_CURRENT_SOURCE_DIR} PRIVATE ${bridge_dir})
target_link_libraries(mng_profiler PUBLIC spatial::foundation_debugbus spatial::foundation_profiler_ring mng_vulkan_loader PRIVATE mng_porting)
target_compile_options(mng_profiler PRIVATE -O2)
add_executable(mng_schedule_probe renderer_schedule_test.cpp)
target_link_libraries(mng_schedule_probe PRIVATE mng_profiler)
target_compile_options(mng_schedule_probe PRIVATE -UNDEBUG)
enable_testing()
add_test(NAME mng_schedule_probe COMMAND mng_schedule_probe)
add_executable(mng_profile_cost_probe profile_cost_probe.cpp)
target_link_libraries(mng_profile_cost_probe PRIVATE mng_profiler)
target_compile_options(mng_profile_cost_probe PRIVATE -O2 -UNDEBUG)
if(APPLE)
    target_sources(mng_profiler PRIVATE socket_debugbus.cpp)
endif()
# Explicit private fixture argument; never check game modules into the test tree.
add_executable(mng_guest_tags_probe guest_tags_test.cpp)
target_link_libraries(mng_guest_tags_probe PRIVATE mng_profiler)
target_compile_options(mng_guest_tags_probe PRIVATE -UNDEBUG)

find_package(Python3 QUIET COMPONENTS Interpreter)
if(Python3_Interpreter_FOUND)
    add_test(NAME mng_guest_tags_catalog COMMAND ${Python3_EXECUTABLE} -I
        ${CMAKE_CURRENT_SOURCE_DIR}/../tools/check_guest_tags.py)
    add_test(NAME mng_publication_cadence COMMAND ${Python3_EXECUTABLE} -I
        ${CMAKE_CURRENT_SOURCE_DIR}/../tests/test_publication_benchmark.py)
endif()
