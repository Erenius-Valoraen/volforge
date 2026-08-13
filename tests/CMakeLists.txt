add_executable(volforge_tests
    main.cpp
    test_data_interface.cpp
    test_event_loop.cpp
    test_lookahead.cpp
    test_risk_rules.cpp
    test_greeks.cpp
    test_accounting.cpp
    test_margin.cpp
    test_backtest.cpp
    test_gfdl.cpp
    test_codec.cpp
    test_vfday.cpp
)

target_link_libraries(volforge_tests PRIVATE volforge::core volforge::adapters volforge::store)
target_include_directories(volforge_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})

add_test(NAME volforge_tests COMMAND volforge_tests)
