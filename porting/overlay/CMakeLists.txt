# The host owns ImGui and the Vulkan dispatch table. Foundation supplies the
# semantic shell, controls and metrics; no emulator or XR runtime is imported.
include(../cmake/fmt.cmake)
file(READ "${CMAKE_CURRENT_SOURCE_DIR}/../../android/dependencies.json" lock)
string(JSON expected GET "${lock}" freetype commit)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${FREETYPE_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
    message(FATAL_ERROR "FreeType source must match dependencies.json")
endif()
foreach(dependency ZLIB BZIP2 PNG HARFBUZZ BROTLI)
    set(FT_DISABLE_${dependency} ON CACHE BOOL "" FORCE)
endforeach()
add_subdirectory(${FREETYPE_SOURCE_DIR} ${CMAKE_CURRENT_BINARY_DIR}/freetype EXCLUDE_FROM_ALL)
string(JSON expected GET "${lock}" imgui commit)
execute_process(COMMAND "${GIT_EXECUTABLE}" -C "${IMGUI_SOURCE_DIR}" rev-parse HEAD
    OUTPUT_VARIABLE actual OUTPUT_STRIP_TRAILING_WHITESPACE RESULT_VARIABLE status)
if(NOT status EQUAL 0 OR NOT actual STREQUAL expected)
    message(FATAL_ERROR "Host ImGui source must match dependencies.json")
endif()
set(imgui_dir "${IMGUI_SOURCE_DIR}")
add_library(mng_imgui STATIC ${imgui_dir}/imgui.cpp ${imgui_dir}/imgui_draw.cpp
    ${imgui_dir}/imgui_tables.cpp ${imgui_dir}/imgui_widgets.cpp ${imgui_dir}/misc/freetype/imgui_freetype.cpp)
target_compile_definitions(mng_imgui PUBLIC IMGUI_ENABLE_FREETYPE)
target_link_libraries(mng_imgui PRIVATE freetype)
target_include_directories(mng_imgui PUBLIC ${imgui_dir})
# This pin's perf CMake assumes the full Foundation Android platform. Use the
# same two portable sources with the host's BatterySource adapter instead.
add_library(foundation_perf_metrics STATIC
    ${FOUNDATION_SOURCE_DIR}/modules/perf_metrics/src/DeviceMetrics.cpp
    ${FOUNDATION_SOURCE_DIR}/modules/perf_metrics/src/PerfSettings.cpp battery.cpp)
add_library(spatial::foundation_perf_metrics ALIAS foundation_perf_metrics)
target_include_directories(foundation_perf_metrics PUBLIC ${FOUNDATION_SOURCE_DIR}/modules/perf_metrics/include)
find_package(Threads REQUIRED)
target_link_libraries(foundation_perf_metrics PUBLIC Threads::Threads)
set(FOUNDATION_OVERLAY_HOST_IMGUI_TARGET mng_imgui CACHE STRING "" FORCE)
add_subdirectory(${FOUNDATION_SOURCE_DIR}/modules/imgui_overlay ${CMAKE_CURRENT_BINARY_DIR}/foundation-overlay)
add_library(mng_overlay STATIC overlay.cpp)
target_include_directories(mng_overlay PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_include_directories(mng_overlay PRIVATE ${FOUNDATION_SOURCE_DIR}/third_party/nlohmann_json/include)
target_link_libraries(mng_overlay PUBLIC spatial::foundation_imgui_overlay PRIVATE mng_imgui)
if(ANDROID OR APPLE)
    add_library(mng_overlay_vulkan STATIC vulkan.cpp sdf_vulkan.cpp ${imgui_dir}/backends/imgui_impl_vulkan.cpp)
    target_compile_definitions(mng_overlay_vulkan PRIVATE IMGUI_IMPL_VULKAN_USE_VOLK)
    target_include_directories(mng_overlay_vulkan PRIVATE ${imgui_dir}/backends)
    target_link_libraries(mng_overlay_vulkan PUBLIC mng_overlay mng_vulkan_loader PRIVATE mng_imgui)
endif()
