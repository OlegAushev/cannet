# Boost for cannet's asynchronous I/O, Boost.Asio, and for the ring buffers of
# signal history, Boost.CircularBuffer; both header-only. Provides the
# INTERFACE targets cannet_asio and cannet_circular_buffer for cannet's own
# targets to link.
#
# The top-level project decides where Boost comes from: an application and
# cannet must share one Asio, because two copies cannot share an io_context.
# cannet asks only for the version it needs. A standalone build falls back to
# a pinned Boost release fetched at configure time, so a host whose
# distribution ships an older Boost — Raspberry Pi OS on Debian 13 has 1.83 —
# still builds the tools and tests. The fetch costs every build tree a
# ~100 MB download on its first configure and ~900 MB unpacked; a Boost
# installed once (e.g. under /usr/local) avoids both.

set(CANNET_BOOST_VERSION 1.90.0)
set(CANNET_BOOST_SHA256
    aca59f889f0f32028ad88ba6764582b63c916ce5f77b31289ad19421a96c555f
)

if(CANNET_STANDALONE)
    find_package(Boost ${CANNET_BOOST_VERSION} CONFIG QUIET)
    if(NOT Boost_FOUND)
        message(STATUS "Boost ${CANNET_BOOST_VERSION} not found — fetching it")
        include(FetchContent)
        set(BOOST_INCLUDE_LIBRARIES asio circular_buffer)
        set(BOOST_SKIP_INSTALL_RULES ON)
        # Asio's dependencies include compiled libraries (Boost.Context for
        # stackful coroutines) that cannet never links; keep them out of the
        # default build.
        set(cannet_boost_exclude_from_all)
        if(CMAKE_VERSION VERSION_GREATER_EQUAL 3.28)
            set(cannet_boost_exclude_from_all EXCLUDE_FROM_ALL)
        endif()
        FetchContent_Declare(Boost
            URL https://github.com/boostorg/boost/releases/download/boost-${CANNET_BOOST_VERSION}/boost-${CANNET_BOOST_VERSION}-cmake.tar.xz
            URL_HASH SHA256=${CANNET_BOOST_SHA256}
            DOWNLOAD_EXTRACT_TIMESTAMP ON
            SYSTEM
            ${cannet_boost_exclude_from_all}
        )
        FetchContent_MakeAvailable(Boost)
    endif()
else()
    find_package(Boost ${CANNET_BOOST_VERSION} CONFIG REQUIRED)
endif()

find_package(Threads REQUIRED)

add_library(cannet_asio INTERFACE)

if(TARGET Boost::asio_core)
    # Boost's own CMake build (the fetch above, or a project's
    # add_subdirectory): Asio without its optional, compiled extras.
    target_link_libraries(cannet_asio INTERFACE Boost::asio_core)
else()
    # An installed Boost: its header-only libraries come as one target.
    target_link_libraries(cannet_asio INTERFACE Boost::headers)
endif()

target_link_libraries(cannet_asio INTERFACE Threads::Threads)

add_library(cannet_circular_buffer INTERFACE)

if(TARGET Boost::circular_buffer)
    target_link_libraries(cannet_circular_buffer INTERFACE Boost::circular_buffer)
else()
    target_link_libraries(cannet_circular_buffer INTERFACE Boost::headers)
endif()
