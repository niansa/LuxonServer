// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

/*
 * MACRO CONFIGURATION
 * ---------------------------
 * This file relies on the following preprocessor macros to control build variants,
 * memory management strategies, concurrency features, and I/O models:
 *
 * LUXON_SERVER_MULTITHREADED
 * Toggles multithreading capabilities and thread-safe synchronization.
 * - Enables the `main_loop_calls_` queue and protects it via `main_loop_calls_mutex_`
 *   to safely dispatch tasks from arbitrary threads back onto the main loop thread.
 *
 * LUXON_SERVER_ENABLE_SETTINGS_DATABASE
 * Toggles embedded settings database management.
 * - Parses the "SettingsDatabase" path string from the root YAML configuration map.
 * - Conditionally initializes the `settings_manager` object lifecycle within the server manager.
 *
 * LUXON_SERVER_ENABLE_HOOKPOINTS
 * Toggles server event interception extensions.
 * - Instantiates the `hookpoints` structure inside the ServerManager to allow external
 *   modules or plugins to register lifecycle and packet hooks.
 *
 * LUXON_ENET_ENABLE_METRICS
 * Toggles granular network telemetry and telemetry tracking.
 * - Instantiates internal `enet::Metrics` tracking objects and interval timers.
 * - Flushes and ticks tracking data periodically (~1000ms intervals) during slow server updates.
 * - Exposes metrics publicly via `get_enet_metrics()`.
 *
 * NDEBUG
 * Standard C++ macro for Release builds.
 * - If UNDEFINED (Debug build):
 *   1. Sets logger verbosity automatically to `trace` level.
 *   2. Enables deep payload inspection (`visualizer::print_ser_message` / `print_http_message`)
 *      and raw hex dumps for unrecognized ENet payloads to assist protocol debugging.
 *
 * LUXON_SERVER_ENABLE_WEBSERVER
 * Toggles the embedded HTTP server component.
 * - Parses the "HTTP" section of the YAML configuration map.
 * - Instantiates the `http_server_` instance and handles automated lifecycle updates.
 * - Collects performance diagnostics (`idle_time`, `busy_time` metrics) per loop tick.
 * - Hooks HTTP socket file descriptors directly into the main read selector (if not polling).
 */

#include "server_manager.hpp"
#include "global.hpp"
#include "platform.hpp"
#include "peer.hpp"
#include "logger.hpp"
#include "handler_nameserver.hpp"
#include "handler_masterserver.hpp"
#include "handler_gameserver.hpp"
#include "coro_support.hpp"
#include "yaml.hpp"
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
#include "ipc_codes.hpp"
#include "pfr_codec.hpp"
#endif

#include <iostream>
#include <string_view>
#include <fstream>
#include <sstream>
#include <format>
#include <random>
#include <exception>
#include <stdexcept>
#include <algorithm>
#include <luxon/common_codes.hpp>
#include <luxon/ser_gp_binary_v18.hpp>
#include <luxon/ser_encryption.hpp>
#include <luxon/visualizer.hpp>
#include <tracy/Tracy.hpp>

