cmake_minimum_required(VERSION 3.14)
project(ArchiveBackendTests LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)

find_package(Qt6 REQUIRED COMPONENTS Core Network Test)

add_executable(tst_lez_client
    tst_lez_client.cpp
    mock_node.cpp
    mock_node.h
    ../../src/lez_client.cpp
    ../../src/lez_client.h
)
target_include_directories(tst_lez_client PRIVATE ../../src)
target_link_libraries(tst_lez_client PRIVATE Qt6::Core Qt6::Network Qt6::Test)

add_executable(tst_storage_client
    tst_storage_client.cpp
    mock_storage.cpp
    mock_storage.h
    ../../src/storage_client.cpp
    ../../src/storage_client.h
)
target_include_directories(tst_storage_client PRIVATE ../../src)
target_link_libraries(tst_storage_client PRIVATE Qt6::Core Qt6::Network Qt6::Test)

enable_testing()
add_test(NAME lez_client COMMAND tst_lez_client)
add_test(NAME storage_client COMMAND tst_storage_client)
