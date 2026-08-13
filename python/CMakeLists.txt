find_package(Python 3.9 COMPONENTS Interpreter Development.Module REQUIRED)

# nanobind ships its CMake config inside the installed package.
execute_process(
    COMMAND "${Python_EXECUTABLE}" -c "import nanobind; print(nanobind.cmake_dir())"
    OUTPUT_VARIABLE nanobind_DIR
    OUTPUT_STRIP_TRAILING_WHITESPACE
    RESULT_VARIABLE nanobind_FOUND_RESULT)

if(NOT nanobind_FOUND_RESULT EQUAL 0)
    message(WARNING "nanobind not installed; skipping Python bindings. "
                    "Install with: pip install nanobind")
    return()
endif()

list(APPEND CMAKE_PREFIX_PATH "${nanobind_DIR}")
find_package(nanobind CONFIG REQUIRED)

nanobind_add_module(_volforge STABLE_ABI src/bindings.cpp)
target_link_libraries(_volforge PRIVATE volforge::core)

# Drop the extension beside the Python package so `import volforge` works from
# the source tree without an install step.
set_target_properties(_volforge PROPERTIES
    LIBRARY_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_SOURCE_DIR}/volforge"
    LIBRARY_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/volforge"
    LIBRARY_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/volforge"
    RUNTIME_OUTPUT_DIRECTORY         "${CMAKE_CURRENT_SOURCE_DIR}/volforge"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/volforge"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG   "${CMAKE_CURRENT_SOURCE_DIR}/volforge")
