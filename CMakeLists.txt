cmake_minimum_required(VERSION 3.19)

# The version lives here and nowhere else. tapeout.json.in (the plugin metadata
# fooyin reads) and src/tapeoutversion.h.in (what the plugin reports to Tapedeck
# as its submission_client_version) are both generated from it.
project(
    tapeout
    VERSION 0.3.0
    DESCRIPTION "Tapedeck scrobbling for fooyin"
    HOMEPAGE_URL "https://github.com/abalajiksh/fooyin-tapeout"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Fooyin's config is AnyNewerVersion, so this reads as a minimum. fooyin plugins
# are ABI-coupled to the host, and nothing at load time catches a mismatch — the
# build is the only place it can be caught, so it is caught here.
find_package(Fooyin 0.12.6 REQUIRED)
find_package(Qt6 6.4 REQUIRED COMPONENTS Core Network Widgets)

configure_file(src/tapeoutversion.h.in ${CMAKE_CURRENT_BINARY_DIR}/tapeoutversion.h @ONLY)

# create_fooyin_plugin configures <name>.json.in into the build dir on its own
# and puts that dir on the include path, which is how Q_PLUGIN_METADATA finds
# the generated tapeout.json.

create_fooyin_plugin(
    tapeout
    DEPENDS Fooyin::Core
            Fooyin::Gui
            Qt6::Network
    SOURCES src/tapeoutplugin.cpp
            src/tapeoutplugin.h
            src/tapeoutconstants.h
            src/tapeoutversion.h.in
            src/tapeoutsettings.cpp
            src/tapeoutsettings.h
            src/core/listen.cpp
            src/core/listen.h
            src/core/listenqueue.cpp
            src/core/listenqueue.h
            src/core/tapedeckclient.cpp
            src/core/tapedeckclient.h
            src/core/tapeoutcontroller.cpp
            src/core/tapeoutcontroller.h
            src/settings/tapeoutpage.cpp
            src/settings/tapeoutpage.h
)

target_include_directories(tapeout PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

# Off by default: a plugin build has no reason to pay for it.
# Enable with -DTAPEOUT_TESTS=ON, then ctest --test-dir build.
option(TAPEOUT_TESTS "Build Tapeout's tests" OFF)

if(TAPEOUT_TESTS)
    enable_testing()

    add_executable(tapeout_roundtrip tests/roundtrip.cpp src/core/listen.cpp)
    target_link_libraries(tapeout_roundtrip PRIVATE Fooyin::Core Qt6::Core)
    target_include_directories(
        tapeout_roundtrip PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src ${CMAKE_CURRENT_BINARY_DIR}
    )
    # assert() is a no-op under NDEBUG, which every release build sets. Without
    # this the test passes by doing nothing at all.
    target_compile_options(tapeout_roundtrip PRIVATE -UNDEBUG)

    add_test(NAME roundtrip COMMAND tapeout_roundtrip)
endif()
