cmake_minimum_required(VERSION 3.14)
project(ArchivePlugin LANGUAGES CXX)

if(DEFINED ENV{LOGOS_MODULE_BUILDER_ROOT})
    include($ENV{LOGOS_MODULE_BUILDER_ROOT}/cmake/LogosModule.cmake)
else()
    message(FATAL_ERROR "LogosModule.cmake not found. Set LOGOS_MODULE_BUILDER_ROOT.")
endif()

logos_module(
    NAME archive
    SOURCES
        src/plugin/ArchivePlugin.h
        src/plugin/ArchivePlugin.cpp
        src/lez_client.h
        src/lez_client.cpp
        src/storage_client.h
        src/storage_client.cpp
        src/logos_storage_transport.h
        src/logos_storage_transport.cpp
        src/share_helper.h
        src/share_helper.cpp
        src/generated/storage_module_api.cpp
    INCLUDE_DIRS
        src
        src/generated
)
