add_executable(short_straddle short_straddle.cpp)
target_link_libraries(short_straddle PRIVATE volforge::core)

add_executable(bollinger bollinger.cpp)
target_link_libraries(bollinger PRIVATE volforge::core)

add_executable(positional positional.cpp)
target_link_libraries(positional PRIVATE volforge::core)

add_executable(store_bench store_bench.cpp)
target_link_libraries(store_bench PRIVATE volforge::core volforge::adapters volforge::store)

add_executable(convert convert.cpp)
target_link_libraries(convert PRIVATE volforge::core volforge::adapters volforge::store)
