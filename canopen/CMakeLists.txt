# cannet::canopen — protocol plane: CANopen over the transport plane.
#
# The wire data layer (types, SDO codecs, object dictionary, text
# conversion: pure functions and constexpr data); the transport the
# services talk to — an interface with a CAN_RAW implementation over
# cannet::cansocket and an in-memory one for tests; and on top of it the
# client, its remote nodes and their services.

add_library(canopen
    src/types.cpp
    src/sdo.cpp
    src/od.cpp
    src/od_format.cpp
    src/subscribers.cpp
    src/subscription.cpp
    src/transport.cpp
    src/raw_transport.cpp
    src/loopback.cpp
    src/timers.cpp
    src/hb_consumer.cpp
    src/emcy_consumer.cpp
    src/tpdo_consumer.cpp
    src/rpdo_producer.cpp
    src/sdo_client.cpp
    src/remote_node.cpp
    src/client.cpp
)

add_library(cannet::canopen ALIAS canopen)

target_include_directories(canopen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(canopen PUBLIC cxx_std_23)

target_link_libraries(canopen
    PUBLIC cannet::cansocket cannet_asio
    PRIVATE cannet_build_config
)

if(CANNET_BUILD_TESTS)
    add_subdirectory(test)
endif()
