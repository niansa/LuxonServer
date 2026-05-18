# Copyright (c) 2026, the Luxon Server contributors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

project(LuxonServer LANGUAGES C CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# WASI-SDK support code
if (WASI)
    set(CMAKE_EXE_LINKER_FLAGS "${CMAKE_EXE_LINKER_FLAGS} -Wl,--gc-sections")
    set(CMAKE_SHARED_LINKER_FLAGS "${CMAKE_SHARED_LINKER_FLAGS} -Wl,--gc-sections")

    add_compile_options(-fwasm-exceptions "SHELL:-mllvm -wasm-use-legacy-eh=false")
    add_compile_definitions(-D_WASI_EMULATED_PROCESS_CLOCKS -D_WASI_EMULATED_SIGNAL)
    add_link_options("SHELL:-mllvm -wasm-use-legacy-eh=false" -Wl,--export=malloc -Wl,--export=free)
    link_libraries(-fwasm-exceptions -lunwind -lsetjmp -lwasi-emulated-process-clocks -lwasi-emulated-signal)
endif()

include(cmake/CPM.cmake)

include(CMakeDependentOption)

# Core server options
option(LUXON_SERVER_ENABLE_WEBSERVER "Enable the built-in webserver" ON)
option(LUXON_SERVER_POLL "Poll blindly instead of select with timeout, less efficient" OFF)
option(LUXON_USE_EMBED_RESOURCE "Use EmbedResource instead of incbin.h" OFF)

# Profiling options
option(LUXON_SERVER_TRACY "Build Luxon Server with tracy enabled" OFF)
cmake_dependent_option(LUXON_SERVER_TRACY_ON_DEMAND
    "Only collect tracy data with profiler server connected" ON
    "LUXON_SERVER_TRACY" OFF)

# FFI & Plugin options
option(LUXON_SERVER_BUILD_FFI "Builds shared library with C FFI" OFF)

cmake_dependent_option(LUXON_SERVER_EXPOSE_FULL_FFI
    "Build with full FFI interface exposed" OFF
    "LUXON_SERVER_BUILD_FFI" OFF)

option(LUXON_SERVER_HOOKPOINTS "Allows library user or plugins to hook into specific parts of Luxon Server" OFF)
option(LUXON_SERVER_ENABLE_PLUGINS "Enable use of plugins" OFF)

if (LUXON_SERVER_EXPOSE_FULL_FFI)
    set(LUXON_SERVER_HOOKPOINTS ON CACHE BOOL "Allows library user or plugins to hook into specific parts of Luxon Server" FORCE)
    set(LUXON_SERVER_ENABLE_PLUGINS ON CACHE BOOL "Enable use of plugins" FORCE)
endif()

cmake_dependent_option(LUXON_SERVER_ENABLE_COROUTINES
    "Enable execution of handler code in coroutines" OFF
    "LUXON_SERVER_ENABLE_PLUGINS;NOT LUXON_SERVER_BUILD_FFI;NOT WASI" OFF)

# Basic defines
set(WEB_ROOT_PATH "${CMAKE_CURRENT_SOURCE_DIR}/web")

# Apply single-threaded optimizations if strictly single-threaded
if (NOT LUXON_SERVER_ENABLE_PLUGINS AND NOT WIN32 AND NOT LUXON_SERVER_TRACY)
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fno-threadsafe-statics>)
    add_compile_definitions(thread_local=)
endif()

# MSVC and WASI -specific configurations
if (MSVC OR WASI)
    set(SANMAKE_ASAN OFF)
    set(LUXON_USE_EMBED_RESOURCE ON)
endif()

# Linux/Windows/FreeBSD get to use sanmake, mbedtls, spdlog and setting storage database support
if (CMAKE_SYSTEM_NAME MATCHES "^(Linux|Windows|FreeBSD|OpenBSD|Darwin)$")
    add_subdirectory(Luxon)

    option(LUXON_SERVER_USE_SPDLOG "Use spdlog for Luxon server" ON)
    option(LUXON_SERVER_USE_SANMAKE "Use basic sanitizers for Luxon server" ON)
    option(LUXON_SERVER_ENABLE_SETTINGS_DATABASE "Build Luxon Server with settings storage database support enabled" ON)
