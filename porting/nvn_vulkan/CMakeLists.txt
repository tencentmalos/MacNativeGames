if(NOT ANDROID)
    message(FATAL_ERROR "The native Vulkan adapter currently requires Android")
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
add_library(mng_nvn_vulkan STATIC nvn_vulkan.cpp vulkan_self_test.cpp
    "${D2R_SOURCE_DIR}/dm/gfx/nvn/nvn_tex.c"
    "${D2R_SOURCE_DIR}/dm/gfx/tex/nvn_deswizzle.c")
target_include_directories(mng_nvn_vulkan PUBLIC . PRIVATE
    "${D2R_SOURCE_DIR}/dm/gfx/nvn/include" "${D2R_SOURCE_DIR}/dm/gfx/nvn")
target_link_libraries(mng_nvn_vulkan PUBLIC dm_vulkan_engine)
add_executable(mng_vulkan_probe vulkan_probe.cpp)
target_link_libraries(mng_vulkan_probe PRIVATE mng_nvn_vulkan)
target_include_directories(mng_vulkan_probe PRIVATE "${D2R_SOURCE_DIR}/dm/gfx/nvn/include" "${D2R_SOURCE_DIR}/dm/gfx/nvn")
target_link_options(mng_vulkan_probe PRIVATE -Wl,--no-undefined -Wl,-z,max-page-size=16384)
