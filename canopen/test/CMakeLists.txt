add_executable(canopen-tests
    test_types.cpp
    test_sdo.cpp
    test_od.cpp
    test_od_format.cpp
    test_od_file.cpp
    test_od_tools.cpp
    test_od_header.cpp
    test_event.cpp
    test_client.cpp
    test_remote_node.cpp
    test_sdo_client.cpp
    test_watch.cpp
    test_snapshot.cpp
    test_config.cpp
    test_history.cpp
    test_loopback.cpp
    test_raw_transport.cpp
)

target_link_libraries(canopen-tests PRIVATE
    cannet::canopen
    cannet::canopen-history
    canopen_od_tools
    Catch2::Catch2WithMain
    cannet_test_support
    cannet_build_config
)

cannet_canopen_dictionary(canopen-tests
    FILE drive.od
    NAME drive_dictionary
    NAMESPACE cannet_test
)

catch_discover_tests(canopen-tests)

# The CLI itself, on an OD file it takes and on one it must refuse.
add_test(NAME "canopen od check takes a sound OD file"
    COMMAND canopen-cli od check ${CMAKE_CURRENT_SOURCE_DIR}/drive.od
)
add_test(NAME "canopen od check refuses a broken OD file"
    COMMAND canopen-cli od check ${CMAKE_CURRENT_SOURCE_DIR}/broken.od
)
set_tests_properties("canopen od check refuses a broken OD file"
    PROPERTIES WILL_FAIL ON
)
