add_executable(volforge_tests
    main.cpp
    test_data_interface.cpp
)

target_link_libraries(volforge_tests PRIVATE volforge::core)
target_include_directories(volforge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME volforge_tests COMMAND volforge_tests)