else()
    # Compatible configuration
    set(LUXON_USE_TOMCRYPT ON CACHE BOOL "Force use of libtomcrypt" FORCE)

    add_subdirectory(Luxon)

    set(LUXON_SERVER_USE_SPDLOG OFF)
    set(LUXON_SERVER_USE_SANMAKE OFF)
    set(LUXON_SERVER_ENABLE_SETTINGS_DATABASE OFF)
endif()

# Include sanmake
if (LUXON_SERVER_USE_SANMAKE)
    CPMAddPackage("gl:niansa/sanmake@1.0")
else()
    add_library(sanmake INTERFACE)
endif()

# Include spdlog
if (LUXON_SERVER_USE_SPDLOG)
    if (NOT TARGET spdlog)
        find_package(spdlog CONFIG)
        if (NOT spdlog_FOUND)
            set(SPDLOG_USE_STD_FORMAT ON CACHE BOOL "Force use of std::format for spdlog" FORCE)
            CPMAddPackage("gh:gabime/spdlog@1.17.0")
        endif()
    endif()
endif()

# Include commoncpp
if (NOT TARGET commoncpp)
    CPMAddPackage("gl:niansa/commoncpp@1.1")
endif()

# Include magic enum
if (NOT TARGET magic_enum)
    CPMAddPackage("gh:Neargye/magic_enum@0.9.7")
endif()

# Add main library+executable target
add_library(luxon_server_lib
    include/luxon/server/apps.hpp  include/luxon/server/global.hpp    include/luxon/server/logger.hpp  src/apps.cpp src/handler_gameserver.cpp    src/lobby.cpp src/platform.cpp
    include/luxon/server/authentication.hpp  include/luxon/server/handler_base.hpp    include/luxon/server/metrics.hpp  src/authentication.cpp src/handler_masterserver.cpp  src/logger.cpp src/platform.hpp
    include/luxon/server/handler_gameserver.hpp    include/luxon/server/peer.hpp  src/handler_nameserver.cpp  src/server_manager.cpp
    include/luxon/server/handler_masterserver.hpp  include/luxon/server/peer_persistence.hpp  src/game.cpp    src/metrics.cpp src/sock_selector.cpp
    include/luxon/server/game.hpp  include/luxon/server/handler_nameserver.hpp    include/luxon/server/server_manager.hpp src/incbin.h       src/yaml.cpp
    include/luxon/server/sock_selector.hpp  include/luxon/server/json_fwd.hpp      src/peer.cpp src/yaml.hpp
    include/luxon/server/lobby.hpp    include/luxon/server/string_hash.hpp  src/handler_base.cpp src/json.hpp      src/peer_persistence.cpp
    include/luxon/server/hookpoints.hpp
    include/luxon/server/data_model.hpp
    include/luxon/server/sqlite3.h src/sqlite3.c
    src/lobby_vt.hpp src/lobby_vt.cpp

    tracy/public/tracy/Tracy.hpp
    src/tracy_mem.cpp
)
target_link_libraries(luxon_server_lib
    PRIVATE magic_enum luxon_http_parser
    PUBLIC sanmake commoncpp luxon_enet luxon_ser
)
target_include_directories(luxon_server_lib PRIVATE include/luxon/server tracy/public PUBLIC include)
target_compile_definitions(luxon_server_lib PRIVATE SQLITE_CUSTOM_INCLUDE=sqlite3-config.h)

target_link_libraries(luxon_server_lib PRIVATE
    $<$<CONFIG:Debug>:luxon_visualizer>
)

if (WASI)
    target_link_libraries(luxon_server_lib PUBLIC wasm_sockets)
endif()

add_executable(luxon_server src/main.cpp)
target_link_libraries(luxon_server luxon_server_lib)

