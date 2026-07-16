# cannet::cansocket — transport plane: raw CAN frame I/O over an already-up
# interface (open/bind, send, recv). Unprivileged; interface configuration
# (bitrate, up/down) belongs to cannet::canup.

add_library(cansocket
    src/socket.cpp
)

add_library(cannet::cansocket ALIAS cansocket)

target_include_directories(cansocket PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_compile_features(cansocket PUBLIC cxx_std_23)

# --- cansock CLI: the "test binary" you drive manually (dump/send) ---
option(CANSOCKET_BUILD_TOOL "Build the cansock CLI diagnostic tool"
       ${CANNET_STANDALONE}
)

if(CANSOCKET_BUILD_TOOL)
    add_executable(cansocket-cli tool/main.cpp)
    set_target_properties(cansocket-cli PROPERTIES OUTPUT_NAME cansock)
    target_link_libraries(cansocket-cli PRIVATE cannet::cansocket)
endif()
