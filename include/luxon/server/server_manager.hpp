// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "global.hpp"
#include "metrics.hpp"
#include "handler_base.hpp"
#include "peer_persistence.hpp"
#include "string_hash.hpp"
#include "logger.hpp"
#include "hookpoints.hpp"
#include "sock_selector.hpp"
#include "game.hpp"
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
#include "ipc.hpp"
#endif
#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
#include "settings_manager.hpp"
#endif
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
#include "http_server.hpp"
#endif
#ifdef LUXON_SERVER_MULTITHREADED
#include "sidethread.hpp"
#endif

#include <string>
#include <vector>
#include <list>
#include <queue>
#include <unordered_map>
#include <optional>
#include <memory>
#include <utility>
#include <functional>
#ifdef LUXON_SERVER_MULTITHREADED
#include <mutex>
#endif
#include <cstdint>
#include <luxon/enet_peer.hpp>
#ifdef LUXON_ENET_ENABLE_METRICS
#include <luxon/enet_metrics.hpp>
#endif
#include <commoncpp/timer.hpp>

namespace server {
class HandlerBase;
struct Peer;
struct PeerPersistent;
class App;

template <typename T> using HandlerPtr = std::unique_ptr<T>;

enum class ServerType { None, NameServer, MasterServer, GameServer };

struct ProxyConfig {
    ServerProtocol protocol;
    std::string address;
};

struct ServerConfig {
    ServerType type = ServerType::None;
    uint16_t port = 0;

    bool allow_unsolicited = false, subprocess = false, is_routing_only = false;

    std::string stun_server_host;
    uint16_t stun_server_port = 19302;

    std::string external_address;
    std::vector<ProxyConfig> proxies;
};

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
struct HttpServerConfig {
    bool enabled = false;
    std::string address = "0.0.0.0";
    uint16_t port = 5088;
};
#endif

///
/// \brief Runtime configuration for ServerManager
/// Can be built directly in C++ when embedding/loading the server as a shared library.
///
struct ServerManagerConfig {
    std::vector<ServerConfig> servers;
    std::vector<std::string> region_list;
    bool enable_ipv6 = true;
    bool no_banner = false;
    unsigned max_connections = 0;
    ///
    /// \brief Maximum peers per game
    /// Values >= 255 are normalized to the legacy internal value 0 when applied.
    ///
    unsigned max_game_peers = 0;

    ///
    /// \brief Tick time budget
    /// Maximum amount of time run_once is allowed to take to service servers/peers.
    /// Lower value means more clients can safely be serviced at once, but processing latency will increase faster.
    ///
    uint32_t tick_time_budget = 2000;

#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
    ///
    /// \brief Settings database file path
    /// Settings database will be initialized/read from this path.
    ///
    std::string settings_database_path;
#endif

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    std::optional<HttpServerConfig> http;
#endif

    ///
    /// \brief Add a listening server
    ///
    ServerManagerConfig& add_server(ServerType type, uint16_t port) {
        ServerConfig server;
        server.type = type;
        server.port = port;
        servers.push_back(std::move(server));
        return *this;
    }

    ///
    /// \brief Add a listening server and a matching external UDP endpoint
    ///
    ServerManagerConfig& add_server(ServerType type, uint16_t port, std::string external_udp_address) {
        ServerConfig server;
        server.type = type;
        server.port = port;
        server.external_address = std::move(external_udp_address);
        servers.push_back(std::move(server));
        return *this;
    }

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    ///
    /// \brief Enable embedded HTTP server
    ///
    ServerManagerConfig& enable_http(std::string address = "0.0.0.0", uint16_t port = 5088) {
        http = HttpServerConfig{true, std::move(address), port};
        return *this;
    }

    ///
    /// \brief Disable embedded HTTP server
    ///
    ServerManagerConfig& disable_http() {
        http.reset();
        return *this;
    }
#endif
};

class ServerManager {
    struct ScheduledTask {
        unsigned execution_time;
        std::function<void()> cb;
        bool operator>(const ScheduledTask& other) const { return execution_time > other.execution_time; }
    };

public:
    std::unordered_map<std::pair<std::string, std::string>, std::weak_ptr<App>, StringPairHasher> apps;
    std::vector<std::unique_ptr<PeerPersistent>> peer_persistent_data;
#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
    std::optional<SettingsManager> settings_manager;
    std::string settings_database_path;
#endif

#ifdef LUXON_SERVER_ENABLE_HOOKPOINTS
    Hookpoints hookpoints;
#endif

    // For use by the handler_nameserver
    const std::vector<std::string>& get_region_list() const { return region_list_; }

private:
    SockSelector sock_selector_;

    // For use by the handler_nameserver
    std::vector<std::string> region_list_;

