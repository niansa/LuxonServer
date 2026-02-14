// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "global.hpp"
#include "apps.hpp"
#include "lobby.hpp"
#include "peer.hpp"
#ifdef LUXON_SERVER_ENABLE_PLUGINS
#include "game_plugin_base.hpp"
#endif

#include <memory>
#include <unordered_set>
#include <vector>
#include <list>
#include <variant>
#include <luxon/ser_types.hpp>
#include <luxon/enet_peer.hpp>

#ifdef LUXON_SERVER_ENABLE_PLUGINS
#define GAME_PLUGINS_INVOKE(...)                                                                                                                               \
    {                                                                                                                                                          \
        using namespace game_plugins;                                                                                                                          \
        __VA_ARGS__                                                                                                                                            \
    }
#else
#define GAME_PLUGINS_INVOKE(...)
#endif

namespace server {
class App;
struct Lobby;
struct Peer;

struct GamePeer {
    std::weak_ptr<Peer> peer;
    int32_t actor_id{};
    ser::Hashtable actor_props;
    std::unordered_set<uint8_t> interest_groups;

    bool is_valid() const { return actor_id > 0; }
    bool has_interest_group(uint8_t group) const;
    bool disconnect();
};

struct Event {
    uint8_t code;
    int32_t sender_actor_id;
    enet::EnetDeliveryMode delivery_mode = enet::EnetDeliveryMode::Reliable;
    uint8_t channel{};
    std::variant<std::monostate, uint8_t, std::unordered_set<int32_t>> receivers{};
    uint8_t interest_group{};
    ser::Value data;
    ser::Dictionary top_params;

    ser::Hashtable& make_params_hashtable() { return *(data = std::make_shared<ser::Hashtable>()).get<ser::HashtablePtr>(); }
    ser::Hashtable *get_params_hashtable() {
        if (data.is<ser::HashtablePtr>())
            return data.get<ser::HashtablePtr>().get();
        return nullptr;
    }
};

struct Game : std::enable_shared_from_this<Game> {
    const std::shared_ptr<Lobby> lobby;
    const std::string id;

    ~Game();

#ifdef LUXON_SERVER_ENABLE_PLUGINS
    std::vector<std::unique_ptr<game_plugins::PluginBase>> plugins;
#endif

    uint8_t flags = 3; // CheckUserOnJoin | DeleteCacheOnLeave
    bool is_open = true;
    bool is_visible = true;
    int32_t player_ttl = 0;
    int32_t empty_game_ttl = 0;
    uint8_t max_peers = 0;
    int32_t master_actor = 1;
    int32_t last_actor_id = 0;
    std::unordered_set<std::string> expected_users;
    ser::Hashtable custom_props;
    std::vector<std::string> lobby_props;
    std::list<Event> event_cache;

    Game(std::shared_ptr<Lobby> lobby, std::string id) : lobby(std::move(lobby)), id(std::move(id)) {}

    std::list<GamePeer> peers;

    ///
    /// \brief Creates a GamePeer that can later be added to the game
    /// \param peer The peer that's going to be behind the GamePeer
    /// \return Complete GamePeer
    /// \note Returned GamePeer must not be added to any other game
    /// \note Returned GamePeer has actor_id set, but actor_id won't be fully reserved. It might be taken by another GamePeer after a long time.
    ///
    GamePeer create_peer(std::shared_ptr<Peer> peer);
    ///
    /// \brief Adds given GamePeer to game
    /// \param game_peer GamePeer to add to game, must've been previously created by the same Game using create_peer()
    /// \return Pointer to added GamePeer if successful, otherwise nullptr
    /// \note Fails if actor_id is already taken or CheckUserOnJoin flag is set and user id is already taken
    ///
    GamePeer *add_peer(GamePeer&& game_peer);
    ///
    /// \brief Removes peer's GamePeer from game
    /// \param peer Peer whos GamePeer to remove
    /// \return True if a GamePeer was removed, otherwise false
    ///
    bool remove_peer(const std::shared_ptr<Peer>& peer);
    ///
    /// \brief Floods given peer with cached events
    /// \param game_peer GamePeer to flood
    /// \return True if flooding was successful, otherwise false
    ///
    bool flood_peer(GamePeer *game_peer);
    ///
    /// \brief Finds peer with given actor_id
    /// \param actor_id Actor_id to look for
    /// \return Pointer to GamePeer with given actor_id if successful, otherwise nullptr
    ///
    GamePeer *find_peer(int32_t actor_id);
    ///
    /// \brief Broadcasts an event to the lobby
    /// \param event Event to broadcast
    ///
    void broadcast_event(Event& event);
    ///
    /// \brief Checks if user + given amount of expected users can join
    /// \param user_id User ID of primary user trying to join
    /// \param new_expected_users_count Amount of users to calculate in as well
    /// \return ErrorCode value
    ///
    int16_t validate_join(const std::string& user_id, size_t new_expected_users_count = 0) const;

