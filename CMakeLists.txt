cmake_minimum_required(VERSION 3.24)

project(volforge
    VERSION 0.0.1
    DESCRIPTION "Backtesting engine for options"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "" FORCE)
endif()

option(VOLFORGE_BUILD_TESTS  "Build tests"           ON)
option(VOLFORGE_BUILD_PYTHON "Build Python bindings" ON)

if(MSVC)
    add_compile_options(/W4 /permissive- /Zc:preprocessor)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Layers are added here as they land. See docs/design.md for the architecture.
#   cpp/core  — data interface, event loop, portfolio, fills, margin, Greeks
#   cpp/ta    — streaming indicators
#   python    — strategy API bindings

add_subdirectory(cpp/core)
add_subdirectory(examples)

if(VOLFORGE_BUILD_PYTHON)
    add_subdirectory(python)
endif()

if(VOLFORGE_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
