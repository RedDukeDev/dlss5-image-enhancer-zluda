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

find_package(Qt6 REQUIRED COMPONENTS Widgets)

# The DLSS half is the addon's, used unchanged. It is the layer that has been
# made to work; a second implementation would only be a second thing to get
# wrong.
set(ADDON ${CMAKE_CURRENT_SOURCE_DIR}/../addon)

add_executable(dlss5-image-enhancer WIN32
    gui/main.cpp
    gui/main_window.cpp
    gui/main_window.h
    gui/half_float.h
    core/image_processor.cpp
    core/image_processor.h
    core/precompile.cpp
    core/precompile.h
    ${ADDON}/dlss_cuda.cpp
    ${ADDON}/frame_blit.cpp
)

target_include_directories(dlss5-image-enhancer PRIVATE ${ADDON})
target_link_libraries(dlss5-image-enhancer PRIVATE Qt6::Widgets d3d12 dxgi d3dcompiler)

# A console alongside the window: the DLSS layer and the network both report
# through standard error, and those messages are the only explanation when
# something is refused.
target_compile_definitions(dlss5-image-enhancer PRIVATE UNICODE _UNICODE NOMINMAX)

# Our own NGX runtime -- nvngx.dll, the stand-in that lets the snippet believe
# it is being driven by NVIDIA's own NGX loader. See the comment at the top of
# ngx_runtime/ngx_runtime.cpp for what it does and why it exists.
#
# The source under ngx_runtime/ is a deliberate copy of addon/ngx_runtime.cpp,
# not a reference to it: this project and the ReShade addon are kept as two
# separate projects, each with its own complete source and its own build, and
# neither CMakeLists reaches into the other's tree. ${ADDON} above is the one
# exception, for the DLSS layer itself, which predates this and is a choice
# already made -- this target does not extend that pattern to the runtime.
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