if (LUXON_SERVER_BUILD_FFI)
    if (NOT WASI)
        add_library(luxon_server_ffi SHARED)
    else()
        add_executable(luxon_server_ffi)
    endif()
    target_sources(luxon_server_ffi PRIVATE src/ffi.cpp)
    target_link_libraries(luxon_server_ffi luxon_server_lib)
    target_include_directories(luxon_server_ffi PUBLIC ffi_include)
    set_target_properties(luxon_server_ffi PROPERTIES
        C_VISIBILITY_PRESET hidden
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN YES
    )
endif()

# Windows specific fixes
if (MSVC)
    target_compile_definitions(luxon_server_lib PUBLIC NOMINMAX)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL Windows)
    target_sources(luxon_server_lib PRIVATE include/luxon/server/wepoll.h src/wepoll.c)
endif()

if (LUXON_USE_EMBED_RESOURCE)
    find_package(EmbedResource)
    if (NOT EmbedResource_FOUND)
        CPMAddPackage("gh:ankurvdev/embedresource@0.0.12")
    endif()
    add_resource_library(luxon_server_resources OBJECT RESOURCE_COLLECTION_NAME http_resources RESOURCES "${WEB_ROOT_PATH}/index.html;${WEB_ROOT_PATH}/stats.html;${WEB_ROOT_PATH}/style.css")
    target_link_libraries(luxon_server_lib PRIVATE luxon_server_resources)
    add_compile_definitions(LUXON_USE_EMBED_RESOURCE)
endif()

# Configuration
if (LUXON_SERVER_POLL)
    target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_POLL)
endif()

if (LUXON_SERVER_ENABLE_WEBSERVER)
    target_sources(luxon_server_lib PRIVATE include/luxon/server/http_server.hpp src/http_server.cpp)
    target_compile_definitions(luxon_server_lib PUBLIC
        LUXON_SERVER_ENABLE_WEBSERVER
        LUXON_SERVER_WEB_ROOT="${WEB_ROOT_PATH}"
    )
endif()

if (LUXON_SERVER_HOOKPOINTS)
    target_compile_definitions(luxon_server_lib PUBLIC
        LUXON_SERVER_ENABLE_HOOKPOINTS
    )
endif()

if (LUXON_SERVER_ENABLE_PLUGINS)
    target_sources(luxon_server_lib PRIVATE
        include/luxon/server/game_plugin_base.hpp src/game_plugin_base.cpp
        include/luxon/server/game_plugin_registry.hpp src/game_plugin_registry.cpp
        include/luxon/server/auth_plugin_registry.hpp src/auth_plugin_registry.cpp
    )
    target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_ENABLE_PLUGINS)

    if (NOT WASI)
        target_sources(luxon_server_lib PRIVATE
            include/luxon/server/sidethread.hpp
        )
        target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_MULTITHREADED)
    endif()
endif()

if (LUXON_SERVER_ENABLE_COROUTINES)
    CPMAddPackage("gl:niansa/minicoropp@1.0")
    target_link_libraries(luxon_server_lib PUBLIC minicoropp)
    target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_ENABLE_COROUTINES)
endif()

if (LUXON_SERVER_USE_SPDLOG)
    target_link_libraries(luxon_server_lib PUBLIC spdlog::spdlog)
    target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_USE_SPDLOG)
endif()

if (LUXON_SERVER_ENABLE_SETTINGS_DATABASE)
    target_sources(luxon_server_lib PRIVATE
        include/luxon/server/sqlite3pp.hpp src/sqlite3pp.cpp
        include/luxon/server/settings_manager.hpp src/settings_manager.cpp
    )
    target_compile_definitions(luxon_server_lib PUBLIC LUXON_SERVER_ENABLE_SETTINGS_DATABASE)
endif()

