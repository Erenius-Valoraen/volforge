add_library(volforge_store
    src/codec.cpp
    src/vfday.cpp
)

target_include_directories(volforge_store PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(volforge_store PUBLIC volforge::core)
target_compile_features(volforge_store PUBLIC cxx_std_20)

add_library(volforge::store ALIAS volforge_store)
