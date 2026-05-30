cmake_minimum_required(VERSION 3.16)
# cspot uses cmake_minimum_required(VERSION 2.8.12); CMake 4+ rejects <3.5 without this
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(spotify-esp32)
