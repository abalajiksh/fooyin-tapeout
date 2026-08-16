cmake_minimum_required(VERSION 3.19)

# The version lives here and nowhere else. tapeout.json.in (the plugin metadata
# fooyin reads) and src/tapeoutversion.h.in (what the plugin reports to Tapedeck
# as its submission_client_version) are both generated from it.
project(
    tapeout
    VERSION 0.1.0
    DESCRIPTION "Tapedeck scrobbling for fooyin"
    HOMEPAGE_URL "https://github.com/abalajiksh/fooyin-tapeout"
    LANGUAGES CXX
)

set(CMAKE_AUTOMOC ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(Fooyin REQUIRED)
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
