add_library(volforge_adapters
    src/gfdl_csv.cpp
    ${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c
)

# Vendored miniz is only used to read vendor archives; volforge's own store
# depends on no compression library at all.
target_include_directories(volforge_adapters PRIVATE ${CMAKE_SOURCE_DIR}/third_party/miniz)
if(MSVC)
    set_source_files_properties(${CMAKE_SOURCE_DIR}/third_party/miniz/miniz.c
                                PROPERTIES COMPILE_FLAGS "/W0")
endif()

target_include_directories(volforge_adapters PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_link_libraries(volforge_adapters PUBLIC volforge::core)
target_compile_features(volforge_adapters PUBLIC cxx_std_20)

add_library(volforge::adapters ALIAS volforge_adapters)
