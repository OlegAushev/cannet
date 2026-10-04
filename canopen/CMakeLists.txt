# cannet::canopen — protocol plane: CANopen over the transport plane.
#
# This stage holds the wire data layer only (types, SDO codecs, object
# dictionary, text conversion): pure functions and constexpr data, no I/O and
# no dependency on cansocket. The client, its services and the transport
# binding land on top of this.

add_library(canopen
    src/types.cpp
    src/sdo.cpp
    src/od.cpp
    src/od_format.cpp
)

add_library(cannet::canopen ALIAS canopen)

target_include_directories(canopen PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(canopen PUBLIC cxx_std_23)

target_link_libraries(canopen PRIVATE cannet_build_config)

if(CANNET_BUILD_TESTS)
    add_subdirectory(test)
endif()
