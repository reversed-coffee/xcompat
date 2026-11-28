cmake_minimum_required(VERSION 4.0)

project(xcompat LANGUAGES C CXX)

include(CTest)

if(NOT WIN32)
    message(FATAL_ERROR "The xcompat project currently only compiles for Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The xcompat project must compile for a 64-bit target.")
endif()

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

set(XCOMPAT_RUNTIME_DIRECTORY "${CMAKE_BINARY_DIR}/runtime")

add_subdirectory(dependencies)
add_subdirectory(project)

if(BUILD_TESTING)
    add_subdirectory(tests)
endif()
