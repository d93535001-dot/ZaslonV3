#
# CMakeLists.txt
# ZASLON User-Mode Utility
# Requires: CMake 3.25+
#
cmake_minimum_required(VERSION 3.25)
project(Zaslon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# --------------------------------------------------------------------------
# Output Directories
# --------------------------------------------------------------------------
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin/${CMAKE_BUILD_TYPE}")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_CURRENT_SOURCE_DIR}/bin/Debug")
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_CURRENT_SOURCE_DIR}/bin/Release")

# --------------------------------------------------------------------------
# Dependencies Management via FetchContent
# --------------------------------------------------------------------------
include(FetchContent)

# 1. ImGui (docking branch)
FetchContent_Declare(
    imgui
    GIT_REPOSITORY https://github.com/ocornut/imgui.git
    GIT_TAG docking
)
FetchContent_MakeAvailable(imgui)

# 2. stb_image
FetchContent_Declare(
    stb
    GIT_REPOSITORY https://github.com/nothings/stb.git
    GIT_TAG master
)
FetchContent_MakeAvailable(stb)

# --------------------------------------------------------------------------
# Main sources
# --------------------------------------------------------------------------
set(APP_SOURCES
    main.cpp
    dashboard.cpp
    process_panel.cpp
    integrity_module.cpp
    offline_registry.cpp
    autorun_scanner.cpp
    file_explorer.cpp
    system_repair.cpp
    restrictions_manager.cpp
    users_manager.cpp
    help_tab.cpp
    gui_theme.cpp
    theme_manager.cpp
    zaslon_core.cpp
    offline_os_manager.cpp
    installer_tracker.cpp
    svg_icons.cpp
    zaslon.rc
)

add_executable(zaslon WIN32 ${APP_SOURCES})

# --------------------------------------------------------------------------
# Includes
# --------------------------------------------------------------------------
# Since ImGui doesn't export a target in its pure CMake-less form, we add its sources manually to our target
target_sources(zaslon PRIVATE
    ${imgui_SOURCE_DIR}/imgui.cpp
    ${imgui_SOURCE_DIR}/imgui_demo.cpp
    ${imgui_SOURCE_DIR}/imgui_draw.cpp
    ${imgui_SOURCE_DIR}/imgui_tables.cpp
    ${imgui_SOURCE_DIR}/imgui_widgets.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_win32.cpp
    ${imgui_SOURCE_DIR}/backends/imgui_impl_dx9.cpp
)

target_include_directories(zaslon PRIVATE
    "${CMAKE_CURRENT_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}"
    "${imgui_SOURCE_DIR}/backends"
    "${stb_SOURCE_DIR}"
)

# --------------------------------------------------------------------------
# Compile Definitions & Options
# --------------------------------------------------------------------------
target_compile_definitions(zaslon PRIVATE
    _UNICODE
    UNICODE
    WIN32_LEAN_AND_MEAN
    NOMINMAX
)

if(MSVC)
    target_compile_options(zaslon PRIVATE
        /utf-8
        /W4
        /WX- # Treat warnings as warnings, not errors for now, but high warning level
    )

    # Force static linking against the C/C++ runtime library (WinPE compatibility)
    set_property(TARGET zaslon PROPERTY MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

    target_link_options(zaslon PRIVATE
        /DELAYLOAD:d3d9.dll
        /DELAYLOAD:Rstrtmgr.dll
        /DELAYLOAD:iphlpapi.dll
        /DELAYLOAD:netapi32.dll
    )
endif()

# --------------------------------------------------------------------------
# Link Libraries
# --------------------------------------------------------------------------
target_link_libraries(zaslon PRIVATE
    d3d9
    advapi32
    kernel32
    user32
    gdi32
    shell32
    ole32
    psapi
    crypt32
    wintrust
    Rstrtmgr
    netapi32
    version
    iphlpapi
    delayimp
)
