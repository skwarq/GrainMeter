# Third-party code for the PC build only, pinned by version and SHA-256.
# The firmware uses ESP-IDF components exclusively.
include(FetchContent)

if(GM_BUILD_PC)
  # cpp-httplib: single-header HTTP server/client (MIT). Header only, no TLS:
  # the PC simulator only talks to local endpoints.
  FetchContent_Declare(httplib
    URL https://github.com/yhirose/cpp-httplib/archive/refs/tags/v0.67.0.tar.gz
    URL_HASH SHA256=be711ddaf7a3655d8f23aa5655709250b24d247aedd4b2629989d18b6164e5be
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  FetchContent_GetProperties(httplib)
  if(NOT httplib_POPULATED)
    FetchContent_Populate(httplib)
  endif()
  add_library(httplib_header INTERFACE)
  target_include_directories(httplib_header SYSTEM INTERFACE ${httplib_SOURCE_DIR})
  find_package(Threads REQUIRED)
  target_link_libraries(httplib_header INTERFACE Threads::Threads)
  if(WIN32)
    target_link_libraries(httplib_header INTERFACE ws2_32 crypt32)
  endif()
endif()

if(GM_BUILD_TESTS)
  # GoogleTest (BSD-3). Requires C++17, so tests are C++17 while the code
  # under test stays C++11.
  FetchContent_Declare(googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.18.0.tar.gz
    URL_HASH SHA256=6e3191c1455468b3fc35a417fb565c1c5071aee1b7e7f85e30cf48a98d37d8b5
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE)
  set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
  set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
  set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
  FetchContent_MakeAvailable(googletest)
endif()
