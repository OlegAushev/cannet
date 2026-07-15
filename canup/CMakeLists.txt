# cannet::canup — thin wrapper over libsocketcan for CAN interface bring-up.
#
# Privilege note: up()/down() issue rtnetlink writes and therefore require
# CAP_NET_ADMIN in the calling process. status() is a read and needs no rights.
# This library holds NO privileges of its own — the caller is responsible
# for the privilege boundary.

add_library(canup
    src/canup.cpp
)

add_library(cannet::canup ALIAS canup)

target_include_directories(canup PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(canup PUBLIC cxx_std_23)

# --- libsocketcan (prefer pkg-config, fall back to plain find) ---
find_package(PkgConfig QUIET)
if(PkgConfig_FOUND)
    pkg_check_modules(SOCKETCAN IMPORTED_TARGET libsocketcan)
endif()

if(TARGET PkgConfig::SOCKETCAN)
    target_link_libraries(canup PRIVATE PkgConfig::SOCKETCAN)
else()
    find_path(SOCKETCAN_INCLUDE_DIR libsocketcan.h)
    find_library(SOCKETCAN_LIBRARY NAMES socketcan)
    if(NOT SOCKETCAN_INCLUDE_DIR OR NOT SOCKETCAN_LIBRARY)
        message(FATAL_ERROR
            "libsocketcan not found. Install it:\n"
            "  Fedora:       sudo dnf install libsocketcan-devel\n"
            "  Debian/RPiOS: sudo apt install libsocketcan-dev")
    endif()
    target_include_directories(canup PRIVATE ${SOCKETCAN_INCLUDE_DIR})
    target_link_libraries(canup PRIVATE ${SOCKETCAN_LIBRARY})
endif()

# --- canup CLI: the "test binary" you drive manually (cansniffer etc.) ---
option(CANUP_BUILD_TOOL "Build the canup CLI diagnostic tool" ${CANNET_STANDALONE})
if(CANUP_BUILD_TOOL)
    add_executable(canup-cli tool/main.cpp)
    set_target_properties(canup-cli PROPERTIES OUTPUT_NAME canup)
    target_link_libraries(canup-cli PRIVATE cannet::canup)
endif()
