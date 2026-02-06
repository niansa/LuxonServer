# Copyright (c) 2026, the Luxon Server contributors
# SPDX-License-Identifier: BSD-3-Clause

cmake_minimum_required(VERSION 3.16)

project(LuxonServer LANGUAGES CXX)

set(CMAKE_POSITION_INDEPENDENT_CODE ON)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(LUXON_SERVER_ENABLE_WEBSERVER "Enable the built-in webserver" ON)
option(LUXON_SERVER_ENABLE_PLUGINS "Enable use of game plugins" OFF)
option(LUXON_SERVER_POLL "Poll blindly instead of select with timeout, less efficient" OFF)
option(LUXON_USE_EMBED_RESOURCE "Use EmbedResource instead of incbin.h" OFF)

# Apply single-threaded optimizations if strictly single-threaded
if (NOT LUXON_SERVER_ENABLE_PLUGINS AND NOT WIN32)
    add_compile_options($<$<COMPILE_LANG_AND_ID:CXX,GNU>:-fno-threadsafe-statics>)
    add_compile_definitions(thread_local=)
endif()

if (MSVC)
    # commoncpp gets compiled without ASAN and includes <vector>.
    # This causes issues during linking, so we have to turn it off.
    set(SANMAKE_ASAN OFF)

    # incbin.h is not supported on MSVC.
    set(LUXON_USE_EMBED_RESOURCE ON)
endif()

option(LUXON_SERVER_USE_SPD_LOG "Use spd_log for Luxon server, if found" ON)
if (LUXON_SERVER_USE_SPD_LOG)
    find_package(spdlog CONFIG)
endif()

if (CMAKE_SYSTEM_NAME STREQUAL Linux OR CMAKE_SYSTEM_NAME STREQUAL Windows)
    add_subdirectory(Luxon)

    add_subdirectory(sanmake)
else()
    set(LUXON_USE_TOMCRYPT ON CACHE BOOL "Force use of libtomcrypt" FORCE)

    add_subdirectory(Luxon)
    add_library(sanmake INTERFACE)
endif()

add_subdirectory(commoncpp)

add_executable(luxon_server
    include/luxon/server/apps.hpp  include/luxon/server/global.hpp    include/luxon/server/logger.hpp  src/apps.cpp src/handler_gameserver.cpp    src/lobby.cpp src/platform.cpp
    include/luxon/server/authentication.hpp  include/luxon/server/handler_base.hpp    include/luxon/server/metrics.hpp  src/authentication.cpp src/handler_masterserver.cpp  src/logger.cpp src/platform.hpp
    include/luxon/server/codes.hpp  include/luxon/server/handler_gameserver.hpp    include/luxon/server/peer.hpp  src/handler_nameserver.cpp    src/main.cpp src/server_manager.cpp
    include/luxon/server/handler_masterserver.hpp  include/luxon/server/peer_persistence.hpp  src/game.cpp    src/metrics.cpp src/sock_selector.cpp
    include/luxon/server/game.hpp  include/luxon/server/handler_nameserver.hpp    include/luxon/server/server_manager.hpp src/incbin.h      src/minicoro.h src/yaml.cpp
    include/luxon/server/sock_selector.hpp  include/luxon/server/json_fwd.hpp      src/peer.cpp src/yaml.hpp
    include/luxon/server/lobby.hpp    include/luxon/server/string_hash.hpp  src/handler_base.cpp src/json.hpp      src/peer_persistence.cpp
)
target_link_libraries(luxon_server
    sanmake commoncpp luxon_enet luxon_ser luxon_http_parser luxon_visualizer
)
target_compile_definitions(luxon_server PRIVATE
    SOURCE_DIR="${CMAKE_CURRENT_SOURCE_DIR}"
)
target_include_directories(luxon_server PRIVATE include/luxon/server PUBLIC include)

if (MSVC)
    target_compile_definitions(luxon_server PUBLIC NOMINMAX)
endif()

