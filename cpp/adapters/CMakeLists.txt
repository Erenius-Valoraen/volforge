add_library(volforge_adapters
    src/gfdl_csv.cpp
)

target_include_directories(volforge_adapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(volforge_adapters PUBLIC volforge::core)
target_compile_features(volforge_adapters PUBLIC cxx_std_20)

add_library(volforge::adapters ALIAS volforge_adapters)
