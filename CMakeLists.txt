cmake_minimum_required(VERSION 3.14)
project(ArchivePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

# Gui is NOT pulled in transitively (builder-cmake-extra-deps-guard-block);
# needed for QDesktopServices/QImage on the share-card path
find_package(Qt6 REQUIRED COMPONENTS Gui)

logos_module(
    NAME archive
    REP_FILE src/archive.rep
    LINK_LIBRARIES Qt6::Gui
    SOURCES
        src/archive_interface.h
        src/archive_plugin.h
        src/archive_plugin.cpp
        src/lez_client.h
        src/lez_client.cpp
        src/storage_client.h
        src/storage_client.cpp
        src/logos_storage_transport.h
        src/logos_storage_transport.cpp
        src/share_helper.h
        src/share_helper.cpp
)
