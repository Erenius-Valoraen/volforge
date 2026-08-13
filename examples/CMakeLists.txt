add_executable(short_straddle short_straddle.cpp)
target_link_libraries(short_straddle PRIVATE volforge::core)

add_executable(bollinger bollinger.cpp)
target_link_libraries(bollinger PRIVATE volforge::core)

add_executable(positional positional.cpp)
target_link_libraries(positional PRIVATE volforge::core)
