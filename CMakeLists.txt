cmake_minimum_required(VERSION 3.10)
project(SPSC_BUFFER)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#------Fetch GoogleTest--------#
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
)

FetchContent_MakeAvailable(googletest)

#------Test Files--------#
enable_testing()

add_executable(my_test test/test_test.cpp)
target_link_libraries(my_test GTest::gtest_main)

include(GoogleTest)
gtest_discover_tests(my_test)
