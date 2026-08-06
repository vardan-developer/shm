cmake_minimum_required(VERSION 3.14)
project(SPSC_BUFFER CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

#------Fetch GoogleTest--------#
include(FetchContent)
FetchContent_Declare(
    googletest
    URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    DOWNLOAD_EXTRACT_TIMESTAMP TRUE
)

FetchContent_MakeAvailable(googletest)

#------Test Files--------#
enable_testing()
include(GoogleTest)

file(GLOB TEST_SOURCES
    CONFIGURE_DEPENDS "${CMAKE_CURRENT_LIST_DIR}/test/*.t.cpp")

foreach(test_src IN LISTS TEST_SOURCES)
    get_filename_component(test_name "${test_src}" NAME_WE)
    add_executable(${test_name} ${test_src})
    target_include_directories(${test_name}
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(${test_name} PRIVATE GTest::gtest_main)
    set_target_properties(${test_name} PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/tests
    )
    gtest_discover_tests(${test_name})

    add_executable(${test_name}_tsan ${test_src})
    target_include_directories(${test_name}_tsan
        PRIVATE "${CMAKE_CURRENT_LIST_DIR}/include")
    target_link_libraries(${test_name}_tsan PRIVATE GTest::gtest_main)
    set_target_properties(${test_name}_tsan PROPERTIES
      RUNTIME_OUTPUT_DIRECTORY ${CMAKE_SOURCE_DIR}/bin/tests_tsan
    )
    target_compile_options(${test_name}_tsan PRIVATE -fsanitize=thread -g)
    target_link_options(${test_name}_tsan PRIVATE -fsanitize=thread)
    gtest_discover_tests(${test_name}_tsan)
endforeach()