if (LUXON_SERVER_TRACY)
    target_sources(luxon_server_lib PRIVATE tracy/public/TracyClient.cpp)
    target_compile_definitions(luxon_server_lib PRIVATE TRACY_ENABLE)
    if (LUXON_SERVER_TRACY_ON_DEMAND)
        target_compile_definitions(luxon_server_lib PRIVATE TRACY_ON_DEMAND)
    endif()
endif()

# Platform dependent configuration
target_compile_definitions(luxon_server_lib PRIVATE PLATFORM="${CMAKE_SYSTEM_NAME}")
if (CMAKE_SYSTEM_NAME STREQUAL Linux)
    target_compile_definitions(luxon_server_lib PRIVATE PLATFORM_LINUX)
elseif (CMAKE_SYSTEM_NAME STREQUAL Windows)
    target_compile_definitions(luxon_server_lib PRIVATE PLATFORM_WINDOWS)
    target_link_libraries(luxon_server_lib PRIVATE ws2_32)
elseif (CMAKE_SYSTEM_NAME STREQUAL Nintendo3DS)
    target_compile_definitions(luxon_server_lib PRIVATE PLATFORM_3DS)
elseif (CMAKE_SYSTEM_NAME STREQUAL FreeBSD)
    target_compile_definitions(luxon_server_lib PRIVATE PLATFORM_FREEBSD)
elseif (CMAKE_SYSTEM_NAME STREQUAL Darwin)
    target_compile_definitions(luxon_server_lib PRIVATE PLATFORM_MACOS)
endif()


# Handle user plugins
if (LUXON_SERVER_ENABLE_PLUGINS)
    # Create target for user plugins to link with
    add_library(luxon_server_api INTERFACE)
    target_link_libraries(luxon_server_api INTERFACE luxon_server_lib luxon_enet luxon_ser commoncpp)
    target_include_directories(luxon_server_api INTERFACE include)
    get_target_property(LUXON_SERVER_DEFS luxon_server_lib COMPILE_DEFINITIONS)
    target_compile_definitions(luxon_server_api INTERFACE ${LUXON_SERVER_DEFS})

    # List of plugin projects in cache for user to populate
    set(LUXON_PLUGINS "" CACHE STRING "Semicolon-separated list of external Luxon server plugin directories to build and link")

    # Function for user to call to register they STATIC library target as a plugin
    function(luxon_register_plugin plugin_target)
        message(STATUS "[Luxon] Registering plugin target: ${plugin_target}")
        set_property(GLOBAL APPEND PROPERTY LUXON_REGISTERED_PLUGINS "${plugin_target}")
        target_link_libraries(${plugin_target} PUBLIC luxon_server_api)
    endfunction()

    # Load user specified list of plugin projects
    if (LUXON_PLUGINS)
        message(STATUS "[Luxon] Loading external plugins...")

        foreach(PLUGIN_PATH ${LUXON_PLUGINS})
            # Calculate a unique binary directory for the plugin to avoid collisions
            get_filename_component(PLUGIN_NAME "${PLUGIN_PATH}" NAME)
            set(PLUGIN_BIN_DIR "${CMAKE_BINARY_DIR}/plugins/${PLUGIN_NAME}")

            if(EXISTS "${PLUGIN_PATH}/CMakeLists.txt")
                add_subdirectory("${PLUGIN_PATH}" "${PLUGIN_BIN_DIR}")
            else()
                message(WARNING "[Luxon] Plugin directory not found or invalid: ${PLUGIN_PATH}")
            endif()
        endforeach()
    endif()

    # Get and link all user plugins
    get_property(REGISTERED_PLUGINS GLOBAL PROPERTY LUXON_REGISTERED_PLUGINS)
    if (REGISTERED_PLUGINS)
        message(STATUS "[Luxon] Linking plugins to server: ${REGISTERED_PLUGINS}")
        target_link_libraries(luxon_server "$<LINK_LIBRARY:WHOLE_ARCHIVE,${REGISTERED_PLUGINS}>")
    else()
        message(WARNING "[Luxon] No plugins were registered, but LUXON_SERVER_ENABLE_PLUGINS is ON")
    endif()
endif()
