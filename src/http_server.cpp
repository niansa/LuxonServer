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
#include <commoncpp/utils.hpp>

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
    INCBIN(index_html, SOURCE_DIR "/index.html");
    INCBIN(stats_html, SOURCE_DIR "/stats.html");
    INCBIN(style_css, SOURCE_DIR "/style.css");
}
#define GET_RESOURCE(name) {incbin_ ## name ## _start, static_cast<size_t>(reinterpret_cast<intptr_t>(incbin_ ## name ## _end - incbin_ ## name ## _start))} 
#endif

using json = nlohmann::json;
using namespace luxon::ser;

namespace server {
namespace {
void set_nonblocking(int fd) {
#if defined(_WIN32)
    u_long mode = 1;
    ioctlsocket(fd, FIONBIO, &mode);
#else
    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
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
    json j = json::object();
    for (const auto& [k, v] : dict)
        j[std::to_string(k)] = photon_val_to_json(v);
    return j;
}

json photon_hash_to_json(const Hashtable& hash) {
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
    return std::visit(
        [](auto&& arg) -> json {
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
        },
        val.value);
}
} // namespace json_conv
} // namespace

HttpServer::HttpServer(ServerManager& manager) : server_manager_(manager) {
    log_ = create_logger("HTTPServer");
#ifndef NDEBUG
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
#ifndef LUXON_SERVER_POLL
        on_delete_fd(server_fd_);
#endif
    }
    for (auto& c : clients_) {
        CLOSE_SOCKET(c.fd);
#ifndef LUXON_SERVER_POLL
        on_delete_fd(c.fd);
#endif
    }
#if defined(_WIN32)
    WSACleanup();
#endif
}

bool HttpServer::bind(const std::string& address, uint16_t port) {
    server_fd_ = socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd_ < 0)
        return false;
#ifndef LUXON_SERVER_POLL
    on_create_fd(server_fd_);
#endif
    const int opt = 1;
    setsockopt(server_fd_, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char *>(&opt), sizeof(opt));
    set_nonblocking(server_fd_);
    sockaddr_in addr{};
    addr.sin_family = AF_INET;
    addr.sin_port = htons(port);
#ifdef LUXON_ENET_HAS_PTON
    if (inet_pton(AF_INET, address.c_str(), &addr.sin_addr) <= 0) {
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
    log_->info("Listening on http://{}:{}", address, port);
    return true;
}

