# cannet::canopen-testing — what tests of code over cannet::canopen run
# against: an emulated device that serves as emblib's CANopen server does,
# a log of every frame on a loopback bus, and helpers to run an io_context
# until what a test expects has happened.
#
# Header-only, and for tests only: cannet's own, and those of the device
# applications built on it, which run against the emulated device on the
# loopback bus (loopback.hpp) with no interface at all.

add_library(canopen-testing INTERFACE)

add_library(cannet::canopen-testing ALIAS canopen-testing)

target_include_directories(canopen-testing INTERFACE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(canopen-testing INTERFACE cannet::canopen)
