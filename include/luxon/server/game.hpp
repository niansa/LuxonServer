// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "global.hpp"
#include "apps.hpp"
#include "lobby.hpp"
#include "peer.hpp"
#include "coro_support.hpp"
#ifdef LUXON_SERVER_ENABLE_PLUGINS
#include "game_plugin_base.hpp"
#endif

#include <memory>
#include <unordered_set>
#include <vector>
#include <list>
#include <variant>
#include <bitset>
#include <array>
#include <utility>
#include <luxon/ser_types.hpp>
#include <luxon/ser_protocol_id.hpp>
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

namespace luxon::ser {
class IProtocol;
}

namespace server {
class App;
struct Lobby;
struct Peer;

struct GameInfo {
    LobbyInfo lobby;

    std::string_view id, server_address;

    void encode_game_info(ser::ParameterList& params) const;
    bool has_game_info() const { return lobby.has_lobby_info() && !id.empty(); }
};

struct InterestGroups {
    std::bitset<256> bitset_{1};

    bool test(uint8_t v) const {
        if (v == 0)
            return true;

        return bitset_.test(v);
    }

    void set(uint8_t v, bool enable = true) { bitset_.set(v, enable); }
    void set() { bitset_.set(); }

    void reset(uint8_t v) { bitset_.reset(v); }
    void reset() { bitset_.reset(); }

    std::string to_string() const {
        auto bs = bitset_;
        bs.set(0);
        return bs.to_string();
    }
};

struct GamePeer {
    std::weak_ptr<Peer> peer;
    int32_t actor_id{};
    ser::Hashtable actor_props;
    InterestGroups interest_groups;

    bool is_valid() const { return actor_id > 0; }
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

    mutable std::array<ser::ByteArray, static_cast<size_t>(ser::ProtocolImplID::__length)> cached_data;
    std::expected<ser::ByteArray, ser::Error> get_cached_data(ser::IProtocol& protocol) const;

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
    const std::string_view server_address;

    ~Game();

#ifdef LUXON_SERVER_ENABLE_PLUGINS
    std::vector<std::unique_ptr<game_plugins::PluginBase>> plugins;
#endif

    uint8_t flags = 3; // CheckUserOnJoin | DeleteCacheOnLeave
    bool is_created = false;
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

    Game(std::shared_ptr<Lobby> lobby, std::string id, std::string_view server_address);

    std::list<GamePeer> peers;
    uint8_t dummy_peer_count = 0;

    ///
    /// \brief Returns the server manager that is managing this game
    /// \return Reference to server manager
    ///
    ServerManager& get_server_manager() const { return lobby->app->server_manager; }

    ///
    /// \brief Adds appid, appver, lobbyid, lobbytype, gameid to parameter list
    /// \param Parameter list to add info to
    ///
    void add_game_info(ser::ParameterList& params) const;

    ///
    /// \brief Gets GameInfo for current game
    /// \return Struct containing game identification information
    ///
    GameInfo get_game_info() const;

    ///
    /// \brief Checks if game info matches this game
    /// \param Info to check against
    /// \return True if info is for this game
    ///
    bool matches_game_info(const GameInfo& info) const;

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
    /// \param game_peer GamePeer to add to game, must've been previously been created by the same Game using create_peer()
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
    /// \brief Finds game peer with given actor_id
    /// \param actor_id Actor_id to look for
    /// \return Pointer to GamePeer with given actor_id if successful, otherwise nullptr
    ///
    GamePeer *find_peer(int32_t actor_id);
    ///
    /// \brief Finds game peer with given peer
    /// \param peer Peer to look for
    /// \return Pointer to GamePeer with given actor_id if successful, otherwise nullptr
    ///
    GamePeer *find_peer(const std::shared_ptr<Peer>& peer);
    ///
    /// \brief Broadcasts an event to the game
    /// \param event Event to broadcast
    ///
    void broadcast_event(Event& event);
    ///
    /// \brief Checks if user + given amount of expected users can join
    /// \param user_id User ID of primary user trying to join
    /// \param new_expected_users_count Amount of users to calculate in as well
    /// \return ErrorCode value and error string
    ///
    std::pair<int16_t, std::string_view> validate_join(const std::string& user_id, size_t new_expected_users_count = 0) const;

    ///
    /// \brief Updates the game in the lobby's game list
    ///
    void trigger_lobby_update();

    ///
    /// \brief Measures footprint of all dynamic data
    /// \return Measured footprint
    ///
    ser::Footprint measure_footprint(ser::FootprintLimits limits = {}) const;

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
    ser::Hashtable get_lobby_game_props() const;
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
    /// \brief Generates property update event for network distribution
    ///
    /// \param actor_id ID of the actor initiating the update (use 0 if update is triggered by server logic)
    /// \param props A collection of key/value pairs representing the properties to be updated on clients
    /// \param target_actor_id ID of specific actor whose properties are being modified, pass 0 to target global game properties instead of a specific actor
    /// \return A constructed Event object ready to be broadcasted to clients
    Event create_property_update_event(int32_t actor_id, ser::Hashtable props, int32_t target_actor_id = 0);

#ifdef LUXON_SERVER_ENABLE_PLUGINS
    template <typename InfoStruct>
    Awaitable<game_plugins::Result>
    execute_plugin_chain(Awaitable<game_plugins::Result> (game_plugins::PluginBase::*method)(luxon::ser::OperationRequestMessage&, InfoStruct&),
                         luxon::ser::OperationRequestMessage& req, InfoStruct& info) {
        for (const auto& plugin : plugins) {
            auto invocation = ((*plugin).*method)(req, info);
            game_plugins::Result result = lco_await std::move(invocation);
            if (result != game_plugins::Result::Continue)
                lco_return result;
        }

        lco_return game_plugins::Result::Continue;
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

    // Helper to decode game info
    static GameInfo decode_game_info(const ser::ParameterList& params);
};
} // namespace server
