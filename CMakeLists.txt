# DLSS 5 Image Enhancer.
#
#   cmake -B build -DCMAKE_PREFIX_PATH=<Qt>/msvc2022_64
#   cmake --build build --config Release

cmake_minimum_required(VERSION 3.19)
project(dlss5-image-enhancer LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR
        "This program can only be built for Windows: the DLSS network is a Windows ")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)

# Sql is here for one job: precompile.cpp gives every translation a cache
# database of its own and merges them when they are done, because several
# processes writing one of them can lose rows and say nothing. Merging SQLite
# properly means talking to SQLite, and Qt already carries it -- windeployqt
# brings sqldrivers\qsqlite.dll along the way it already brings the platform and
# image plugins, so nothing in build.bat has to change.
find_package(Qt6 REQUIRED COMPONENTS Widgets Sql)

add_executable(dlss5-image-enhancer WIN32
    gui/main.cpp
    gui/main_window.cpp
    gui/main_window.h
    gui/half_float.h
    core/image_processor.cpp
    core/image_processor.h
    core/precompile.cpp
    core/precompile.h
    dlss_layer/dlss_cuda.cpp
    dlss_layer/frame_blit.cpp
)

target_include_directories(dlss5-image-enhancer PRIVATE dlss_layer)
target_link_libraries(dlss5-image-enhancer PRIVATE Qt6::Widgets Qt6::Sql d3d12 dxgi d3dcompiler)

# A console alongside the window: the DLSS layer and the network both report
# through standard error, and those messages are the only explanation when
# something is refused.
target_compile_definitions(dlss5-image-enhancer PRIVATE UNICODE _UNICODE NOMINMAX)

# Our own NGX runtime -- nvngx.dll, the stand-in that lets the snippet believe
# it is being driven by NVIDIA's own NGX loader. See the comment at the top of
# ngx_runtime/ngx_runtime.cpp for what it does and why it exists.
add_library(nvngx SHARED
    ngx_runtime/ngx_runtime.cpp
    ngx_runtime/ngx_cuda.h
)
target_compile_definitions(nvngx PRIVATE UNICODE _UNICODE NOMINMAX)
set_target_properties(nvngx PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY $<TARGET_FILE_DIR:dlss5-image-enhancer>
    # No Qt in this DLL at all, so nothing for AUTOMOC to look for -- turned
    # off here rather than left to inherit the directory-wide setting, which
    # would otherwise generate an empty nvngx_autogen/ for it to find nothing
    # in.
    AUTOMOC OFF
)
