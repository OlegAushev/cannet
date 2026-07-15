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

# --- config plane: interface bring-up (privileged, needs CAP_NET_ADMIN) ---
add_subdirectory(canup)

# --- transport plane: raw frame I/O (unprivileged) ---
# TODO: move the slimmed-down can::Socket (bind/send/recv) here as cannet::cansocket
# add_subdirectory(cansocket)

# --- protocol plane: CANopen over the transport (unprivileged) ---
# TODO: move canopen here as cannet::canopen (depends on cannet::cansocket)
# add_subdirectory(canopen)