#ifndef LUXON_SERVER_POLL
void HttpServer::service(const std::vector<socket_t>& fds) {
#else
void HttpServer::service() {
#endif
    if (server_fd_ == -1)
        return;

#ifndef LUXON_SERVER_POLL
    if (std::ranges::contains(fds, server_fd_))
#endif
    {
        sockaddr_in cli_addr;
        socklen_t clilen = sizeof(cli_addr);
        socket_t new_fd = accept(server_fd_, reinterpret_cast<struct sockaddr *>(&cli_addr), &clilen);
        if (new_fd >= 0) {
            set_nonblocking(new_fd);
            clients_.push_back({new_fd, "", "", false, false});
#ifndef LUXON_SERVER_POLL
            on_create_fd(new_fd);
#endif
        }
    }

    for (auto& client : clients_) {
        if (client.mark_for_delete)
            continue;

        // Handle Outgoing Data
        if (!client.write_buffer.empty()) {
            int sent = send(client.fd, client.write_buffer.data(), client.write_buffer.size(), 0);
            if (sent > 0) {
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

        // Handle Incoming Data
#ifndef LUXON_SERVER_POLL
        if (std::ranges::contains(fds, client.fd))
#endif
        {
            char buffer[4096];
            const int n = recv(client.fd, buffer, sizeof(buffer), 0);

            if (n > 0) {
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
    }

    std::erase_if(clients_, [this](const HttpClient& c) {
        if (c.mark_for_delete) {
            CLOSE_SOCKET(c.fd);
#ifndef LUXON_SERVER_POLL            
            on_delete_fd(c.fd);
#endif
            return true;
        }
        return false;
    });
}

void HttpServer::queue_data(HttpClient& client, std::string_view data, bool close_connection) {
    client.write_buffer.append(data);
    client.close_after_write = close_connection;
}

void HttpServer::handle_client_data(HttpClient& client) {
    auto result = luxon::parse_raw_http(client.request_buffer);
    if (!result.has_value()) {
        send_error(client, 400, "Bad Request");
        return;
    }

    const luxon::HttpRequest req = result.value();
    try {
        std::string_view content, content_type = "text/html";
        if (req.method == "GET") {
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
            send_response(client, 200, route_request(req.method, req.path));
        } else {
            const std::string response = std::format("HTTP/1.1 200 OK\r\n"
                                                     "Content-Type: {}\r\n"
                                                     "Content-Length: {}\r\n"
                                                     "Connection: close\r\n"
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

void HttpServer::send_response(HttpClient& client, int status, const json& body) {
    const std::string body_str = body.dump(2);
    const std::string response = std::format("HTTP/1.1 {} OK\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: {}\r\n"
                                             "Connection: close\r\n"
                                             "\r\n{}",
                                             status, body_str.size(), body_str);
    queue_data(client, response, true);
}

void HttpServer::send_error(HttpClient& client, int status, std::string_view message) {
    const json err_body = {{"error", message}};
    const std::string body_str = err_body.dump();
    const std::string response = std::format("HTTP/1.1 {} Error\r\n"
                                             "Content-Type: application/json\r\n"
                                             "Content-Length: {}\r\n"
                                             "Connection: close\r\n"
                                             "\r\n{}",
                                             status, body_str.size(), body_str);
    queue_data(client, response, true);
}

// Helpers

std::shared_ptr<App> HttpServer::find_app_by_id(std::string_view app_id) {
    const auto apps = App::get_all(server_manager_);
    for (const auto& app : apps)
        if (app->id == app_id)
            return app;
    return nullptr;
}

Lobby *HttpServer::find_lobby_by_index(App *app, std::string_view index_str) {
    const auto lobbies = app->get_lobbies();
    size_t idx = 0;
    const auto [ptr, ec] = std::from_chars(index_str.data(), index_str.data() + index_str.size(), idx);
    if (ec == std::errc() && idx < lobbies.size())
        return lobbies[idx];
    return nullptr;
}

// Routing

json HttpServer::route_request(std::string_view method, std::string path) {
    url_decode_in_place(path);

    auto segs = common::utils::str_split(path, '/');
    if (segs.empty())
        throw std::out_of_range("Path empty");
    if (segs.front().empty())
        segs.erase(segs.begin());

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
            json item = {{"address", p->enet_peer->remote_endpoint()->to_string()},
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
                                   {"address", p->enet_peer->remote_endpoint()->to_string()},
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

        // /apps/{app}/lobbies
        if (segs.size() == 3 && segs[2] == "lobbies") {
            json res = json::array();
            int idx = 0;
            for (auto *lobby : app->get_lobbies()) {
                res.push_back({{"index", idx++},
                               {"name", lobby->name},
                               {"type", lobby->type},
                               {"games_count", lobby->games.size()},
                               {"peer_count", lobby->get_peer_count()}});
            }
            return res;
        }

        // Lobby roots: /apps/{app}/lobbies/{index}/...
        if (segs.size() >= 4 && segs[2] == "lobbies") {
            const Lobby *lobby = find_lobby_by_index(app.get(), segs[3]);
            if (!lobby)
                throw std::out_of_range("Lobby index invalid");

            // /apps/{app}/lobbies/{index}/games
            if (segs.size() == 5 && segs[4] == "games") {
                json res = json::array();
                for (auto& [gid, weak_g] : lobby->games) {
                    if (auto g = weak_g.lock()) {
                        res.push_back({{"id", g->id},
                                       {"player_count", g->peers.size()},
                                       {"max_players", g->max_peers},
                                       {"is_open", g->is_open},
                                       {"is_visible", g->is_visible},
                                       {"basic_props", json_conv::photon_hash_to_json(g->get_basic_game_props())}});
                    }
                }
                return res;
            }

            // Game roots: /apps/{app}/lobbies/{index}/games/{game_id}/...
            if (segs.size() >= 6 && segs[4] == "games") {
                std::string_view gameId = segs[5];
                auto it = lobby->games.find(gameId);
                if (it == lobby->games.end())
                    throw std::out_of_range("Game ID not found");
                auto game = it->second.lock();
                if (!game)
                    throw std::out_of_range("Game expired");

                // /apps/{app}/lobbies/{index}/games/{game_id}
                if (segs.size() == 6)
                    return {{"id", game->id},
                            {"master_client_id", game->master_actor},
                            {"player_ttl", game->player_ttl},
                            {"empty_room_ttl", game->empty_game_ttl},
                            {"flags", game->flags},
                            {"expected_users", game->expected_users}, // automatic json conversion for set<string>
                            {"full_props", json_conv::photon_hash_to_json(game->get_game_props())}};

                // /apps/{app}/lobbies/{index}/games/{game_id}/actors
                if (segs.size() == 7 && segs[6] == "actors") {
                    json res = json::array();
                    for (auto& gp : game->peers) {
                        std::string uid = "";
                        // If persistent peer is still attached (could be disconnected/stale)
                        if (auto p = gp.peer.lock())
                            if (p->is_authenticated())
                                uid = p->persistent->user_id;

                        // Check actor props for UserId if persistent isn't available or just to be sure
                        if (uid.empty() && gp.actor_props.contains(static_cast<int32_t>(253) /* UserId */)) {
                            // Assuming UserId is stored as string in props
                            auto& val = gp.actor_props.at(static_cast<int32_t>(253));
                            if (val.is<std::string>())
                                uid = val.get<std::string>();
                        }

                        res.push_back({{"actor_id", gp.actor_id}, {"user_id", uid}});
                    }
                    return res;
                }

                // /apps/{app}/lobbies/{index}/games/{game_id}/actors/{actor_id}
                if (segs.size() == 8 && segs[6] == "actors") {
                    int actorId = 0;
                    std::from_chars(segs[7].data(), segs[7].data() + segs[7].size(), actorId);

                    const auto *gp = game->find_peer(actorId);
                    if (!gp)
                        throw std::out_of_range("Actor ID not found");

                    json res = {
                        {"actor_id", gp->actor_id},
                        {"props", json_conv::photon_hash_to_json(gp->actor_props)},
                        {"interest_groups", gp->interest_groups} // set<uint8> auto conv
                    };

                    if (auto p = gp->peer.lock())
                        if (p->is_authenticated())
                            res["connection_info"] = {
                                {"peer_id", p->enet_peer->peer_id()}, {"rtt", p->enet_peer->round_trip_time()}, {"user_id", p->persistent->user_id}};
                    return res;
                }
            }
        }
    }

    throw std::out_of_range("Endpoint not found");
}
} // namespace server
