add_library(volforge_core
    src/types.cpp
    src/instrument.cpp
    src/quotes.cpp
    src/data_source.cpp
    src/memory_source.cpp
    src/synthetic.cpp
    src/condition.cpp
    src/portfolio.cpp
    src/strategy.cpp
    src/event_loop.cpp
)

target_include_directories(volforge_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
)

target_compile_features(volforge_core PUBLIC cxx_std_20)

add_library(volforge::core ALIAS volforge_core)
