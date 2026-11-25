// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "http_server.hpp"
#include "json.hpp"
#include "server_manager.hpp"
#include "apps.hpp"
#include "lobby.hpp"
#include "game.hpp"
#include "peer.hpp"
#include "peer_persistence.hpp"
#include "handler_base.hpp"
#include "logger.hpp"

#ifdef LUXON_USE_EMBED_RESOURCE
#include <EmbeddedResource.h>
#else
#include "incbin.h"
#endif

#include <format>
#include <charconv>
#include <algorithm>
#include <unordered_map>
#include <fcntl.h>
#include <luxon/http_parser.hpp>
#include <luxon/ser_types.hpp>
#include <luxon/enet_peer.hpp>
#include <luxon/enet_metrics.hpp>
#include <commoncpp/utils.hpp>
#include <tracy/Tracy.hpp>

#ifdef _WIN32
#define CLOSE_SOCKET closesocket
#define SHUT_WR SD_SEND
#else
#include <sys/ioctl.h>
#include <arpa/inet.h>
#define CLOSE_SOCKET close
#endif

#ifdef LUXON_USE_EMBED_RESOURCE
DECLARE_RESOURCE_COLLECTION(http_resources);
DECLARE_RESOURCE(http_resources, index_html);
DECLARE_RESOURCE(http_resources, stats_html);
DECLARE_RESOURCE(http_resources, style_css);
#define GET_RESOURCE(name) LOAD_RESOURCE(http_resources, name).data
#else
extern "C" {
INCBIN(index_html, LUXON_SERVER_WEB_ROOT "/index.html");
INCBIN(stats_html, LUXON_SERVER_WEB_ROOT "/stats.html");
INCBIN(style_css, LUXON_SERVER_WEB_ROOT "/style.css");
}
#define GET_RESOURCE(name) {incbin_##name##_start, static_cast<size_t>(reinterpret_cast<intptr_t>(incbin_##name##_end - incbin_##name##_start))}
#endif

#ifdef __wasi__
#define FCNTL socket_fcntl
#else
#define FCNTL fcntl
#endif

using json = nlohmann::json;
using namespace luxon::ser;

namespace server {
namespace {
void set_nonblocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#elif defined(__BLOCKSDS__)
    int mode = 1;
    ioctl(fd, FIONBIO, &mode);

    // Disable Nagle's algorithm
    int yes = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, reinterpret_cast<const char *>(&yes), sizeof(yes));
#else
    int flags = FCNTL(fd, F_GETFL, 0);
    FCNTL(fd, F_SETFL, flags | O_NONBLOCK);
#endif
}

std::string bytes_to_hex(const std::vector<uint8_t>& bytes) {
    std::string res;
    res.reserve(bytes.size() * 2);
    static const char hex[] = "0123456789ABCDEF";
    for (const uint8_t b : bytes) {
        res += hex[b >> 4];
        res += hex[b & 0x0F];
    }
    return res;
}

void url_decode_in_place(std::string& path) {
    ZoneScoped;

    std::string result;
    result.reserve(path.size());
    for (size_t i = 0; i < path.size(); ++i) {
        if (path[i] == '+') {
            result += ' ';
        } else if (path[i] == '%' && i + 2 < path.size()) {
            std::string hex = path.substr(i + 1, 2);
            auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'); };
            if (is_hex(hex[0]) && is_hex(hex[1])) {
                result += static_cast<char>(std::stoi(hex, nullptr, 16));
                i += 2;
            } else {
                result += '%';
            }
        } else {
            result += path[i];
        }
    }
    path = std::move(result);
}

