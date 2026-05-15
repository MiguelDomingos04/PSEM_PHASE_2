cmake_minimum_required(VERSION 3.16)

message(STATUS "ROOT CMake: TARGET_SUBDIR is '${TARGET_SUBDIR}'")

if(NOT DEFINED TARGET_SUBDIR OR "${TARGET_SUBDIR}" STREQUAL "")
    message(FATAL "Target subdirectory not specified! Aborting...")
endif()

set(TARGET_SUBDIR "${TARGET_SUBDIR}" CACHE STRING "Target Subdirectory" FORCE)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(acquisition-framework)