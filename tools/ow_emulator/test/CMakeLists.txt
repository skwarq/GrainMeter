# SPDX-License-Identifier: MIT
cmake_minimum_required(VERSION 3.16)
project(ow_emulator_test LANGUAGES CXX)
enable_testing()
add_executable(ow_slave_test ow_slave_test.cpp ../main/ow_slave.cpp)
target_include_directories(ow_slave_test PRIVATE ../main)
target_compile_features(ow_slave_test PRIVATE cxx_std_17)
target_compile_options(ow_slave_test PRIVATE -Wall -Wextra -Werror -fno-exceptions -fno-rtti)
add_test(NAME ow_slave COMMAND ow_slave_test)
