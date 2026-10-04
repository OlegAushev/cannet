add_executable(canopen-tests
    test_types.cpp
    test_sdo.cpp
    test_od.cpp
    test_od_format.cpp
)

target_link_libraries(canopen-tests PRIVATE
    cannet::canopen
    Catch2::Catch2WithMain
    cannet_build_config
)

catch_discover_tests(canopen-tests)