    std::shared_ptr<logger> log_;
    std::vector<ServerConfig> configs_;
    std::unordered_map<uint16_t, enet::EnetServer> servers_;
    decltype(servers_)::iterator next_server_it_;
    std::list<HandlerPtr<HandlerBase>> connections_;
    std::vector<std::shared_ptr<Game>> external_games_;
    std::priority_queue<ScheduledTask, std::vector<ScheduledTask>, std::greater<ScheduledTask>> scheduled_tasks_;
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    IPC parent_ipc_;
    IPC *ipc_broadcast_skip_{};
    bool is_subprocess_{};
    std::unordered_map<uint16_t, IPC> subprocesses_;
#endif
#ifdef LUXON_SERVER_MULTITHREADED
    std::mutex scheduled_tasks_mutex_;
#endif

    common::Timer startup_time_;
    common::Timer last_slow_update_;

#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    std::optional<HttpServerConfig> http_config_;
    std::optional<HttpServer> http_server_;
#endif

#ifdef LUXON_ENET_ENABLE_METRICS
    enet::Metrics enet_metrics_;
    common::Timer enet_metrics_last_tick_;
#endif

    bool running_ = true;

    bool enable_ipv6_ = true;
    unsigned max_connections_ = 0;
    uint8_t max_game_peers_ = 0;
    uint32_t tick_time_budget_ = 2000;

    void setup();
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    void setup_subprocess(const ServerConfig& config);
#endif
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    void setup_http_server();
#endif

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    void process_child_ipc_message(IPC& sender, const luxon::ser::Message& msg);
    void process_parent_ipc_message(IPC& sender, const luxon::ser::Message& msg);
    void process_ipc_event(const ser::EventMessage& event_msg);
#endif

    void run_scheduled_tasks();
    void stun_keepalive(enet::EnetServer& server, uint16_t port);

public:
#ifdef LUXON_SERVER_ENABLE_WEBSERVER
    Metric busy_time, idle_time;
#endif

    ///
    /// \brief Construct manager from YAML config file
    ///
    explicit ServerManager(const std::string& config_file);

    ///
    /// \brief Construct manager directly from C++ configuration
    ///
    explicit ServerManager(ServerManagerConfig config
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
                           ,
                           IPC&& ipc = {}
#endif
    );

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    ///
    /// \brief Construct manager from IPC child fd
    /// \note Configuration will be received from parent via IPC
    ///
    explicit ServerManager(IPC&& ipc);

    ///
    /// \brief Receive ServerManagerConfig from parent
    ///
    static ServerManagerConfig receive_config_from_ipc(IPC& ipc);
#endif

    ///
    /// \brief Load ServerManagerConfig from YAML file
    ///
    static ServerManagerConfig load_config_from_file(const std::string& config_file);

    ///
    /// \brief Parse ServerManagerConfig from YAML contents
    ///
    static ServerManagerConfig parse_config(const std::string& config_contents);

    ///
    /// \brief Runs server until stop() is called
    ///
    void run();
    ///
    /// \brief Ticks once
    /// \return True if the server should continue running, false if stop() was called
    /// \note Non-blocking if polling is enabled
    ///
    bool run_once();
    ///
    /// \brief Signals the server to stop running asap
    /// \note It might take the server up to about 125 milliseconds to stop if polling is disabled
    ///
    void stop() { running_ = false; }

    ///
    /// \brief Schedules a function to be called from main loop
    /// \param delay_ms Delay in milliseconds
    /// \param callback Function to be called
    ///
    void add_scheduled_task(unsigned delay_ms, std::function<void()>&& callback) {
        unsigned target_time = startup_time_.get() + delay_ms;
#ifdef LUXON_SERVER_MULTITHREADED
        std::scoped_lock L(scheduled_tasks_mutex_);
#endif
        scheduled_tasks_.push({target_time, std::move(callback)});
    }

    ///
    /// \brief Retrieves an application instance based on the provided info
    /// \param info Identifying information for the target application
    /// \return Shared pointer to the application
    ///
    std::shared_ptr<App> get_app(const AppInfo& info);
    ///
    /// \brief Retrieves an application instance based on the provided info if it already exists
    /// \param info Identifying information for the target application
    /// \return Shared pointer to the application
    ///
    std::shared_ptr<App> try_get_app(const AppInfo& info);

    ///
    /// \brief Retrieves a lobby instance within a specific application
    /// \param app Parent application hosting the lobby
    /// \param info Identifying information for the target lobby
    /// \return Shared pointer to the lobby
    ///
    std::shared_ptr<Lobby> get_lobby(App& app, const LobbyInfo& info);
    ///
    /// \brief Retrieves a lobby instance
    /// \param info Identifying information for the target lobby
    /// \return Shared pointer to the lobby
    ///
    std::shared_ptr<Lobby> get_lobby(const LobbyInfo& info) { return get_lobby(*get_app(info.app), info); }
    ///
    /// \brief Retrieves a lobby instance
    /// \param info Identifying information for the target lobby if app already exists
    /// \return Shared pointer to the lobby
    ///
    std::shared_ptr<Lobby> try_get_lobby(const LobbyInfo& info) { return get_lobby(*try_get_app(info.app), info); }

