# cannet::canopen — protocol plane: CANopen over the transport plane.
#
# The wire data layer (types, SDO codecs, object dictionary, text
# conversion: pure functions and constexpr data); the transport the
# services talk to — an interface with a CAN_RAW implementation over
# cannet::cansocket and an in-memory one for tests; and on top of it the
# client, its remote nodes and their services. Signal history for plots is
# a target of its own, cannet::canopen-history, in history/.

add_library(canopen
    src/types.cpp
    src/sdo.cpp
    src/od.cpp
    src/od_format.cpp
    src/od_file.cpp
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
    src/watch.cpp
    src/watch_snapshot.cpp
    src/config.cpp
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

add_subdirectory(history)

# --- canopen CLI: the protocol's diagnostic tool, and the dictionary tools ---
# Built in a standalone build. In a project that embeds cannet, only when
# cannet_canopen_dictionary() needs it to generate a header.
option(CANOPEN_BUILD_TOOL "Build the canopen CLI diagnostic tool"
       ${CANNET_STANDALONE}
)

add_library(canopen_od_tools STATIC tool/od_tools.cpp)
target_include_directories(canopen_od_tools PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/tool
)
target_link_libraries(canopen_od_tools
    PUBLIC cannet::canopen
    PRIVATE cannet_build_config
)

add_executable(canopen-cli tool/main.cpp)
set_target_properties(canopen-cli PROPERTIES OUTPUT_NAME canopen)
target_link_libraries(canopen-cli PRIVATE canopen_od_tools cannet_build_config)

if(NOT CANOPEN_BUILD_TOOL)
    set_target_properties(canopen_od_tools canopen-cli PROPERTIES
        EXCLUDE_FROM_ALL ON
    )
endif()

include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/canopen_dictionary.cmake)

if(CANNET_BUILD_TESTS)
    add_subdirectory(test)
endif()
