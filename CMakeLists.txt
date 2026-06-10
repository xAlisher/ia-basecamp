cmake_minimum_required(VERSION 3.14)
project(ArchivePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME archive
    REP_FILE src/archive.rep
    SOURCES
        src/archive_interface.h
        src/archive_plugin.h
        src/archive_plugin.cpp
        src/lez_client.h
        src/lez_client.cpp
        src/storage_client.h
        src/storage_client.cpp
)
