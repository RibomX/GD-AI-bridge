cmake_minimum_required(VERSION 3.25)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_OSX_ARCHITECTURES "arm64;x86_64")
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(GDAIBridge VERSION 0.1.0)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable to point to Geode")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# Small HTTP server library (header-only), runs the local API server
CPMAddPackage(
    NAME httplib
    GITHUB_REPOSITORY yhirose/cpp-httplib
    VERSION 0.18.3
    DOWNLOAD_ONLY YES
)
target_include_directories(${PROJECT_NAME} PRIVATE ${httplib_SOURCE_DIR})
# Geode's precompiled header pulls in windows.h (winsock 1), which clashes with httplib's winsock2
set_source_files_properties(src/Server.cpp PROPERTIES SKIP_PRECOMPILE_HEADERS ON)

setup_geode_mod(${PROJECT_NAME})
