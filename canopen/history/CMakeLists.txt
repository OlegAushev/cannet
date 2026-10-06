# cannet::canopen-history — signal history for plots, over cannet::canopen.
#
# A target of its own: only a GUI needs it, and a headless application or
# the CLI does not link it. Boost.CircularBuffer stays inside.

add_library(canopen-history
    src/history.cpp
)

add_library(cannet::canopen-history ALIAS canopen-history)

target_include_directories(canopen-history PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(canopen-history PUBLIC cxx_std_23)

target_link_libraries(canopen-history
    PUBLIC cannet::canopen
    PRIVATE cannet_circular_buffer cannet_build_config
)
