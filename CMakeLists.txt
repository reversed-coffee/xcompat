cmake_minimum_required(VERSION 4.0)

project(xcompat LANGUAGES C CXX)

if(NOT WIN32)
    message(FATAL_ERROR "The xcompat project currently only compiles for Windows.")
endif()

if(NOT CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "The xcompat project must compile for a 64-bit target.")
endif()

add_subdirectory(dependencies/hydra)

add_subdirectory(project/xcompat)
add_subdirectory(project/xcompat-cli)
add_subdirectory(project/xcompat-tgt)
