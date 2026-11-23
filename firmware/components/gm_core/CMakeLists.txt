# Shared GrainMeter logic (same sources as the PC build: core/sources.cmake).
# Compiled as C++11 without exceptions/RTTI, exactly like on the PC.
include(${CMAKE_CURRENT_LIST_DIR}/../../../core/sources.cmake)

idf_component_register(SRCS ${GM_CORE_SOURCES}
                       INCLUDE_DIRS ${GM_CORE_DIR})

idf_build_get_property(project_ver PROJECT_VER)
target_compile_definitions(${COMPONENT_LIB} PUBLIC GM_FW_VERSION="${project_ver}")
target_compile_options(${COMPONENT_LIB} PRIVATE
  -std=gnu++11 -Wall -Wextra -Wshadow -Wformat=2 -Wold-style-cast -Werror)
