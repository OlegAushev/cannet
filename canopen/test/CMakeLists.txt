add_executable(canopen-tests
    test_types.cpp
    test_sdo.cpp
    test_od.cpp
    test_od_format.cpp
    test_event.cpp
    test_client.cpp
    test_remote_node.cpp
    test_loopback.cpp
    test_raw_transport.cpp
)

target_link_libraries(canopen-tests PRIVATE
    cannet::canopen
    Catch2::Catch2WithMain
    cannet_test_support
    cannet_build_config
)

catch_discover_tests(canopen-tests)
