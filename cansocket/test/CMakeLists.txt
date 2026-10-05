add_executable(cansocket-tests
    test_socket.cpp
    test_async_socket.cpp
)

target_link_libraries(cansocket-tests PRIVATE
    cannet::cansocket
    Catch2::Catch2WithMain
    cannet_build_config
)

catch_discover_tests(cansocket-tests)