namespace server {
std::function<void(const std::string&)> ServerManager::handle_start_subprocess{};

namespace {
ServerType StringToServerType(const std::string& str) {
    if (str == "NameServer")
        return ServerType::NameServer;
    if (str == "MasterServer")
        return ServerType::MasterServer;
    if (str == "GameServer")
        return ServerType::GameServer;

    throw std::runtime_error("Unknown ServerType: " + str);
}

ServerProtocol StringToEndpointProtocol(const std::string& str) {
    if (str == "UDP")
        return ServerProtocol::UDP;
    if (str == "TCP")
        return ServerProtocol::TCP;
    if (str == "WebSocket")
        return ServerProtocol::WebSocket;

    throw std::runtime_error("Unknown protocol: " + str);
}

std::string LoadFile(const std::string& filename) {
    std::ifstream f(filename, std::ios::binary);
    if (!f)
        throw std::runtime_error("Failed to open config file: " + filename);
    std::stringstream buffer;
    buffer << f.rdbuf();
    return buffer.str();
}

std::string_view ServerTypeToString(ServerType type) {
    switch (type) {
    case ServerType::NameServer:
        return "NameServer";
    case ServerType::MasterServer:
        return "MasterServer";
    case ServerType::GameServer:
        return "GameServer";
    default:
        return "Unknown???";
    }
}

HandlerPtr<HandlerBase> ServerTypeToHandler(ServerType type, ServerManager& server_man, std::shared_ptr<Peer>&& peer) {
    switch (type) {
    case ServerType::NameServer:
        return HandlerPtr<NameServerHandler>(new NameServerHandler(server_man, peer));
    case ServerType::MasterServer:
        return HandlerPtr<MasterServerHandler>(new MasterServerHandler(server_man, peer));
    case ServerType::GameServer:
        return HandlerPtr<GameServerHandler>(new GameServerHandler(server_man, peer));
    default:
        return nullptr;
    }
}

uint8_t NormalizeMaxGamePeers(unsigned value) {
    uint8_t normalized = static_cast<uint8_t>(std::min<unsigned>(value, 255));
    if (normalized == 255)
        normalized = 0;
    return normalized;
}

[[noreturn]]
void ThrowConfigError(std::string_view path, std::string_view message) {
    throw std::runtime_error(std::format("Config error at {}: {}", path, message));
}

void ExpectMap(Yaml::Node& node, std::string_view path) {
    if (!node.IsMap())
        ThrowConfigError(path, "expected a map");
}

void ExpectSequence(Yaml::Node& node, std::string_view path) {
    if (!node.IsSequence())
        ThrowConfigError(path, "expected a sequence");
}

void ValidateAllowedKeys(Yaml::Node& map, std::string_view path, std::initializer_list<std::string_view> allowed) {
    ExpectMap(map, path);

    for (auto it = map.Begin(); it != map.End(); it++) {
        std::string_view key = (*it).first;
        if (std::find(allowed.begin(), allowed.end(), key) == allowed.end())
            ThrowConfigError(path, std::format("unknown key '{}'", key));
    }
}

template <typename T> T ReadNodeScalar(Yaml::Node& node, std::string_view path) {
    try {
        return node.As<T>();
    } catch (const std::exception& e) {
        ThrowConfigError(path, e.what());
    }
}

template <typename T> std::optional<T> ReadOptionalScalar(Yaml::Node& map, const char *key, std::string_view path) {
    Yaml::Node& child = map[key];
    if (child.IsNone())
        return std::nullopt;

    return ReadNodeScalar<T>(child, std::format("{}.{}", path, key));
}

template <typename T> T ReadRequiredScalar(Yaml::Node& map, const char *key, std::string_view path) {
    auto value = ReadOptionalScalar<T>(map, key, path);
    if (!value)
        ThrowConfigError(path, std::format("missing required key '{}'", key));
    return std::move(*value);
}

ServerType ReadRequiredServerType(Yaml::Node& map, const char *key, std::string_view path) {
    const auto value = ReadRequiredScalar<std::string>(map, key, path);
    try {
        return StringToServerType(value);
    } catch (const std::exception& e) {
        ThrowConfigError(std::format("{}.{}", path, key), e.what());
    }
}

ServerProtocol ReadOptionalProtocol(Yaml::Node& map, const char *key, std::string_view path, ServerProtocol fallback = ServerProtocol::UDP) {
    auto value = ReadOptionalScalar<std::string>(map, key, path);
    if (!value)
        return fallback;

    try {
        return StringToEndpointProtocol(*value);
    } catch (const std::exception& e) {
        ThrowConfigError(std::format("{}.{}", path, key), e.what());
    }
}

std::optional<std::string> ReadOptionalExternalAddress(Yaml::Node& map, std::string_view path) {
    // `address` is accepted as a legacy alias for `external_address`
    auto external_address = ReadOptionalScalar<std::string>(map, "external_address", path);
    auto legacy_address = ReadOptionalScalar<std::string>(map, "address", path);

    if (external_address && legacy_address)
        ThrowConfigError(path, "use only one of 'external_address' or 'address'");

    if (external_address)
        return std::move(*external_address);
    if (legacy_address) {
        create_logger("ConfigParser")->warn("The 'address' key at {} is deprecated. Please migrate to using 'external_address'.", path);
        return std::move(*legacy_address);
    }
    return std::nullopt;
}

void ParseStunServer(ServerConfig& server, Yaml::Node& stun_node, const std::string& path) {
    if (stun_node.IsScalar()) {
        server.stun_server_host = ReadNodeScalar<std::string>(stun_node, path);
        server.stun_server_port = 19302;
        return;
    }

    if (!stun_node.IsMap())
        ThrowConfigError(path, "expected a string or a map");

    ValidateAllowedKeys(stun_node, path, {"host", "port"});

    server.stun_server_host = ReadRequiredScalar<std::string>(stun_node, "host", path);
    if (auto port = ReadOptionalScalar<uint16_t>(stun_node, "port", path))
        server.stun_server_port = *port;
}

void ParseServersSection(ServerManagerConfig& config, Yaml::Node& section) {
    ExpectMap(section, "Servers");

    for (auto it = section.Begin(); it != section.End(); it++) {
        const std::string type_name = (*it).first;
        Yaml::Node& list = (*it).second;
        const std::string type_path = std::format("Servers.{}", type_name);

        ServerType type;
        try {
            type = StringToServerType(type_name);
        } catch (const std::exception& e) {
            ThrowConfigError(type_path, e.what());
        }

        ExpectSequence(list, type_path);

        size_t index = 0;
        for (auto itemIt = list.Begin(); itemIt != list.End(); itemIt++, ++index) {
            Yaml::Node& item = (*itemIt).second;
            const std::string item_path = std::format("{}[{}]", type_path, index);

            ValidateAllowedKeys(item, item_path, {"port", "external_address", "address", "allow_unsolicited", "subprocess", "stun_server", "proxies"});

            ServerConfig server;
            server.type = type;
            server.port = ReadRequiredScalar<uint16_t>(item, "port", item_path);

            if (auto allow_unsolicited = ReadOptionalScalar<bool>(item, "allow_unsolicited", item_path))
                server.allow_unsolicited = *allow_unsolicited;

            if (auto subprocess = ReadOptionalScalar<bool>(item, "subprocess", item_path))
                server.subprocess = *subprocess;

            if (Yaml::Node& stun_node = item["stun_server"]; !stun_node.IsNone())
                ParseStunServer(server, stun_node, item_path + ".stun_server");

            auto external_address = ReadOptionalExternalAddress(item, item_path);
            if (external_address)
                server.external_address = std::move(*external_address);

            if (!item["proxies"].IsNone()) {
                ExpectSequence(item["proxies"], item_path + ".proxies");
                size_t p_index = 0;
                for (auto pIt = item["proxies"].Begin(); pIt != item["proxies"].End(); pIt++, ++p_index) {
                    Yaml::Node& pNode = (*pIt).second;
                    std::string p_path = std::format("{}.proxies[{}]", item_path, p_index);

                    ValidateAllowedKeys(pNode, p_path, {"protocol", "address"});

                    ProxyConfig proxy;
                    proxy.protocol = ReadOptionalProtocol(pNode, "protocol", p_path, ServerProtocol::TCP);
                    proxy.address = ReadRequiredScalar<std::string>(pNode, "address", p_path);
                    server.proxies.push_back(std::move(proxy));
                }
            }

            config.servers.emplace_back(std::move(server));
        }
    }
}

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
void ParseHttpSection(ServerManagerConfig& config, Yaml::Node& section) {
    ValidateAllowedKeys(section, "HTTP", {"enabled", "active", "address", "port"});

    const bool has_enabled = !section["enabled"].IsNone();
    const bool has_active = !section["active"].IsNone();
    if (has_enabled && has_active)
        ThrowConfigError("HTTP", "use only one of 'enabled' or 'active'");

    HttpServerConfig http_cfg;

    if (has_enabled)
        http_cfg.enabled = ReadRequiredScalar<bool>(section, "enabled", "HTTP");
    else if (has_active)
        http_cfg.enabled = ReadRequiredScalar<bool>(section, "active", "HTTP");

    if (auto address = ReadOptionalScalar<std::string>(section, "address", "HTTP"))
        http_cfg.address = std::move(*address);

    if (auto port = ReadOptionalScalar<uint16_t>(section, "port", "HTTP"))
        http_cfg.port = *port;

    config.http = std::move(http_cfg);
}
#endif

template <typename T> T *GetRawPointer(T *ptr) { return ptr; }
template <typename T> T *GetRawPointer(const std::shared_ptr<T>& ptr) { return ptr.get(); }
template <typename T> T *GetRawPointer(const std::unique_ptr<T>& ptr) { return ptr.get(); }
} // namespace

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
ServerManagerConfig ServerManager::receive_config_from_ipc(IPC& ipc) {
    std::optional<luxon::ser::Message> msg;

    // Poll the non-blocking IPC socket until the parent transmits the configuration
    while (!(msg = ipc.receive_message()))
        ;

    // Expect a GenericValueMessage containing the PFR-encoded ServerManagerConfig
    if (auto *gvm = msg.value().get_if<luxon::ser::GenericValueMessage>()) {
        auto decoded = pfr_codec::from_value<ServerManagerConfig>(gvm->value);
        if (!decoded)
            throw std::runtime_error("Failed to decode ServerManagerConfig from IPC: " + decoded.error().message);
        return std::move(*decoded);
    }

    throw std::runtime_error("Unexpected IPC message type during subprocess initialization");
}
#endif

ServerManagerConfig ServerManager::load_config_from_file(const std::string& config_file) { return parse_config(LoadFile(config_file)); }

ServerManagerConfig ServerManager::parse_config(const std::string& config_contents) {
    Yaml::Node root;

    try {
        Yaml::Parse(root, config_contents);
    } catch (const std::exception& e) {
        throw std::runtime_error(std::string("YAML Parsing failed: ") + e.what());
    }

    if (!root.IsMap())
        throw std::runtime_error("Root of config must be a map");

    if (!root["MaxConnections"].IsNone() && !root["CCU"].IsNone())
        ThrowConfigError("root", "use only one of 'MaxConnections' or 'CCU'");

    ServerManagerConfig config;

    for (auto it = root.Begin(); it != root.End(); it++) {
        std::string key = (*it).first;
        Yaml::Node& section = (*it).second;

        if (key == "Servers") {
            ParseServersSection(config, section);
        } else if (key == "RegionList") {
            if (section.IsScalar()) {
                config.region_list.push_back(section.As<std::string>());
            } else if( section.IsSequence() ) {
                for (Yaml::Iterator it = section.Begin(); it != section.End(); it.operator++(1)) {
                    Yaml::Node& region_node = (*it).second;
                    if (region_node.IsScalar()) {
                       config.region_list.push_back(region_node.As<std::string>());
                    } else {        
                        create_logger("ConfigParser")
                            ->warn("The 'RegionList' must be a valid scalar or valid sequence in yml. It is not, so the default value will be used.");
                        config.region_list.clear();
                        break;
                    }
                }
            } else { 
                create_logger("ConfigParser")
                    ->warn("The 'RegionList' must be a valid scalar or valid sequence in yml. It is not, so the default value will be used.");
            }
        } else if (key == "External") {
            create_logger("ConfigParser")
                ->warn("The 'External' configuration block is deprecated and will be ignored. Please migrate to using 'proxies' within the 'Servers' block.");
        } else if (key == "EnableIPv6") {
            config.enable_ipv6 = ReadNodeScalar<bool>(section, "EnableIPv6");
        } else if (key == "NoBanner") {
            config.no_banner = ReadNodeScalar<bool>(section, "NoBanner");
        } else if (key == "MaxConnections" || key == "CCU") {
            config.max_connections = ReadNodeScalar<unsigned>(section, key);
        } else if (key == "MaxGamePeers") {
            config.max_game_peers = ReadNodeScalar<unsigned>(section, "MaxGamePeers");
        } else if (key == "TickTimeBudget") {
            config.tick_time_budget = ReadNodeScalar<uint32_t>(section, "TickTimeBudget");
#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
        } else if (key == "SettingsDatabase") {
            config.settings_database_path = ReadNodeScalar<std::string>(section, "SettingsDatabase");
#else
        } else if (key == "SettingsDatabase") {
            // Accepted but ignored when settings DB support is not compiled in
#endif
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
        } else if (key == "HTTP") {
            ParseHttpSection(config, section);
#else
        } else if (key == "HTTP") {
            // Accepted but ignored when embedded HTTP support is not compiled in
#endif
        } else {
            ThrowConfigError(key, "unknown top-level key: " + key);
        }
    }

    return config;
}

ServerManager::ServerManager(const std::string& config_file) : ServerManager(load_config_from_file(config_file)) {}

ServerManager::ServerManager(ServerManagerConfig config
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
                             ,
                             IPC&& ipc
#endif
                             )
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    : parent_ipc_(std::move(ipc))
#endif
{
    log_ = create_logger(
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
        this->parent_ipc_.is_open() ? std::format("Subprocess {} ServerManager", this->parent_ipc_.get_fd()) :
#endif
                                    "ServerManager");
#ifdef LUXON_SERVER_ENABLE_VISUALIZER
    log_->set_level(log_level::trace);
#endif

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    if (parent_ipc_.is_open())
        config.no_banner = true;
#endif

    // Fill in the region list for the handler_nameserver
    region_list_ = (std::move(config.region_list));

    if (!config.no_banner) {
        static bool banner_printed = false;
        if (!banner_printed) {
            log_->info(" ");
            log_->info(" |===================== http://github.com/niansa/LuxonServer =====================|");
            log_->info(" | BSD 3-Clause License                                                           |");
            log_->info(" |                                                                                |");
            log_->info(" | Copyright (c) 2026, the Luxon Server contributors                              |");
            log_->info(" |                                                                                |");
            log_->info(" | Redistribution and use in source and binary forms, with or without             |");
            log_->info(" | modification, are permitted provided that the following conditions are met:    |");
            log_->info(" |                                                                                |");
            log_->info(" | 1. Redistributions of source code must retain the above copyright notice, this |");
            log_->info(" |    list of conditions and the following disclaimer.                            |");
            log_->info(" |                                                                                |");
            log_->info(" | 2. Redistributions in binary form must reproduce the above copyright notice,   |");
            log_->info(" |    this list of conditions and the following disclaimer in the documentation   |");
            log_->info(" |    and/or other materials provided with the distribution.                      |");
            log_->info(" |                                                                                |");
            log_->info(" | 3. Neither the name of the copyright holder nor the names of its               |");
            log_->info(" |    contributors may be used to endorse or promote products derived from        |");
            log_->info(" |    this software without specific prior written permission.                    |");
            log_->info(" |                                                                                |");
            log_->info(" | THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS \" AS IS \"  |");
            log_->info(" | AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE      |");
            log_->info(" | IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE ARE |");
            log_->info(" | DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT HOLDER OR CONTRIBUTORS BE LIABLE   |");
            log_->info(" | FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR CONSEQUENTIAL     |");
            log_->info(" | DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF SUBSTITUTE GOODS OR     |");
            log_->info(" | SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS INTERRUPTION) HOWEVER     |");
            log_->info(" | CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN CONTRACT, STRICT LIABILITY,  |");
            log_->info(" | OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE) ARISING IN ANY WAY OUT OF THE USE  |");
            log_->info(" | OF THIS SOFTWARE, EVEN IF ADVISED OF THE POSSIBILITY OF SUCH DAMAGE.           |");
            log_->info(" |                                                                                |");
            log_->info(" |____________ Set `NoBanner: true` in config to disable this banner! ____________|");
            log_->info(" ");
            banner_printed = true;
        }
    }

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    if (ipc.is_open())
        is_subprocess_ = true;
#endif

    configs_ = std::move(config.servers);
    enable_ipv6_ = config.enable_ipv6;
    max_connections_ = config.max_connections;
    max_game_peers_ = NormalizeMaxGamePeers(config.max_game_peers);
    tick_time_budget_ = config.tick_time_budget;
    if (tick_time_budget_ == 0)
        tick_time_budget_ = ~tick_time_budget_;

#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
    if (!config.settings_database_path.empty()) {
        log_->info("Using settings database!");
        settings_manager.emplace(config.settings_database_path);
    }
#endif

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    http_config_ = std::move(config.http);
#endif

    log_->info("Config looks alright, setting up accordingly");
    setup();
}

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
ServerManager::ServerManager(IPC&& ipc) : ServerManager(receive_config_from_ipc(ipc), std::move(ipc)) {}
#endif

std::string_view ServerManager::get_static_endpoint_address_str(std::string_view address) {
    for (auto& cfg : configs_) {
        if (cfg.external_address == address)
            return cfg.external_address;
        for (auto& proxy : cfg.proxies) {
            if (proxy.address == address)
                return proxy.address;
        }
    }

    log_->error("Failed to resolve endpoint address into static string: {}", address);
    return {};
}

std::string_view ServerManager::get_random_server_base_address(ServerType server_type) {
    ZoneScoped;
    std::vector<const ServerConfig *> candidates;
    candidates.reserve(configs_.size());

    // Collect all valid endpoints for requested type
    for (const auto& cfg : configs_)
        if (cfg.type == server_type && !cfg.external_address.empty())
            candidates.push_back(&cfg);

    // Handle cases where no config exists
    if (candidates.empty())
        throw std::runtime_error(std::format("No server configuration found for {}", ServerTypeToString(server_type)));

    // Return random endpoint from candidates
    static std::mt19937 generator{1234};
    std::uniform_int_distribution<size_t> distribution(0, candidates.size() - 1);
    return candidates[distribution(generator)]->external_address;
}

std::string_view ServerManager::resolve_server_address(ServerType type, ServerProtocol proto, std::string_view base_address) {
    for (const auto& cfg : configs_) {
        if (cfg.type == type && cfg.external_address == base_address) {
            if (proto == ServerProtocol::UDP)
                return cfg.external_address;

            for (const auto& proxy : cfg.proxies)
                if (proxy.protocol == proto)
                    return proxy.address;

            // Fallback
            return cfg.external_address;
        }
    }
    return base_address;
}

std::string_view ServerManager::get_random_server_address(ServerType server_type, ServerProtocol proto) {
    std::string_view base = get_random_server_base_address(server_type);
    return resolve_server_address(server_type, proto, base);
}

void ServerManager::run_scheduled_tasks() {
    ZoneScoped;

    if (scheduled_tasks_.empty())
        return;

    const auto& task = scheduled_tasks_.top();
    if (task.execution_time < startup_time_.get()) {
        auto callback = task.cb;
        scheduled_tasks_.pop();
        callback();
    }
}

void ServerManager::stun_keepalive(enet::EnetServer& server, uint16_t port) {
    if (!server.keepalive_stun_binding())
        log_->warn("[STUN:{}] Failed to keep alive server STUN binding", port);

    add_scheduled_task(17500, std::bind(&ServerManager::stun_keepalive, this, std::ref(server), port));
}

void ServerManager::run() {
    // Main Service Loop
    running_ = true;
    do {
        run_once();
    } while (running_ && Platform::cooperate());
    running_ = true;
}

bool ServerManager::run_once() {
    running_ = true;

    ZoneScoped;
    {
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
        // Start idle performance timer
        const auto start_time = std::chrono::steady_clock::now();
#endif

        // Run sock selector
        sock_selector_.run(125);

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
        // End idle performance timer
        const auto end_time = std::chrono::steady_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

        // Store metric
        idle_time.add(static_cast<unsigned>(duration));
#endif
    }
    {
        ZoneScopedN("run_once_busy");
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
        // Start busy performance timer
        const auto start_time = std::chrono::steady_clock::now();
#endif

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
        // Receive and process IPC messages from parent
        if (parent_ipc_.is_open())
            while (const auto ipc_msg = parent_ipc_.receive_message())
                process_parent_ipc_message(parent_ipc_, *ipc_msg);

        ipc_broadcast_skip_ = nullptr;
#endif

        // Check if slow update should be done
        const bool slow_update = last_slow_update_.get() > 250;
        if (slow_update)
            last_slow_update_.reset();

        // Dispatch and handle incoming application messages
        uint32_t remaining_timeout = tick_time_budget_;
        size_t servers_to_process = servers_.size();

        {
            ZoneScopedN("service_peers");

            // Round-robin over servers to prevent starvation
            while (servers_to_process > 0 && remaining_timeout > 0) {
                // Wrap around to beginning if end is hit
                if (next_server_it_ == servers_.end())
                    next_server_it_ = servers_.begin();

                auto& [port, server] = *next_server_it_;

                try {
                    // Service peers using whatever remains of global budget
                    if (!server.service_peers(remaining_timeout))
                        log_->warn("Queueing UDP datagrams on port {}!", port);
                } catch (const std::exception& e) {
                    log_->warn("Uncaught exception on port {}: {}", port, e.what());
                }

                // Move to the next server and decrement safety counter
                ++next_server_it_;
                --servers_to_process;
            }
        }

        if (servers_to_process > 0)
            log_->warn("Tick time budget exhausted! {} servers deferred to next tick.", servers_to_process);

        // Trigger updates
        if (slow_update) {
            ZoneScopedN("service_slow_updates");
#ifdef LUXON_ENET_ENABLE_METRICS
            // Tick enet metrics
            const auto enet_metrics_last_tick_ms = enet_metrics_last_tick_.get();
            if (enet_metrics_last_tick_ms > 1000) {
                const double enet_metrics_last_tick_s = double(enet_metrics_last_tick_ms) * 0.001;
                enet_metrics_last_tick_.reset();
                enet_metrics_.tick(enet_metrics_last_tick_s);
            }
#endif
            // Update connection handlers
            for (auto& connection : connections_) {
                try {
                    connection->HandleSlowUpdate();
                } catch (const std::exception& e) {
                    auto& peer = *connection->get_peer();
                    log_->warn("Disconnecting due to uncaught exception in slow update: {}", e.what());
                    peer.disconnect();
                }
            }
        }

        // Run scheduled tasks
        run_scheduled_tasks();
#ifdef LUXON_SERVER_ENABLE_WEBSERVER

        // Update HTTP server
        if (http_server_)
            http_server_->service_now();
#endif

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
        // Stop if parent has died
        if (is_subprocess_ && !parent_ipc_.is_open()) {
            stop();
            log_->info("Parent has closed the IPC connection. Stopping...");
        }
#endif

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
        // End busy performance timer
        const auto end_time = std::chrono::steady_clock::now();
        const auto duration = std::chrono::duration_cast<std::chrono::microseconds>(end_time - start_time).count();

        // Store metric
        busy_time.add(static_cast<unsigned>(duration));
#endif
    }

    FrameMark;
    return running_;
}

std::shared_ptr<App> ServerManager::get_app(const AppInfo& info) { return App::get(*this, std::string(info.id), std::string(info.version)); }

std::shared_ptr<App> ServerManager::try_get_app(const AppInfo& info) { return App::try_get(*this, std::string(info.id), std::string(info.version)); }

std::shared_ptr<Lobby> ServerManager::get_lobby(App& app, const LobbyInfo& info) { return app.get_lobby(info); }
std::expected<std::shared_ptr<Game>, std::string> ServerManager::get_game(Lobby& lobby, const GameInfo& info) {
    auto game = lobby.create_game(std::string(info.id), info.server_address, true);
    if (!game)
        return std::unexpected(game.error().debug_message.value_or("Unknown error"));
    return *game;
}

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
void ServerManager::ipc_broadcast(const ser::Message& message, bool parent, bool children) {
    if (parent)
        if (&parent_ipc_ != ipc_broadcast_skip_)
            if (parent_ipc_.is_open())
                parent_ipc_.send_message(message);
    if (children)
        for (auto& [port, ipc] : subprocesses_)
            if (&ipc != ipc_broadcast_skip_)
                if (ipc.is_open())
                    ipc.send_message(message);
}
#endif

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
void ServerManager::setup_http_server() {
    if (!http_config_ || !http_config_->enabled) {
        log_->debug("HTTP Server disabled in config.");
        return;
    }

    log_->info("Initializing HTTP Server on port {}", http_config_->port);
    http_server_.emplace(*this);
    http_server_->on_create_fd =
        std::bind(&SockSelector::add_read_fd, &sock_selector_, std::placeholders::_1, [this](int fd) { http_server_->service_later(fd); });
    http_server_->on_delete_fd = std::bind(&SockSelector::remove_read_fd, &sock_selector_, std::placeholders::_1);
    if (!http_server_->bind(http_config_->address, http_config_->port))
        log_->error("Failed to bind HTTP server to port {}! Is the port already in use?", http_config_->port);
}
#endif

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
void ServerManager::process_child_ipc_message(IPC& sender, const ser::Message& msg) {
    ipc_broadcast_skip_ = &sender;

    // Only accept event messages
    auto *event_msg = msg.get_if<ser::EventMessage>();
    if (!event_msg)
        return;

    return process_ipc_event(*event_msg);
}

void ServerManager::process_parent_ipc_message(IPC& sender, const ser::Message& msg) {
    ipc_broadcast_skip_ = &sender;

    // Only accept event messages
    auto *event_msg = msg.get_if<ser::EventMessage>();
    if (!event_msg)
        return;

    return process_ipc_event(*event_msg);
}

void ServerManager::process_ipc_event(const ser::EventMessage& event_msg) {
    const auto& params = event_msg.parameters;

    // Handle lobby/game updates
    if (event_msg.event_code == IPCEventCodes::GameUpdate || event_msg.event_code == IPCEventCodes::GameDelete) {
        // Find tracked external game
        const auto game_info = Game::decode_game_info(params);
        auto it = std::find_if(external_games_.begin(), external_games_.end(), [&](const std::shared_ptr<Game>& g) { return g->matches_game_info(game_info); });

        if (event_msg.event_code == IPCEventCodes::GameDelete) {
            // Handle game deletion
            if (it != external_games_.end()) {
                auto game = *it;

                // Make sure game expires immediately
                (*it)->empty_game_ttl = 0;

                // Remove from external games tracking
                external_games_.erase(it);
            }

            return;
        } else if (event_msg.event_code == IPCEventCodes::GameUpdate) {
            // Handle game update
            std::shared_ptr<Game> game;
            bool is_new = false;

            // If known, reuse it. Otherwise, create new external game
            if (it != external_games_.end()) {
                game = *it;
            } else {
                auto app = App::get(*this, std::string(game_info.lobby.app.id), std::string(game_info.lobby.app.version));
                if (!app) {
                    log_->error("Failed to synchronize game via IPC: Unable to get matching application");
                    return;
                }

                auto lobby = app->get_lobby({game_info.lobby.name, game_info.lobby.type});
                if (!lobby) {
                    log_->error("Failed to synchronize game via IPC: Unable to get matching lobby");
                    return;
                }

                std::string_view address;
                if (auto *address_ptr = params[DictKeyCodes::LoadBalancing::Address].get_ptr<std::string>())
                    address = *address_ptr;
                if (address.empty()) {
                    log_->error("Failed to synchronize game via IPC: Unable to get matching address");
                    return;
                }

                auto expected_game = lobby->create_game(std::string(game_info.id), address, true);
                if (expected_game) {
                    game = std::move(expected_game.value());
                    if (game) {
                        // Persist locally
                        external_games_.push_back(game);
                        is_new = true;
                    } else {
                        log_->error("Failed to synchronize game via IPC: Unable to create matching game");
                        return;
                    }
                }
            }

            // Apply updated properties
            auto props_ptr = params[DictKeyCodes::Properties::GameProperties].get_or<ser::HashtablePtr>(nullptr);
            if (props_ptr) {
                // Handle PlayerCount separately
                if (auto it = props_ptr->find(GameProps::PlayerCount); it != props_ptr->end())
                    it->second.store_if<uint8_t>(game->dummy_peer_count);
                if (game->dummy_peer_count > 0)
                    game->is_created = true;

                // Apply all other properties
                game->insert_game_props(*props_ptr);
            }

            if (is_new)
                reset_persistent_peer_game_ownership(*this, *game);

            return;
        }
    }

    // Handle persistent peer stores
    if (event_msg.event_code == IPCEventCodes::PersistentPeerStore) {
        // Get token
        std::string_view token;
        if (const auto& token_param = params[DictKeyCodes::LoadBalancing::Token]; token_param.is<std::string>()) {
            token = token_param.get<std::string>();
        } else {
            log_->error("Failed to decode persistent peer received via IPC: Could not get token string");
            return;
        }

        // Get user id
        std::string_view user_id;
        if (const auto& user_id_param = params[DictKeyCodes::LoadBalancing::UserId]; user_id_param.is<std::string>()) {
            user_id = user_id_param.get<std::string>();
        } else {
            log_->error("Failed to decode persistent peer received via IPC: Could not get user id string");
            return;
        }

        auto pp = create_persistent_peer();
        pp->token = token;
        pp->user_id = user_id;

        const auto game_info = Game::decode_game_info(params);
        pp->app = get_app(game_info.lobby.app);
        if (!pp->app) {
            log_->error("Failed to store persistent peer received via IPC: Could not get associated app");
            return;
        }

        if (auto lobby = get_lobby(*pp->app, game_info.lobby))
            pp->invite(game_info);

        store_persistent_peer(*this, std::move(pp));

        return;
    }
}
#endif

void ServerManager::setup() {
    ZoneScoped;

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    setup_http_server();
#endif

    enet::EnetPeerConfig cfg;
    cfg.time_base = enet::EnetPeer::create_time_base();
    cfg.time_ping_interval_ms = 1000;
    cfg.disconnect_timeout_ms = 5000;

    // Create servers
    for (const auto& config : configs_) {
        if (config.is_routing_only)
            continue;

        if (config.subprocess) {
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
            setup_subprocess(config);
            continue; // Skip the native bind routine in the parent for this iteration
#else
            log_->warn("Subprocess enabled in config, but not enabled at compile time");
#endif
        }

        // Create enet server and configure it
        log_->info("Setting up {} on port {}", ServerTypeToString(config.type), config.port);

        auto& server = servers_
                           .try_emplace(config.port, cfg
#ifdef LUXON_ENET_ENABLE_METRICS
                                        ,
                                        enet_metrics_
#endif
                                        )
                           .first->second;

        server.on_peer_connected = [this, &config](std::shared_ptr<enet::EnetPeer> enetPeer) {
            // Construct peer
            auto peer = std::make_shared<Peer>();
            peer->enet_peer = enetPeer;
            peer->log = create_logger(std::format("Peer {}@{}", enetPeer->peer_id(), enetPeer->remote_endpoint()->to_string()));
            peer->protocol = std::make_unique<ser::GpBinaryV18>(); // Default version
#ifdef LUXON_SERVER_ENABLE_VISUALIZER
            peer->log->set_level(log_level::trace);
#endif
            peer->log->info("Peer {} constructed with {} handler", peer->enet_peer->peer_id(), ServerTypeToString(config.type));

            // Construct handler
            auto handler = ServerTypeToHandler(config.type, *this, std::move(peer));
            handler->set_allow_unsolicited(config.allow_unsolicited);

            // Handler pointer must be owning with plugins enabled to ensure no destruction while coroutine is active

            auto *handler_ptr = handler.get();
            auto *raw_handler = GetRawPointer(handler_ptr);

            // Install callbacks
            enetPeer->on_log_message = [this, handler = raw_handler](enet::LogLevel enet_level, std::string_view message) {
                // Convert log level
                log_level level;
                switch (enet_level) {
                case luxon::enet::LogLevel::Warning:
                    level = log_level::warn;
                    break;
                case luxon::enet::LogLevel::Error:
                    level = log_level::err;
                    break;
                }

                // Emit log message
                handler->get_peer()->log->log(level, "[ENet] {}", message);
            };

            enetPeer->on_state_changed = [this, handler = raw_handler](enet::EnetConnectionState state) {
                [](ServerManager& self, enet::EnetConnectionState state, server::HandlerBase *handler) -> Awaitable<> {
                    try {
                        lco_await handler->HandleENetConnectionStateChange(state);
                    } catch (const std::exception& e) {
                        auto& peer = *handler->get_peer();
                        peer.log->warn("Uncaught exception in ENet connect state change handler: {}", e.what());
                    }

                    if (state == luxon::enet::EnetConnectionState::Connected)
                        lco_await handler->HandleConnect();

                    if (state == enet::EnetConnectionState::Disconnected) {
                        lco_await handler->HandleDisconnect();
                        // Self-destruct handler, this will invalidate the pointer
                        self.add_scheduled_task(0, [&self, handler]() { self.connections_.remove_if([handler](auto& v) { return v.get() == handler; }); });
                    }
                }(*this, state, handler)_lco_detached;
            };


            enetPeer->on_payload_command = [this, handler_capture = handler_ptr](enet::EnetCommand&& cmd) {
                [](ServerManager& self, enet::EnetCommand cmd, auto h_token) -> Awaitable<> {
                    auto *handler = h_token;
                    auto& peer = handler->get_peer();

#ifdef LUXON_SERVER_ENABLE_VISUALIZER
                    peer->log->trace("Received message using mode {} on channel {}:", static_cast<int>(enet::FlagsToEnetDeliveryMode(cmd.header.flags)),
                                     cmd.header.channel_id);
                    if (!visualizer::print_ser_message(cmd.get_payload(), 2, *peer->protocol)) {
                        if (!visualizer::print_http_message(cmd.get_payload(), 2)) {
                            peer->log->error("Message not understood!");
                            visualizer::helpers::print_hex_dump(cmd.get_payload(), 2);
                        }
                    }
#endif

                    try {
                        lco_await handler->HandleENetCommand(std::move(cmd));
                    } catch (const std::exception& e) {
                        peer->log->critical("Disconnecting due to uncaught exception in ENet command handler: {}", e.what());
                        peer->disconnect();
                    }
                }(*this, std::move(cmd), std::move(handler_capture))_lco_detached;
            };

            // Add to connection list
            auto& handlerPtr = connections_.emplace_back(std::move(handler));           
        };

        server.on_stun_bind = [this, &config](enet::EnetEndpoint&& ep) {
            log_->info("[STUN:{}] NAT punch complete  --> {} <-- ", config.port, ep.to_string());
        };

        // Make server ready for listening
        log_->info("Starting {} on port {}", ServerTypeToString(config.type), config.port);

        if (!server.bind(config.port, enable_ipv6_)) {
            log_->error("Failed to bind {} to port {}!", ServerTypeToString(config.type), config.port);
            continue;
        }

        // Start STUN binding request if enabled
        if (!config.stun_server_host.empty()) {
            if (server.request_stun_binding(config.stun_server_host.c_str(), enable_ipv6_, config.stun_server_port)) {
                log_->info("[STUN:{}] Starting NAT punch via STUN server: {}:{}", config.port, config.stun_server_host, config.stun_server_port);
                stun_keepalive(server, config.port);
            } else {
                log_->error("[STUN:{}] Failed to start NAT punch via STUN server", config.port);
            }
        }

        // Add server to sock selector
        if (!sock_selector_.add_read_fd(server.native_handle(), [&server](int fd) {
                ZoneScopedN("service_server");
                server.service_self();
            }))
            log_->error("Failed to add new server to sock selector!", ServerTypeToString(config.type), config.port);
    }

    next_server_it_ = servers_.end();
}

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
void ServerManager::setup_subprocess(const ServerConfig& config) {
    log_->info("Setting up {} on port {} as a subprocess", ServerTypeToString(config.type), config.port);

    if (!handle_start_subprocess) {
        log_->error("handle_start_subprocess is not configured! Cannot start subprocess for port {}", config.port);
        return;
    }

    auto ipc_opt = IPC::create();
    if (!ipc_opt) {
        log_->error("Failed to create IPC socket pair for port {}", config.port);
        return;
    }

    // Emplace IPC into manager's tracked subprocesses
    IPC& ipc = subprocesses_.try_emplace(config.port, std::move(*ipc_opt)).first->second;

    // Set sock selector handler
    sock_selector_.add_read_fd(ipc.get_fd(), [this, &ipc](SockSelector::socket_t) {
        while (const auto ipc_msg = ipc.receive_message())
            process_child_ipc_message(ipc, *ipc_msg);
        ipc_broadcast_skip_ = nullptr;
    });

    // Trigger external subprocess handler using new child socket
    handle_start_subprocess(IPC::socket_to_string(ipc.get_child_fd()));
    ipc.close_child_fd();

    // Synthesize child's configuration state
    ServerManagerConfig child_config;
    child_config.enable_ipv6 = enable_ipv6_;
    child_config.max_connections = max_connections_;
    child_config.max_game_peers = max_game_peers_;
    child_config.tick_time_budget = tick_time_budget_;
#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
    if (!settings_database_path.empty())
        child_config.settings_database_path = settings_database_path + ":ro";
#endif

    // Make sure child actually binds server, doesn't endlessly fork and knows about all other servers
    for (const auto& cfg : configs_) {
        ServerConfig child_cfg = cfg;
        child_cfg.subprocess = false;

        if (cfg.port == config.port && cfg.type == config.type)
            child_cfg.is_routing_only = false;
        else
            child_cfg.is_routing_only = true;
        child_config.servers.emplace_back(std::move(child_cfg));
    }

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    // Set up embedded HTTP server on child
    if (http_config_) {
        child_config.http = http_config_;
        child_config.http->port += ipc.get_fd(); // Use a different port
    }
#endif

    // Encode the configuration object
    auto val_res = pfr_codec::to_value(child_config, {.zero_copy = true});
    if (val_res)
        ipc.send_message(luxon::ser::GenericValueMessage{std::move(*val_res)});
    else
        log_->error("Failed to serialize config for subprocess on port {}: {}", config.port, val_res.error().message);
}
#endif
} // namespace server
