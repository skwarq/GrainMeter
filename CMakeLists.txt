cmake_minimum_required(VERSION 3.25)

file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" GM_VERSION LIMIT_COUNT 1)
project(GrainMeter VERSION ${GM_VERSION} LANGUAGES CXX)

option(GM_BUILD_PC "Build the PC simulator" ON)
option(GM_BUILD_TESTS "Build unit and integration tests" ON)
option(GM_WERROR "Treat warnings in own code as errors" OFF)
option(GM_PC_TLS "PC simulator: HTTPS via OpenSSL (real ThingSpeak account)" OFF)

include(cmake/Warnings.cmake)
include(cmake/Dependencies.cmake)

add_subdirectory(core)
if(GM_BUILD_PC)
  add_subdirectory(pc)
endif()
if(GM_BUILD_TESTS)
  enable_testing()
  add_subdirectory(tests)
endif()
