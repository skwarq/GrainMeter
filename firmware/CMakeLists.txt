# GrainMeter firmware - ESP-IDF v6.1.1 (pinned, see main/idf_component.yml
# and the CI/Docker image espressif/idf:v6.1.1), target esp32c6.
cmake_minimum_required(VERSION 3.22)

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../VERSION" PROJECT_VER LIMIT_COUNT 1)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
# The project name is checked by the OTA upload ("grainmeter").
project(grainmeter)
