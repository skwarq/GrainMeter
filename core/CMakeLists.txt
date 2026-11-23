# Shared GrainMeter logic (PC + ESP32). The ESP-IDF build compiles the same
# files through firmware/components/gm_core.
include(${CMAKE_CURRENT_LIST_DIR}/sources.cmake)

add_library(gm_core STATIC ${GM_CORE_SOURCES})
target_include_directories(gm_core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
set_target_properties(gm_core PROPERTIES CXX_STANDARD 11 CXX_STANDARD_REQUIRED ON CXX_EXTENSIONS OFF)
target_compile_definitions(gm_core PUBLIC GM_FW_VERSION="${PROJECT_VERSION}")
gm_set_warnings(gm_core)
gm_no_exceptions(gm_core)
