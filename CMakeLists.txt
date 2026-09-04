cmake_minimum_required(VERSION 3.16.0)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# The application sources live in src/. PlatformIO passes src/ to the build as
# an extra component itself, so only register it here when building natively
# with idf.py (where EXTRA_COMPONENT_DIRS is undefined). This keeps both
# build flows working.
if(NOT DEFINED EXTRA_COMPONENT_DIRS)
    list(APPEND EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/src")
endif()

project(DHCPServer)