    ///
    /// \brief Updates the game in the lobby's game list
    ///
    void trigger_lobby_update();

    ///
    /// \brief Gets well-known or custom game property
    /// \param key Property to get
    /// \return Value of property, null-value if not found
    ///
    ser::Value get_game_prop(const ser::Value& key);
    ///
    /// \brief Gets all well-known game properties that are to be shown in lobby
    /// \return Hashtable with well-known keys/value property pairs
    ///
    ser::Hashtable get_lobby_game_props();
    ///
    /// \brief Gets all game properties
    /// \param no_custom Excludes custom properties
    /// \return Hashtable with keys/value property pairs
    ///
    ser::Hashtable get_game_props(bool no_custom = false);
    ///
    /// \brief Gets all properties from all actors
    /// \return Hashtable with actor->properties pairs containing key/value property pairs
    ///
    ser::Hashtable get_actor_props();
    ///
    /// \brief Merges a hashtable with given properties into game properties
    /// \param update Hashtable with keys/value property pairs
    ///
    void insert_game_props(ser::Hashtable update);
    ///
    /// \brief Checks if the expectation of given properties is met
    /// \param expected Hashtable with keys/value property pairs
    /// \return True if expectation is met, otherwise false
    ///
    bool expect_game_props(ser::Hashtable expected);
    ///
    /// \brief Merges a hashtable with given properties into given actors properties
    /// \param actor_id actor_id of actor whos properties to access
    /// \param update Hashtable with keys/value property pairs
    /// \return True if given actor was found and its properties updated
    ///
    bool insert_actor_props(int32_t actor_id, const ser::Hashtable& update);
    ///
    /// \brief Checks if the expectation of given properties is met
    /// \param actor_id actor_id of actor whos properties to access
    /// \param expected Hashtable with keys/value property pairs
    /// \return True if expectation is met, otherwise false
    ///
    bool expect_actor_props(int32_t actor_id, const ser::Hashtable& expected);

#ifdef LUXON_SERVER_ENABLE_PLUGINS
    template <typename InfoStruct>
    game_plugins::Result execute_plugin_chain(game_plugins::Result (game_plugins::PluginBase::*method)(const luxon::ser::OperationRequestMessage&, InfoStruct&),
                                              const luxon::ser::OperationRequestMessage& req, InfoStruct& info) {
        for (const auto& plugin : plugins) {
            game_plugins::Result result = ((*plugin).*method)(req, info);
            if (result != game_plugins::Result::Continue)
                return result;
        }

        return game_plugins::Result::Continue;
    }

    template <typename InfoStruct>
    game_plugins::Result execute_plugin_chain(game_plugins::Result (game_plugins::PluginBase::*method)(InfoStruct&), InfoStruct& info) {
        for (const auto& plugin : plugins) {
            game_plugins::Result result = ((*plugin).*method)(info);
            if (result != game_plugins::Result::Continue)
                return result;
        }

        return game_plugins::Result::Continue;
    }
#endif

    // Helper to check if event data matches a filter hashtable
    static bool matches_filter(const ser::Value& event_data, const ser::Hashtable& filter);
};
} // namespace server
