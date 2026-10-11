cmake_minimum_required(VERSION 3.20)

project(FarQoiViewer LANGUAGES CXX)

set(FAR_SOURCE_DIR "" CACHE PATH
    "Path to Far Manager source tree containing far/plugin.hpp")

if(NOT FAR_SOURCE_DIR)
    message(FATAL_ERROR
        "Set FAR_SOURCE_DIR, e.g. -DFAR_SOURCE_DIR=C:/far")
endif()

if(NOT EXISTS "${FAR_SOURCE_DIR}/far/plugin.hpp")
    message(FATAL_ERROR
        "Cannot find ${FAR_SOURCE_DIR}/far/plugin.hpp")
endif()

if(NOT MINGW)
    message(FATAL_ERROR
        "This project is configured for MinGW (MSYS2 UCRT64)")
endif()

find_package(Threads REQUIRED)

add_library(FarQoiViewer SHARED
    src/plugin.cpp
    src/qoi.cpp
    src/qoi.h
    src/FarQoiViewer.def
)

target_include_directories(FarQoiViewer PRIVATE
    "${FAR_SOURCE_DIR}/far"
    "${CMAKE_CURRENT_SOURCE_DIR}/src"
)

target_compile_features(FarQoiViewer PRIVATE cxx_std_17)

set_target_properties(FarQoiViewer PROPERTIES
    CXX_EXTENSIONS OFF
    OUTPUT_NAME "FarQoiViewer"
    PREFIX ""
    SUFFIX ".dll"
)

target_compile_definitions(FarQoiViewer PRIVATE
    UNICODE
    _UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

target_compile_options(FarQoiViewer PRIVATE
    -Wall
    -Wextra
    -Wpedantic
)

target_link_options(FarQoiViewer PRIVATE
    -static
    -static-libgcc
    -static-libstdc++
)

target_link_libraries(FarQoiViewer PRIVATE
    user32
    gdi32
    msimg32
    Threads::Threads
)