namespace json_conv {
json photon_val_to_json(const Value& val);

json photon_dict_to_json(const Dictionary& dict) {
    ZoneScoped;

    json j = json::object();
    for (const auto& [k, v] : dict)
        j[std::to_string(k)] = photon_val_to_json(v);
    return j;
}

json photon_hash_to_json(const Hashtable& hash) {
    ZoneScoped;

    json j = json::object();
    for (const auto& [k, v] : hash) {
        std::string key_str;
        if (k.is<std::string>())
            key_str = k.get<std::string>();
        else if (k.is<uint8_t>())
            key_str = std::to_string(k.get<uint8_t>());
        else if (k.is<int32_t>())
            key_str = std::to_string(k.get<int32_t>());
        else if (k.is<int16_t>())
            key_str = std::to_string(k.get<int16_t>());
        else
            key_str = "(complex_key)";
        j[key_str] = photon_val_to_json(v);
    }
    return j;
}

json photon_val_to_json(const Value& val) {
    ZoneScoped;

    return val.value.visit([](auto&& arg) -> json {
        using T = std::decay_t<decltype(arg)>;
        if constexpr (std::is_same_v<T, std::monostate>)
            return nullptr;
        else if constexpr (std::is_same_v<T, bool>)
            return arg;
        else if constexpr (std::is_arithmetic_v<T>)
            return arg;
        else if constexpr (std::is_same_v<T, std::string>)
            return arg;
        else if constexpr (std::is_same_v<T, std::vector<uint8_t>>)
            return bytes_to_hex(arg);
        else if constexpr (std::is_same_v<T, std::vector<std::string>>)
            return arg;
        else if constexpr (std::is_same_v<T, Dictionary>)
            return photon_dict_to_json(arg);
        else if constexpr (std::is_same_v<T, std::shared_ptr<Hashtable>>)
            return arg ? photon_hash_to_json(*arg) : json(nullptr);
        else if constexpr (std::is_same_v<T, ObjectArray>) {
            json arr = json::array();
            for (const auto& v : arg)
                arr.push_back(photon_val_to_json(v));
            return arr;
        } else
            return "<complex>";
    });
}
} // namespace json_conv
} // namespace

HttpServer::HttpServer(ServerManager& manager) : server_manager_(manager) {
    log_ = create_logger("HTTPServer");
#ifdef LUXON_SERVER_ENABLE_VISUALIZER
    log_->set_level(log_level::trace);
#endif
#if defined(_WIN32)
    WSADATA wsa;
    WSAStartup(MAKEWORD(2, 2), &wsa);
#endif
}