    ///
    /// \brief Retrieves a game instance within a specific lobby
    /// \param lobby Parent lobby hosting the game
    /// \param info Identifying information for the target game
    /// \return Expected containing a shared pointer to the game on success, or an error message string on failure
    ///
    std::expected<std::shared_ptr<Game>, std::string> get_game(Lobby& lobby, const GameInfo& info);
    ///
    /// \brief Retrieves a game instance within a specific app
    /// \param app Parent application hosting the game
    /// \param info Identifying information for the target game
    /// \return Expected containing a shared pointer to the game on success, or an error message string on failure
    ///
    std::expected<std::shared_ptr<Game>, std::string> get_game(App& app, const GameInfo& info) { return get_game(*get_lobby(app, info.lobby), info); }
    ///
    /// \brief Retrieves a game instance
    /// \param info Identifying information for the target game
    /// \return Expected containing a shared pointer to the game on success, or an error message string on failure
    ///
    std::expected<std::shared_ptr<Game>, std::string> get_game(const GameInfo& info) { return get_game(*get_lobby(info.lobby), info); }
    ///
    /// \brief Retrieves a game instance
    /// \param info Identifying information for the target game if app already exists
    /// \return Expected containing a shared pointer to the game on success, or an error message string on failure
    ///
    std::expected<std::shared_ptr<Game>, std::string> try_get_game(const GameInfo& info) { return get_game(*try_get_lobby(info.lobby), info); }

    bool is_game_external(Game& game) const {
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
        for (const auto& external_game : external_games_)
            if (external_game.get() == &game)
                return true;
#endif
        return false;
    }

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    ///
    /// \brief Broadcasts a serialized message via IPC to parent and/or child processes
    /// \param message The serialized message payload to send
    /// \param parent True to transmit the message to the parent process
    /// \param children True to transmit the message to all child subprocesses
    /// \param skip Optional IPC connection pointer to exclude from the broadcast (e.g., the original sender)
    ///
    void ipc_broadcast(const ser::Message& message, bool parent, bool children);

    ///
    /// \brief Broadcasts a serialized message via IPC to all connected processes (parent and children)
    /// \param message The serialized message payload to send
    /// \param skip Optional IPC connection pointer to exclude from the broadcast (e.g., the original sender)
    ///
    void ipc_broadcast(const ser::Message& message) { return ipc_broadcast(message, true, true); }
#endif

    std::string_view get_static_endpoint_address_str(std::string_view address);

    ///
    /// \brief Gets the base external address of a random server of given type
    /// \param server_type Type of server to get
    /// \return Base externally reachable address of server, e.g. "104.18.26.120:5058"
    ///
    std::string_view get_random_server_base_address(ServerType server_type);

    ///
    /// \brief Resolves a specific proxy address for a base server address based on requested protocol
    /// \param type Type of server to get
    /// \param proto Protocol of server to get
    /// \param base_address The base UDP external address to resolve a proxy for
    /// \return Proxy address if found, otherwise returns base_address
    ///
    std::string_view resolve_server_address(ServerType type, ServerProtocol proto, std::string_view base_address);

    ///
    /// \brief Gets the resolved external address (or proxy) of a random server of given type
    /// \param server_type Type of server to get
    /// \param proto Protocol of server to get
    /// \return Resolved address
    ///
    std::string_view get_random_server_address(ServerType server_type, ServerProtocol proto = ServerProtocol::UDP);

    ///
    /// \brief Gets a list of active connections to this instance
    /// \return Linked list of handlers representing a connection
    ///
    const std::list<HandlerPtr<HandlerBase>>& get_connections() { return connections_; }

    ///
    /// \brief Counts connections to servers on this instance of any type
    /// \return Amount of connections
    ///
    size_t get_connection_count() { return connections_.size(); }
    ///
    /// \brief Counts connections to servers on this instance with handler of or derived from given type
    /// \return Amount of connections
    ///
    template <class HandlerT> size_t get_connection_count() {
        size_t fres = 0;
        for (const auto& connection : connections_)
            fres += !!dynamic_cast<HandlerT *>(connection.get());
        return fres;
    }
    ///
    /// \brief Gets the maximum allowed connections
    ///
    unsigned get_max_connections() const { return max_connections_; }
    ///
    /// \brief Gets the maximum allowed peers per game
    ///
    uint8_t get_max_game_peers() const { return max_game_peers_; }

    ///
    /// \brief Gets list of servers
    ///
    auto get_servers() {
        std::vector<std::pair<uint16_t, enet::EnetServer *>> fres;
        fres.reserve(servers_.size());
        for (auto& [port, server] : servers_)
            fres.push_back({port, &server});
        return fres;
    }
#ifdef LUXON_ENET_ENABLE_METRICS

    ///
    /// \brief Gets all metrics exposed by ENet
    ///
    const enet::Metrics& get_enet_metrics() const { return enet_metrics_; }
#endif

    static std::function<void(const std::string& fd)> handle_start_subprocess;
};
} // namespace server
