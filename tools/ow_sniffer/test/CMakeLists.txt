# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(ow_sniffer_test LANGUAGES CXX)
enable_testing()
add_executable(ow_decoder_test decoder_test.cpp ../main/ow_decoder.cpp
                               ../main/dns_reply.cpp)
target_include_directories(ow_decoder_test PRIVATE ../main)
target_compile_features(ow_decoder_test PRIVATE cxx_std_17)
target_compile_options(ow_decoder_test PRIVATE -Wall -Wextra -Werror -fno-exceptions)
add_test(NAME decoder COMMAND ow_decoder_test
         ${CMAKE_CURRENT_SOURCE_DIR}/fixtures/stw_boot_edges.csv)
