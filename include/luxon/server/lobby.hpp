// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <string>
#include <memory>
#include <list>
#include <functional>
#include <unordered_map>
#include <cstdint>

namespace server {
class App;
struct Lobby;
struct Game;

struct GameListUpdateHandler {
    std::function<void(const std::shared_ptr<Game>&)> game_create;
    std::function<void(const std::shared_ptr<Game>&)> game_change;
    std::function<void(Game *)> game_delete;
};

struct Lobby {
    App& app;
    const std::string name;
    const uint8_t type = 0;

    std::unordered_map<std::string_view, std::weak_ptr<Game>> games;
    std::list<GameListUpdateHandler> game_list_update_handlers;

    std::shared_ptr<Game> create_game(std::string id, bool or_get = false);

    size_t get_peer_count() const;
    size_t get_master_peer_count() const;
};
} // namespace server