HttpServer::~HttpServer() {
    if (server_fd_ != -1) {
        CLOSE_SOCKET(server_fd_);
        on_delete_fd(server_fd_);
    }
    for (auto& c : clients_) {
        CLOSE_SOCKET(c.fd);
        on_delete_fd(c.fd);
    }
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool HttpServer::bind(const std::string& address, uint16_t port) {
    ZoneScoped;

    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return false;

    const int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
    set_nonblocking(server_fd_);

    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#if defined(LUXON_ENET_HAS_PTON)
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) <= 0) {
#elif defined(_WIN32)
    addr.sin_addr.s_addr = inet_addr(address.c_str());
    if (addr.sin_addr.s_addr == INADDR_NONE && address != "255.255.255.255") {
#else
    if (inet_aton(address.c_str(), &addr.sin_addr) == 0) {
#endif
        log_->error("Invalid address: {}", address);
        CLOSE_SOCKET(server_fd_);
        return false;
    }

    if (::bind(server_fd_, (struct sockaddr *)&addr, sizeof(addr)) < 0)
        return false;
    if (::listen(server_fd_, 5) < 0)
        return false;

    on_create_fd(server_fd_);

    log_->info("Listening on http://{}:{}", address, port);
    return true;
}

void HttpServer::service_later(int fd) { servicable_fds_.push_back(fd); }

void HttpServer::service_now() {
    ZoneScoped;

    if (server_fd_ == -1)
        return;

    const auto now = std::chrono::steady_clock::now();
    constexpr auto IDLE_TIMEOUT = std::chrono::seconds(30);

    if (clients_.size() < MAX_CLIENTS) {
        if (std::ranges::contains(servicable_fds_, server_fd_)) {
            sockaddr_in cli_addr;
            socklen_t clilen = sizeof(cli_addr);
            socket_t new_fd = accept(server_fd_, reinterpret_cast<struct sockaddr *>(&cli_addr), &clilen);
            if (new_fd >= 0) {
                set_nonblocking(new_fd);
                clients_.push_back({new_fd, "", "", false, false, false, now});
                on_create_fd(new_fd);
            }
        }
    }

    for (auto& client : clients_) {
        if (client.mark_for_delete)
            continue;

        if (now - client.last_activity > IDLE_TIMEOUT) {
            log_->warn("Client timed out (fd: {})", client.fd);
            client.mark_for_delete = true;
            continue;
        }

        // Handle Incoming Data
        if (std::ranges::contains(servicable_fds_, client.fd)) {
            char buffer[4096];
            const int n = recv(client.fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
                client.last_activity = now;

                if (client.request_buffer.size() + n > MAX_PAYLOAD_SIZE) {
                    send_error(client, 413, "Payload Too Large");
                    client.close_after_write = true;
                    continue;
                }
                client.request_buffer.append(buffer, n);
                // Only parse if we haven't decided to close yet
                if (!client.close_after_write && client.request_buffer.find("\r\n\r\n") != std::string::npos) {
                    handle_client_data(client);
                }
            } else if (n == 0) {
                // Client closed gracefully (ACKed our shutdown)
                client.mark_for_delete = true;
            } else if (n < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                // Socket error
                client.mark_for_delete = true;
            }
        }

        // Handle Outgoing Data
        if (!client.write_buffer.empty()) {
            int sent = send(client.fd, client.write_buffer.data(), client.write_buffer.size(), 0);
            if (sent > 0) {
                client.last_activity = now;
                client.write_buffer.erase(0, sent);
            } else if (sent < 0 && errno != EWOULDBLOCK && errno != EAGAIN) {
                client.mark_for_delete = true;
            }
        }

        // Graceful Shutdown Logic
        if (client.close_after_write && client.write_buffer.empty()) {
            if (!client.shutdown_sent) {
                shutdown(client.fd, SHUT_WR);
                client.shutdown_sent = true;
            }
        }

        if (client.mark_for_delete)
            continue;
    }

    std::erase_if(clients_, [this](const HttpClient& c) {
        if (c.mark_for_delete) {
            CLOSE_SOCKET(c.fd);
            on_delete_fd(c.fd);
            return true;
        }
        return false;
    });

    servicable_fds_.clear();
}

void HttpServer::queue_data(HttpClient& client, std::string_view data, bool close_connection) {
    ZoneScoped;

    client.write_buffer.append(data);
    client.close_after_write = close_connection;
}

void HttpServer::handle_client_data(HttpClient& client) {
    ZoneScoped;

    auto result = luxon::parse_raw_http(client.request_buffer);
    if (!result.has_value()) {
        send_error(client, 400, "Bad Request");
        return;
    }

    const luxon::HttpRequest req = result.value();
    try {
        if (req.method == "POST" && req.path == "/batch") {
            try {
                json batch_req = json::parse(req.body);
                json batch_res = execute_batch(batch_req);
                send_json_response(client, 200, batch_res);
            } catch (const json::exception& e) {
                send_error(client, 400, "Invalid JSON payload");
            }
            return;
        }

        std::string_view content, content_type = "text/html";
        if (req.method == "GET") {
#ifdef LUXON_ENET_ENABLE_METRICS
            if (req.path == "/metrics") {
                send_text_response(client, 200, generate_prometheus_metrics(), "text/plain; version=0.0.4");
                return;
            }

#endif
            if (req.path == "/") {
                content = GET_RESOURCE(index_html);
            } else if (req.path == "/stats") {
                content = GET_RESOURCE(stats_html);
            } else if (req.path == "/style.css") {
                content = GET_RESOURCE(style_css);
                content_type = "text/css";
            }
        }

        if (content.empty()) {
            send_json_response(client, 200, route_request(req.method, req.path));
        } else {
            const std::string response = std::format("HTTP/1.1 200 OK\r\n"
                                                     "Content-Type: {}\r\n"
                                                     "Content-Length: {}\r\n"
                                                     "Connection: close\r\n"
                                                     "Server: LuxonServer (" __DATE__ " " __TIME__ ")\r\n"
                                                     "\r\n{}",
                                                     content_type, content.size(), content);
            queue_data(client, response, true);
        }
    } catch (const std::out_of_range& e) {
        send_error(client, 404, "Not Found");
    } catch (const std::exception& e) {
        send_error(client, 500, std::string("Internal Error: ") + e.what());
    }
}

void HttpServer::send_text_response(HttpClient& client, int status, std::string_view body, std::string_view content_type) {
    const std::string response = std::format("HTTP/1.1 {} OK\r\n"
                                             "Content-Type: {}\r\n"
                                             "Content-Length: {}\r\n"
                                             "Connection: close\r\n"
                                             "Server: LuxonServer (" __DATE__ " " __TIME__ ")\r\n"
                                             "\r\n{}",
                                             status, content_type, body.size(), body);
    queue_data(client, response, true);
}

void HttpServer::send_json_response(HttpClient& client, int status, const json& body) { send_text_response(client, status, body.dump(), "application/json"); }

void HttpServer::send_error(HttpClient& client, int status, std::string_view message) {
    const json err_body = {{"error", message}};
    const std::string body_str = err_body.dump();
    const std::string response = std::format("HTTP/1.1 {} Error\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: {}\r\n"
                                             "Connection: close\r\n"
                                             "Server: LuxonServer (" __DATE__ " " __TIME__ ")\r\n"
                                             "\r\n{}",
                                             status, body_str.size(), body_str);
    queue_data(client, response, true);
}

// Helpers

std::shared_ptr<App> HttpServer::find_app_by_id(std::string_view app_id) {
    ZoneScoped;

    const auto apps = App::get_all(server_manager_);
    for (const auto& app : apps)
        if (app->id == app_id)
            return app;
    return nullptr;
}

Lobby *HttpServer::find_lobby(App *app, std::string_view name, uint8_t type) {
    ZoneScoped;

    const auto lobbies = app->get_lobbies();
    if (auto res = lobbies.find({name, type}); res != lobbies.end())
        return res->second.lock().get();
    return nullptr;
}

Lobby *HttpServer::find_lobby(App *app, std::string_view name) {
    ZoneScoped;

    const uint8_t type = name.back() - '0';
    name = name.substr(0, name.size() - 1);
    return find_lobby(app, name, type);
}

// Routing

#ifdef LUXON_ENET_ENABLE_METRICS
std::string HttpServer::generate_prometheus_metrics() {
    ZoneScoped;
    std::string out;
    out.reserve(8192);

    auto append_gauge = [&](const std::string& name, auto value) { out += std::format("# TYPE {} gauge\n{} {}\n", name, name, value); };

    auto append_counter = [&](const std::string& name, auto value) { out += std::format("# TYPE {} counter\n{} {}\n", name, name, value); };

    auto append_rate = [&](const std::string& prefix, const luxon::enet::RateCounter& rc) { append_counter(prefix + "_total", rc.total); };

    auto append_flow = [&](const std::string& prefix, const luxon::enet::FlowCounter& fc) {
        append_counter(prefix + "_added_total", fc.total_added);
        append_counter(prefix + "_removed_total", fc.total_removed);
        append_gauge(prefix + "_current", fc.current());
    };

    const auto& m = server_manager_.get_enet_metrics();

    // Global
    append_rate("luxon_global_bytes_in", m.global.bytes_in);
    append_rate("luxon_global_bytes_out", m.global.bytes_out);
    append_rate("luxon_global_messages_in", m.global.messages_in);
    append_rate("luxon_global_messages_out", m.global.messages_out);
    append_gauge("luxon_global_connections_active", m.global.connections_active.current);
    append_flow("luxon_global_peers", m.global.peers);
    append_rate("luxon_global_disconnected_peers", m.global.disconnected_peers);
    append_rate("luxon_global_disconnected_peers_client", m.global.disconnected_peers_c);
    append_rate("luxon_global_disconnected_peers_server", m.global.disconnected_peers_s);
    append_rate("luxon_global_disconnected_peers_timeout", m.global.disconnected_peers_t);

    // UDP
    append_rate("luxon_udp_datagrams_in", m.udp.datagrams_in);
    append_rate("luxon_udp_datagrams_out", m.udp.datagrams_out);

    // ENet
    append_rate("luxon_enet_datagram_validation_failures", m.enet.datagram_validation_failures);
    append_rate("luxon_enet_commands_in", m.enet.commands_in);
    append_rate("luxon_enet_commands_out", m.enet.commands_out);
    append_rate("luxon_enet_commands_out_throttled", m.enet.commands_out_throttled);
    append_rate("luxon_enet_reliable_commands_in", m.enet.reliable_commands_in);
    append_rate("luxon_enet_reliable_commands_out", m.enet.reliable_commands_out);
    append_rate("luxon_enet_reliable_commands_in_dropped", m.enet.reliable_commands_in_dropped);
    append_rate("luxon_enet_reliable_commands_out_resent", m.enet.reliable_commands_out_resent);
    append_rate("luxon_enet_unreliable_commands_in", m.enet.unreliable_commands_in);
    append_rate("luxon_enet_unreliable_commands_out", m.enet.unreliable_commands_out);
    append_rate("luxon_enet_unreliable_commands_in_dropped", m.enet.unreliable_commands_in_dropped);
    append_rate("luxon_enet_acknowledgements_in", m.enet.acknowledgements_in);
    append_rate("luxon_enet_acknowledgements_out", m.enet.acknowledgements_out);
    append_rate("luxon_enet_pings_in", m.enet.pings_in);
    append_rate("luxon_enet_pings_out", m.enet.pings_out);

    // Server Performance Stats
    auto append_server_metric = [&](const std::string& name, auto& metric_data, unsigned duration_ms) {
        try {
            append_gauge(name + "_min", metric_data.min(duration_ms));
            append_gauge(name + "_avg", metric_data.avg(duration_ms));
            append_gauge(name + "_max", metric_data.max(duration_ms));
        } catch (...) {
            // Silently skip if metric isn't populated
        }
    };

    // 1-minute (60,000ms) rolling statistics
    append_server_metric("luxon_server_busy_time_1m", server_manager_.busy_time, 60000);
    append_server_metric("luxon_server_idle_time_1m", server_manager_.idle_time, 60000);

    return out;
}

#endif

json HttpServer::execute_batch(const json& batch_req) {
    ZoneScoped;

    json results = json::array();
    json responses = json::array();

    if (!batch_req.contains("steps") || !batch_req["steps"].is_array()) {
        throw std::invalid_argument("Batch request must contain a 'steps' array");
    }

    const auto& steps = batch_req["steps"];
    if (steps.size() > 25) {
        throw std::invalid_argument("Batch limit exceeded (max 25 steps)");
    }

    for (size_t i = 0; i < steps.size(); ++i) {
        const auto& step = steps[i];
        std::string method = step.value("method", "GET");
        std::string raw_path = step.at("path").get<std::string>();

        try {
            // Extract all template strings
            std::vector<std::string> templates;
            size_t start_pos = 0;
            while ((start_pos = raw_path.find("{$steps", start_pos)) != std::string::npos) {
                size_t end_pos = raw_path.find('}', start_pos);
                if (end_pos == std::string::npos)
                    break;
                templates.push_back(raw_path.substr(start_pos, end_pos - start_pos + 1));
                start_pos = end_pos + 1;
            }

            // Expand paths for wildcards
            std::vector<std::string> paths_to_execute = {raw_path};

            for (const auto& tmpl : templates) {
                std::string ptr_str = tmpl.substr(7, tmpl.length() - 8);
                size_t wildcard_pos = ptr_str.find("/*");

                std::vector<std::string> new_paths;

                if (wildcard_pos != std::string::npos) {
                    // Split pointer: Base ("/0") and Sub ("/id")
                    std::string base_ptr = ptr_str.substr(0, wildcard_pos);
                    std::string sub_ptr = ptr_str.substr(wildcard_pos + 2);

                    const json& base_arr = results.at(json::json_pointer(base_ptr));
                    if (!base_arr.is_array())
                        throw std::invalid_argument("Wildcard base must be an array");

                    // Generate new path for every item in array
                    for (const auto& item : base_arr) {
                        const json& val = sub_ptr.empty() ? item : item.at(json::json_pointer(sub_ptr));
                        std::string resolved_val = val.is_string() ? val.get<std::string>() : val.dump();

                        for (const auto& p : paths_to_execute) {
                            std::string next_path = p;
                            size_t pos = next_path.find(tmpl);
                            if (pos != std::string::npos)
                                next_path.replace(pos, tmpl.length(), resolved_val);
                            new_paths.push_back(next_path);
                        }
                    }
                } else {
                    // Standard replacement
                    const json& val = results.at(json::json_pointer(ptr_str));
                    std::string resolved_val = val.is_string() ? val.get<std::string>() : val.dump();

                    for (const auto& p : paths_to_execute) {
                        std::string next_path = p;
                        size_t pos = next_path.find(tmpl);
                        if (pos != std::string::npos)
                            next_path.replace(pos, tmpl.length(), resolved_val);
                        new_paths.push_back(next_path);
                    }
                }

                // Prevent nested wildcards from causing a cartesian explosion
                if (new_paths.size() > 50)
                    throw std::runtime_error("Batch fan-out limit exceeded (max 50)");
                paths_to_execute = std::move(new_paths);
            }

            // Execute all generated paths
            json step_result;
            if (paths_to_execute.size() == 1) {
                step_result = route_request(method, paths_to_execute[0]);
            } else {
                step_result = json::array();
                for (const auto& p : paths_to_execute) {
                    json res = route_request(method, p);

                    // Flatten arrays so downstream steps remain 1D
                    if (res.is_array()) {
                        for (const auto& item : res)
                            step_result.push_back(item);
                    } else {
                        step_result.push_back(res);
                    }
                }
            }

            results.push_back(step_result);
            responses.push_back({{"step", i}, {"status", 200}, {"body", step_result}});

        } catch (const std::exception& e) {
            responses.push_back({{"step", i}, {"status", 500}, {"error", e.what()}});
            break; // Cancel on failure
        }
    }

    return responses;
}

json HttpServer::route_request(std::string_view method, std::string path) {
    ZoneScoped;

    url_decode_in_place(path);

    auto segs = common::utils::str_split(path, '/');
    if (segs.empty())
        throw std::out_of_range("Path empty");
    if (segs.front().empty())
        segs.erase(segs.begin());

    // Redirect old legacy paths
    //  - /apps/{app}/lobbies/{name}/games/{game_id}/... -> /apps/{app}/games/{game_id}/...
    if (segs.size() >= 6 && segs[0] == "apps" && segs[2] == "lobbies" && segs[4] == "games")
        segs.erase(segs.begin() + 2, segs.begin() + 4);

    // /stats/{metric}/{type}/{duration_ms}
    if (segs.size() >= 1 && segs[0] == "stats") {
        if (segs.size() != 4) {
            throw std::out_of_range("Invalid stats request format. Expected: /stats/{metric}/{type}/{duration_ms}");
        }

        if (segs.size() >= 2) {
            std::string_view metric = segs[1];
            std::string_view type = segs[2];
            unsigned duration_ms = 0;
            auto [ptr, ec] = std::from_chars(segs[3].data(), segs[3].data() + segs[3].size(), duration_ms);

            if (ec != std::errc())
                throw std::invalid_argument("Invalid duration parameter");

            Metric *metric_data{};
            if (metric == "busy_time")
                metric_data = &server_manager_.busy_time;
            else if (metric == "idle_time")
                metric_data = &server_manager_.idle_time;
            else
                throw std::invalid_argument("Unknown metric. Expected: busy_time, wait_time");

            if (type == "min")
                return metric_data->min(duration_ms);
            else if (type == "avg")
                return metric_data->avg(duration_ms);
            else if (type == "max")
                return metric_data->max(duration_ms);
            else
                throw std::out_of_range("Unknown metric type. Available: min, avg, max");
        }
    }

    // /connections
    if (segs.size() == 1 && segs[0] == "connections") {
        json res = json::array();
        for (const auto& conn : server_manager_.get_connections()) {
            const auto p = conn->get_peer();
            json item = {{"address", p->enet_peer->remote_endpoint().to_string()},
                         {"peer_id", p->enet_peer->peer_id()},
                         {"state", static_cast<int32_t>(p->enet_peer->state())},
                         {"stats",
                          {{"rtt", p->enet_peer->round_trip_time()},
                           {"rtt_var", p->enet_peer->round_trip_variance()},
                           {"bytes_in", p->enet_peer->bytes_in()},
                           {"bytes_out", p->enet_peer->bytes_out()},
                           {"packet_loss_crc", p->enet_peer->packet_loss_by_crc()},
                           {"packet_loss_challenge", p->enet_peer->packet_loss_by_challenge()}}}};
            if (p->is_authenticated())
                item["auth"] = {{"user_id", p->persistent->user_id}, {"app_id", p->persistent->app->id}, {"app_ver", p->persistent->app->version}};
            res.push_back(item);
        }
        return res;
    }

    // /apps
    if (segs.size() == 1 && segs[0] == "apps") {
        json res = json::array();
        for (const auto& app : App::get_all(server_manager_))
            res.push_back({{"id", app->id}, {"version", app->version}, {"lobbies_count", app->get_lobbies().size()}});
        return res;
    }

    // /apps/{app} roots
    if (segs.size() >= 2 && segs[0] == "apps") {
        std::string_view appId = segs[1];
        const auto app = find_app_by_id(appId);
        if (!app)
            throw std::out_of_range("App not found");

        // /apps/{app} (Info)
        if (segs.size() == 2)
            return {{"id", app->id}, {"version", app->version}, {"lobbies_count", app->get_lobbies().size()}};

        // /apps/{app}/connections
        if (segs.size() == 3 && segs[2] == "connections") {
            json res = json::array();
            for (const auto& conn : server_manager_.get_connections()) {
                const auto p = conn->get_peer();
                if (p->is_authenticated() && p->persistent->app->id == appId) {
                    res.push_back({{"peer_id", p->enet_peer->peer_id()},
                                   {"address", p->enet_peer->remote_endpoint().to_string()},
                                   {"user_id", p->persistent->user_id},
                                   {"token", p->persistent->token},
                                   {"stats",
                                    {{"rtt", p->enet_peer->round_trip_time()},
                                     {"bytes_out", p->enet_peer->bytes_out()},
                                     {"packet_loss", p->enet_peer->packet_loss_by_challenge()}}}});
                }
            }
            return res;
        }

        // Game roots: /apps/{app}/games/{game_id}/...
        if (segs.size() >= 4 && segs[2] == "games") {
            std::string_view gameId = segs[3];
            const auto& app_games = app->get_games();
            auto it = app_games.find(gameId);
            if (it == app_games.end())
                throw std::out_of_range("Game ID not found");
            auto game = it->second.lock();
            if (!game)
                throw std::out_of_range("Game expired");

            // /apps/{app}/games/{game_id}
            if (segs.size() == 4) {
                const auto footprint =
                    game->measure_footprint(FootprintLimits{.max_bytes = 1000000, .max_complexity = 50000});
                return {{"id", game->id},
                        {"master_client_id", game->master_actor},
                        {"player_ttl", game->player_ttl},
                        {"empty_room_ttl", game->empty_game_ttl},
                        {"flags", game->flags},
                        {"expected_users", game->expected_users},
                        {"full_props", json_conv::photon_hash_to_json(game->get_game_props())},
                        {"footprint",
                         {{"bytes", footprint.bytes},
                          {"complexity", footprint.complexity},
                          {"measurement_limit_exceeded", footprint.limit_exceeded}}}};
            }

            // /apps/{app}/games/{game_id}/actors
            if (segs.size() == 5 && segs[4] == "actors") {
                json res = json::array();
                for (auto& gp : game->peers) {
                    std::string uid = "";
                    // If persistent peer is still attached (could be disconnected/stale)
                    if (auto p = gp.peer.lock())
                        if (p->is_authenticated())
                            uid = p->persistent->user_id;

                    // Check actor props for UserId if persistent isn't available or just to be sure
                    if (uid.empty() && gp.actor_props.contains(static_cast<int32_t>(253) /* UserId */)) {
                        auto& val = gp.actor_props.at(static_cast<int32_t>(253));
                        if (val.is<std::string>())
                            uid = val.get<std::string>();
                    }

                    res.push_back({{"actor_id", gp.actor_id}, {"user_id", uid}});
                }
                return res;
            }

            // /apps/{app}/games/{game_id}/actors/{actor_id}
            if (segs.size() == 6 && segs[4] == "actors") {
                int actorId = 0;
                std::from_chars(segs[5].data(), segs[5].data() + segs[5].size(), actorId);

                const auto *gp = game->find_peer(actorId);
                if (!gp)
                    throw std::out_of_range("Actor ID not found");

                json res = {{"actor_id", gp->actor_id},
                            {"props", json_conv::photon_hash_to_json(gp->actor_props)},
                            {"interest_groups", gp->interest_groups.to_string()}};

                if (auto p = gp->peer.lock())
                    if (p->is_authenticated())
                        res["connection_info"] = {
                            {"peer_id", p->enet_peer->peer_id()}, {"rtt", p->enet_peer->round_trip_time()}, {"user_id", p->persistent->user_id}};
                return res;
            }
        }

        // /apps/{app}/lobbies
        if (segs.size() == 3 && segs[2] == "lobbies") {
            json res = json::array();
            for (const auto& [name, weak_lobby] : app->get_lobbies()) {
                if (auto lobby = weak_lobby.lock())
                    res.push_back({{"name", lobby->name + char(lobby->type + '0')},
                                   {"type", lobby->type},
                                   {"games_count", lobby->games.size()},
                                   {"peer_count", lobby->get_peer_count()}});
            }
            return res;
        }

        // Lobby roots: /apps/{app}/lobbies/{name}/...
        if (segs.size() >= 4 && segs[2] == "lobbies") {
            const Lobby *lobby = find_lobby(app.get(), segs[3]);
            if (!lobby)
                throw std::out_of_range("Lobby name invalid");

            // /apps/{app}/lobbies/{name}/games
            if (segs.size() == 5 && segs[4] == "games") {
                json res = json::array();
                for (auto& weak_g : lobby->games) {
                    if (auto g = weak_g.lock()) {
                        res.push_back({{"id", g->id},
                                       {"player_count", g->peers.size()},
                                       {"max_players", g->max_peers},
                                       {"is_open", g->is_open && !g->is_force_closed},
                                       {"is_visible", g->is_visible},
                                       {"lobby_props", json_conv::photon_hash_to_json(g->get_lobby_game_props())}});
                    }
                }
                return res;
            }
        }
    }

    throw std::out_of_range("Endpoint not found");
}
} // namespace server
