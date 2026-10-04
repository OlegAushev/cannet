# Catch2 v3 for the unit tests: an installed one if the system has it,
# otherwise a pinned source build fetched at configure time (needs network on
# the first configure only). Tests are standalone-build only, so a project
# embedding cannet never reaches this file.

find_package(Catch2 3 QUIET)

if(NOT Catch2_FOUND)
    message(STATUS "Catch2 not found — fetching v${CANNET_CATCH2_VERSION}")
    include(FetchContent)
    FetchContent_Declare(Catch2
        GIT_REPOSITORY https://github.com/catchorg/Catch2.git
        GIT_TAG v${CANNET_CATCH2_VERSION}
        GIT_SHALLOW TRUE
        SYSTEM
    )
    FetchContent_MakeAvailable(Catch2)
    list(APPEND CMAKE_MODULE_PATH ${catch2_SOURCE_DIR}/extras)
endif()

# Provides catch_discover_tests(): one CTest entry per TEST_CASE.
include(Catch)
