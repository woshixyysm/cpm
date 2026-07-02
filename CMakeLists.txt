cmake_minimum_required(VERSION 3.23)
project(cppm LANGUAGES CXX)

# Use C++20 (modules-aware compilers will benefit)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Sources - adjust or add more files as needed
set(CPPM_SRCS
    module_scanner.cpp
)

add_library(cppm STATIC ${CPPM_SRCS})

# Make the project root available for includes; headers appear to be in a "cppm/" subdir
target_include_directories(cppm PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

# Prefer target-level modern CMake
target_compile_features(cppm PUBLIC cxx_std_20)

# Warnings
if (MSVC)
  target_compile_options(cppm PRIVATE /W4 /permissive-)
else()
  target_compile_options(cppm PRIVATE -Wall -Wextra -Wpedantic)
endif()

# CLI tool
add_executable(cpm cli.cpp)
target_link_libraries(cpm PRIVATE cppm)
target_include_directories(cpm PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

# Option to build example executables (disabled by default)
option(CPPM_BUILD_EXAMPLES "Build example executables" OFF)
if (CPPM_BUILD_EXAMPLES)
  add_executable(example_app example/main.cpp)
  target_link_libraries(example_app PRIVATE cppm)
  target_include_directories(example_app PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
endif()

message(STATUS "Configured cppm project")