if (LUXON_USE_EMBED_RESOURCE)
    find_package(EmbedResource REQUIRED)
    add_resource_library(luxon_server_resources OBJECT RESOURCE_COLLECTION_NAME http_resources RESOURCES "index.html;stats.html;style.css")
    target_link_libraries(luxon_server luxon_server_resources)
    add_compile_definitions(LUXON_USE_EMBED_RESOURCE)
endif()

if (LUXON_SERVER_POLL)
    target_compile_definitions(luxon_server PRIVATE LUXON_SERVER_POLL)
endif()

if (LUXON_SERVER_ENABLE_WEBSERVER)
    target_sources(luxon_server PRIVATE include/luxon/server/http_server.hpp src/http_server.cpp)
    target_compile_definitions(luxon_server PRIVATE LUXON_SERVER_ENABLE_WEBSERVER)
endif()

if (LUXON_SERVER_ENABLE_PLUGINS)
    target_sources(luxon_server PRIVATE
        include/luxon/server/coroutine.hpp src/coroutine.cpp
        include/luxon/server/game_plugin_base.hpp src/game_plugin_base.cpp
        include/luxon/server/game_plugin_registry.hpp src/game_plugin_registry.cpp
    )
    target_compile_definitions(luxon_server PRIVATE LUXON_SERVER_ENABLE_PLUGINS)
endif()

if (spdlog_FOUND)
    target_link_libraries(luxon_server spdlog::spdlog)
    target_compile_definitions(luxon_server PUBLIC LUXON_SERVER_USE_SPDLOG)
endif()

target_compile_definitions(luxon_server PUBLIC PLATFORM="${CMAKE_SYSTEM_NAME}")
if (CMAKE_SYSTEM_NAME STREQUAL Linux)
    target_compile_definitions(luxon_server PUBLIC PLATFORM_LINUX)
elseif (CMAKE_SYSTEM_NAME STREQUAL Windows)
    target_compile_definitions(luxon_server PUBLIC PLATFORM_WINDOWS)
    target_link_libraries(luxon_server ws2_32)
elseif (CMAKE_SYSTEM_NAME STREQUAL Nintendo3DS)
    target_compile_definitions(luxon_server PUBLIC PLATFORM_3DS)
else()
    message(SEND_ERROR "${CMAKE_SYSTEM_NAME} is not a supported platform!")
endif()


# Handle user plugins
if (LUXON_SERVER_ENABLE_PLUGINS)
    add_library(luxon_server_api INTERFACE)
    target_link_libraries(luxon_server_api INTERFACE commoncpp)
    target_include_directories(luxon_server_api INTERFACE include)
    get_target_property(LUXON_SERVER_DEFS luxon_server COMPILE_DEFINITIONS)
    target_compile_definitions(luxon_server_api INTERFACE ${LUXON_SERVER_DEFS})

    set(LUXON_PLUGINS "" CACHE STRING "Semicolon-separated list of external Luxon server plugin directories to build and link")

    function(luxon_register_plugin plugin_target)
        message(STATUS "[Luxon] Registering plugin target: ${plugin_target}")
        set_property(GLOBAL APPEND PROPERTY LUXON_REGISTERED_PLUGINS "${plugin_target}")
        target_link_libraries(${plugin_target} PUBLIC luxon_server_api luxon_enet luxon_ser)
    endfunction()

    if (LUXON_SERVER_ENABLE_PLUGINS AND LUXON_PLUGINS)
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

    get_property(REGISTERED_PLUGINS GLOBAL PROPERTY LUXON_REGISTERED_PLUGINS)

    if (REGISTERED_PLUGINS)
        message(STATUS "[Luxon] Linking plugins to server: ${REGISTERED_PLUGINS}")
        if (NOT MSVC)
            target_link_libraries(luxon_server "-Wl,--whole-archive" ${REGISTERED_PLUGINS} "-Wl,--no-whole-archive")
        else()
            target_link_libraries(luxon_server ${REGISTERED_PLUGINS})
        endif()
    else()
        message(FATAL_ERROR "[Luxon] No plugins were registered, but LUXON_SERVER_ENABLE_PLUGINS is ON")
    endif()
endif()
