cmake_minimum_required(VERSION 3.13)

project(cannet
    VERSION 0.1.0
    DESCRIPTION "CAN networking library family"
    LANGUAGES CXX
)

# Standalone build (this is the top-level project) vs embedded via add_subdirectory().
if(CMAKE_SOURCE_DIR STREQUAL CMAKE_CURRENT_SOURCE_DIR)
    set(CANNET_STANDALONE ON)
else()
    set(CANNET_STANDALONE OFF)
endif()

# --- build policy: warnings and hardening for cannet's own targets ---
#
# Attached PRIVATE by every cannet target, never PUBLIC: a project that embeds
# cannet via add_subdirectory keeps its own warning policy, and cannet's
# headers must compile cleanly under someone else's flags. For the same reason
# -Werror is on only in a standalone build — a newer compiler must not break a
# consumer's build over a new warning.

option(CANNET_WERROR "Treat compiler warnings as errors" ${CANNET_STANDALONE})
option(CANNET_SANITIZE "Build with AddressSanitizer + UBSan" OFF)

add_library(cannet_build_config INTERFACE)

target_compile_options(cannet_build_config INTERFACE
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wdouble-promotion
    -Wformat=2
    -Wformat-truncation
    -Wundef
    -fno-common
    -fstack-protector-strong
    $<$<BOOL:${CANNET_WERROR}>:-Werror>
    $<$<COMPILE_LANGUAGE:CXX>:
        -Wconversion
        -Wsign-conversion
        -Wsign-compare
        -Wold-style-cast
        -Wuseless-cast
        -Wsuggest-override
        -Wnon-virtual-dtor
        -Woverloaded-virtual>)

target_compile_definitions(cannet_build_config INTERFACE
    # libstdc++ precondition checks (container bounds, iterator validity).
    $<$<CONFIG:Debug>:_GLIBCXX_ASSERTIONS>
    # _FORTIFY_SOURCE needs an optimized build to do anything.
    $<$<NOT:$<CONFIG:Debug>>:_FORTIFY_SOURCE=3>)

if(CANNET_SANITIZE)
    target_compile_options(cannet_build_config INTERFACE
        -fsanitize=address,undefined
        -fno-omit-frame-pointer)
    target_link_options(cannet_build_config INTERFACE
        -fsanitize=address,undefined)
endif()

# --- config plane: interface bring-up (privileged, needs CAP_NET_ADMIN) ---
add_subdirectory(canup)

# --- transport plane: CAN frame I/O (unprivileged); raw now, isotp/j1939 later ---
add_subdirectory(cansocket)

# --- protocol plane: CANopen over the transport (unprivileged) ---
# TODO: move canopen here as cannet::canopen (depends on cannet::cansocket)
# add_subdirectory(canopen)
