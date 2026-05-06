cmake_minimum_required(VERSION 3.20)

project(totalreaper
    VERSION 0.1.2
    DESCRIPTION "REAPER ↔ RME TotalMix FX Global OSC Bridge"
    LANGUAGES CXX
)

# ──────────────────────────────────────────────────────────────────────────────
# Global build settings
# ──────────────────────────────────────────────────────────────────────────────
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# Warnings
if(MSVC)
    add_compile_options(/W4 /permissive-)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# ──────────────────────────────────────────────────────────────────────────────
# REAPER SDK
# ──────────────────────────────────────────────────────────────────────────────
set(REAPER_SDK_DIR "${CMAKE_SOURCE_DIR}/external/reaper-sdk")
set(WDL_DIR        "${CMAKE_SOURCE_DIR}/external/WDL")
if(NOT EXISTS "${REAPER_SDK_DIR}/sdk")
    message(FATAL_ERROR
        "REAPER SDK not found at ${REAPER_SDK_DIR}/sdk\n"
        "Run: git submodule update --init --recursive"
    )
endif()
if(NOT EXISTS "${WDL_DIR}/WDL")
    message(FATAL_ERROR
        "WDL not found at ${WDL_DIR}/WDL\n"
        "Run: git submodule update --init --recursive"
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Source files
# ──────────────────────────────────────────────────────────────────────────────
set(SOURCES
    src/main.cpp
    src/osc/OscMessage.cpp
    src/osc/OscClient.cpp
    src/osc/OscServer.cpp
    src/osc/TotalMixState.cpp
    src/reaper/ReaperAPI.cpp
    src/reaper/ChannelMap.cpp
    src/actions/DumpOscAction.cpp
    src/actions/TestSendAction.cpp
    src/actions/RoutingMirrorAction.cpp
    src/actions/PreampActions.cpp
    src/csurf/TotalReaperCSurf.cpp
)

set(HEADERS
    src/osc/OscMessage.h
    src/osc/OscClient.h
    src/osc/OscServer.h
    src/osc/TotalMixState.h
    src/reaper/ReaperAPI.h
    src/reaper/ChannelMap.h
    src/reaper/Console.h
    src/actions/Actions.h
    src/csurf/TotalReaperCSurf.h
)

# ──────────────────────────────────────────────────────────────────────────────
# Build target — REAPER plugin (shared library)
# ──────────────────────────────────────────────────────────────────────────────
add_library(reaper_totalreaper SHARED ${SOURCES} ${HEADERS})

target_include_directories(reaper_totalreaper PRIVATE src)
# Treat third-party headers as SYSTEM so their warnings stay out of our log.
# WDL has a nested layout (external/WDL/WDL/...) — including the inner dir
# lets reaper_plugin.h's "../WDL/swell/swell.h" resolve correctly.
target_include_directories(reaper_totalreaper SYSTEM PRIVATE
    "${REAPER_SDK_DIR}/sdk"
    "${WDL_DIR}/WDL"
)

# Platform-specific networking and naming
if(APPLE)
    # REAPER's plugin loader on macOS expects an arch suffix unless the dylib
    # is a universal binary. Match REAPER's own convention used by SWS, ImGui,
    # ReaPack etc. ("reaper_<name>-arm64.dylib" / "-x86_64.dylib").
    if(CMAKE_OSX_ARCHITECTURES)
        list(LENGTH CMAKE_OSX_ARCHITECTURES _osx_arch_count)
        if(_osx_arch_count GREATER 1)
            set(_arch_suffix "")  # universal binary
        else()
            set(_arch_suffix "-${CMAKE_OSX_ARCHITECTURES}")
        endif()
    else()
        set(_arch_suffix "-${CMAKE_HOST_SYSTEM_PROCESSOR}")
    endif()
    set_target_properties(reaper_totalreaper PROPERTIES
        PREFIX ""
        SUFFIX ".dylib"
        OUTPUT_NAME "reaper_totalreaper${_arch_suffix}"
    )
elseif(WIN32)
    set_target_properties(reaper_totalreaper PROPERTIES
        PREFIX ""
        SUFFIX ".dll"
        OUTPUT_NAME "reaper_totalreaper"
    )
    target_link_libraries(reaper_totalreaper PRIVATE ws2_32)
    # WIN32_LEAN_AND_MEAN tells windows.h (pulled in by the REAPER SDK) NOT
    # to include winsock.h, so our winsock2.h includes can be the first ones
    # — without this, winsock1/winsock2 headers redefine each other and the
    # build fails with hundreds of struct-redefinition errors.
    target_compile_definitions(reaper_totalreaper PRIVATE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
    )
elseif(UNIX)
    set_target_properties(reaper_totalreaper PROPERTIES
        PREFIX ""
        SUFFIX ".so"
        OUTPUT_NAME "reaper_totalreaper"
    )
endif()

# ──────────────────────────────────────────────────────────────────────────────
# Optional: install into REAPER UserPlugins folder for quick local iteration
# Usage: cmake --build build --target install_local
# ──────────────────────────────────────────────────────────────────────────────
if(APPLE)
    set(REAPER_USER_PLUGINS "$ENV{HOME}/Library/Application Support/REAPER/UserPlugins")
elseif(WIN32)
    set(REAPER_USER_PLUGINS "$ENV{APPDATA}/REAPER/UserPlugins")
elseif(UNIX)
    set(REAPER_USER_PLUGINS "$ENV{HOME}/.config/REAPER/UserPlugins")
endif()

add_custom_target(install_local
    COMMAND ${CMAKE_COMMAND} -E make_directory "${REAPER_USER_PLUGINS}"
    COMMAND ${CMAKE_COMMAND} -E copy
        $<TARGET_FILE:reaper_totalreaper>
        "${REAPER_USER_PLUGINS}"
    DEPENDS reaper_totalreaper
    COMMENT "Installing reaper_totalreaper to ${REAPER_USER_PLUGINS}"
)

message(STATUS "TotalReaper ${PROJECT_VERSION} configured")
message(STATUS "  Build type:   ${CMAKE_BUILD_TYPE}")
message(STATUS "  Compiler:     ${CMAKE_CXX_COMPILER_ID}")
message(STATUS "  Local deploy: ${REAPER_USER_PLUGINS}")
