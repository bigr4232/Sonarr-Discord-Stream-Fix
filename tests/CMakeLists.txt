# Google Test via FetchContent (no external dependency required)
include(FetchContent)

FetchContent_Declare(
    googletest
    GIT_REPOSITORY  https://github.com/google/googletest.git
    GIT_TAG         v1.14.0
    GIT_SHALLOW     TRUE
)

# Prevent GTest from overriding our warning settings and using its own MSVC runtime flags
set(gtest_force_shared_crt ON CACHE BOOL "" FORCE)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# Test executable - links mdd_pure.cpp directly (no WIN32/GUI dependencies)
# ---------------------------------------------------------------------------
add_executable(MuteDiscordDeviceTests
    ../mdd_pure.cpp
    test_session_fingerprint.cpp
    test_config_serialization.cpp
    test_device_name_matching.cpp
    test_utilities.cpp
    test_filter_summary.cpp
    test_device_config_defaults.cpp
    test_route_target_io.cpp
)

target_include_directories(MuteDiscordDeviceTests PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/..
)

target_link_libraries(MuteDiscordDeviceTests
    PRIVATE
        GTest::gtest
        GTest::gtest_main
        GTest::gmock
)

# Discover tests so `ctest` picks them up
include(GoogleTest)
gtest_discover_tests(MuteDiscordDeviceTests
    DISCOVERY_TIMEOUT 60
)
