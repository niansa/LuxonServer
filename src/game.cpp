// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "game.hpp"
#include "global.hpp"
#include "server_manager.hpp"

#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>

namespace server {
bool GamePeer::has_interest_group(uint8_t group) const {
    if (group == 0)
        return true;

    return interest_groups.contains(group);
}

bool GamePeer::disconnect() {
    if (auto peer_ = peer.lock()) {
        peer_->disconnect();
        return true;
    }

    return false;
}

Game::~Game(){// Call into plugins
              GAME_PLUGINS_INVOKE({
                  OnCloseGameCallInfo info{.failed_on_create = last_actor_id == 0};
                  execute_plugin_chain(&PluginBase::OnCloseGame, info);
              })}

GamePeer Game::create_peer(std::shared_ptr<Peer> peer) {
    GamePeer fres{.peer = std::move(peer)};

    if (!peer)
        return {};
    if (!peer->persistent)
        return {};

    // Find free actor number
    do {
        if (last_actor_id >= 0xfe)
            return {};
    } while (find_peer(++last_actor_id));
    fres.actor_id = last_actor_id;

    // Add user id
    if (flags & GameFlags::PublishUserId)
        fres.actor_props[ActorProps::UserId] = peer->persistent->user_id;

    return fres;
}

GamePeer *Game::add_peer(GamePeer&& game_peer) {
    auto peer = game_peer.peer.lock();
    if (!peer)
        return nullptr;

    // Make sure actor_id is unique
    if (find_peer(game_peer.actor_id))
        return nullptr;

    // Check user id uniqueness if enabled
    if (flags & GameFlags::CheckUserOnJoin)
        for (const auto& that_game_peer : peers)
            if (auto that_peer = that_game_peer.peer.lock())
                if (that_peer->persistent && that_peer->persistent->user_id == peer->persistent->user_id)
                    return nullptr;

    // Add peer to list
    auto& fres = peers.emplace_back(game_peer);

    // Remove user from expected users
    expected_users.erase(peer->persistent->user_id);

    // Update the lobby about new player count
    trigger_lobby_update();

    return &fres;
}

bool Game::remove_peer(const std::shared_ptr<Peer>& peer) {
    if (!peer)
        return false;

    // Clean up peer list until ineffective
    int32_t leaving_actor_id = 0;
restart:
    for (auto it = peers.begin(); it != peers.end(); ++it) {
        // Get peer from game peer
        if (auto this_peer = it->peer.lock()) {
            // Remove given peer
            if (this_peer.get() == peer.get()) {
                leaving_actor_id = it->actor_id;

                if (flags & GameFlags::DeleteCacheOnLeave)
                    event_cache.remove_if([leaving_actor_id](const Event& ev) { return ev.sender_actor_id == leaving_actor_id && ev.sender_actor_id != 0; });

                peers.erase(it);
                goto restart;
            }
        } else {
            // Remove stale peers
            peers.erase(it);
            goto restart;
        }
    }

    if (!leaving_actor_id)
        return false;

    // Update the lobby about new player count
    trigger_lobby_update();

    if (peers.empty()) {
        // Lobby is empty now, use a scheduled tasks to stay alive for at least empty_game_ttl milliseconds
        if (empty_game_ttl > 0)
            lobby->app->server_manager.add_scheduled_task(empty_game_ttl, [game = shared_from_this()]() {});

        // Call into plugins
        GAME_PLUGINS_INVOKE({
            BeforeCloseGameCallInfo info{.failed_on_create = last_actor_id == 0};
            execute_plugin_chain(&PluginBase::BeforeCloseGame, info);
        })
    } else if (leaving_actor_id == master_actor) {
        // Lobby is not empty yet, but there's no master assigned anymore, so assign a new one
        master_actor = peers.front().actor_id;
    }

    return true;
}

bool Game::flood_peer(GamePeer *game_peer) {
    // Send cached events to the new peer
    if (auto peer = game_peer->peer.lock()) {
        size_t count = 0;

        for (const auto& event : event_cache) {
            // Actor events don't go back to the sender
            if (event.sender_actor_id != 0 && event.sender_actor_id == game_peer->actor_id)
                continue;

            // Serialize event
            ser::EventMessage event_data{.event_code = event.code, .parameters = event.top_params};
            event_data.parameters[DictKeyCodes::GameAndActor::ActorNo] = static_cast<int32_t>(event.sender_actor_id);
            if (!event.data.is_null())
                event_data.parameters[DictKeyCodes::RoutingAndEvents::Data] = event.data;

            // Cached events are re-sent as if they were fresh to the joining player
            const auto expected_event_payload = peer->protocol->Serialize(event_data, false);
            if (!expected_event_payload)
                peer->log->warn("Failed to serialize flooded event: {}", expected_event_payload.error().message);
            else
                peer->send(*expected_event_payload, enet::EnetSendOptions{.channel = event.channel, .mode = event.delivery_mode});

            ++count;
        }

        peer->log->info("Client successfully flooded with {} events", count);
        return true;
    }
    return false;
}

GamePeer *Game::find_peer(int32_t actor_id) {
    for (auto& peer : peers)
        if (peer.actor_id == actor_id)
            return &peer;
    return nullptr;
}

void Game::broadcast_event(Event& event) {
    // Serialize event once if not cached
    ser::EventMessage event_data{.event_code = event.code, .parameters = std::move(event.top_params)};
    event_data.parameters[DictKeyCodes::GameAndActor::ActorNo] = static_cast<int32_t>(event.sender_actor_id);
    if (!event.data.is_null())
        event_data.parameters[DictKeyCodes::RoutingAndEvents::Data] = std::move(event.data);

    enet::EnetSendOptions send_options{.channel = event.channel, .mode = event.delivery_mode};

    // Set default recipients
    if (event.receivers.index() == 0)
        event.receivers = ReceiverGroup::Others;

    // Send to all recipients
    if (auto *receiver_group = std::get_if<uint8_t>(&event.receivers)) {
        if (*receiver_group == ReceiverGroup::MasterClient) {
            // Send to master client
            if (auto *game_peer = find_peer(master_actor)) {
                if (game_peer->has_interest_group(event.interest_group)) {
                    if (auto peer = game_peer->peer.lock()) {
                        const auto expected_event_payload = peer->protocol->Serialize(event_data, false);
                        if (!expected_event_payload)
                            peer->log->warn("Failed to serialize event: {}", expected_event_payload.error().message);
                        else
                            peer->send(*expected_event_payload, send_options);
                    }
                }
            }
        } else {
            // Send to others (or all)
            for (auto& game_peer : peers) {
                if (*receiver_group == ReceiverGroup::Others && game_peer.actor_id == event.sender_actor_id)
                    continue;
                if (!game_peer.has_interest_group(event.interest_group))
                    continue;
                if (auto peer = game_peer.peer.lock()) {
                    const auto expected_event_payload = peer->protocol->Serialize(event_data, false);
                    if (!expected_event_payload)
                        peer->log->warn("Failed to serialize event: {}", expected_event_payload.error().message);
                    else
                        peer->send(*expected_event_payload, send_options);
                }
            }
        }
    } else if (auto *actors = std::get_if<std::unordered_set<int32_t>>(&event.receivers)) {
        // Send to given actors
        for (const int32_t actor_id : *actors) {
            auto *game_peer = find_peer(actor_id);
            if (!game_peer)
                continue;
            if (!game_peer->has_interest_group(event.interest_group))
                continue;
            if (auto peer = game_peer->peer.lock()) {
                const auto expected_event_payload = peer->protocol->Serialize(event_data, false);
                if (!expected_event_payload)
                    peer->log->warn("Failed to serialize event: {}", expected_event_payload.error().message);
                else
                    peer->send(*expected_event_payload, send_options);
            }
        }
    }
}

int16_t Game::validate_join(const std::string& user_id, size_t new_expected_users_count) const {
    // Return error if game is closed
    if (!is_open)
        return ErrorCodes::Matchmaking::GameClosed;

    // Check capacity (peers + expected users)
    if (max_peers > 0) {
        const size_t current_count = peers.size();
        const size_t reserved_count = expected_users.size();

        // Expected users don't consume at slot
        const bool joining_user_is_reserved = expected_users.contains(user_id);

        // Calculate total needed slots
        size_t needed_slots = (joining_user_is_reserved ? 0 : 1) + new_expected_users_count;

        // Return error if game is full
        if (current_count + reserved_count + needed_slots > max_peers)
            return ErrorCodes::Matchmaking::GameFull;

        // Return error if actor list is full
        if (peers.size() + needed_slots > 0xfe)
            return ErrorCodes::Matchmaking::ActorListFull;
    }

    // Check user id uniqueness if enabled
    if (flags & GameFlags::CheckUserOnJoin)
        for (const auto& that_game_peer : peers)
            if (auto that_peer = that_game_peer.peer.lock())
                if (that_peer->persistent && that_peer->persistent->user_id == user_id)
                    return ErrorCodes::Matchmaking::JoinFail::JoinFailedPeerAlreadyJoined;

    return ErrorCodes::Core::Ok;
}

void Game::trigger_lobby_update() {
    auto shared_this = shared_from_this();
    for (auto& handler : lobby->game_list_update_handlers)
        handler.game_change(shared_this);
}

#define PROP_MAP                                                                                                                                               \
    PROP_MAP_ENTRY(MaxPlayers, uint8_t, max_peers, true);                                                                                                      \
    PROP_MAP_ENTRY(IsVisible, bool, is_visible, false);                                                                                                        \
    PROP_MAP_ENTRY(IsOpen, bool, is_open, true);                                                                                                               \
    PROP_MAP_ENTRY(PlayerTTL, int32_t, player_ttl, false);                                                                                                     \
    PROP_MAP_ENTRY(EmptyGameTTL, int32_t, empty_game_ttl, false);                                                                                              \
    PROP_MAP_ENTRY(MasterClientId, int32_t, master_actor, false);                                                                                              \
    PROP_MAP_ENTRY(LobbyProperties, std::vector<std::string>, lobby_props, true)

ser::Value Game::get_game_prop(const ser::Value& key) {
    bool update_lobby = false;

    if (key.is<uint8_t>()) {
        switch (key.get<uint8_t>()) {
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    case GameProps::game_param:                                                                                                                                \
        update_lobby |= updates_lobby;                                                                                                                         \
        return var;
            PROP_MAP
#undef PROP_MAP_ENTRY
        case GameProps::PlayerCount:
            return static_cast<uint8_t>(peers.size());
        }
    }

    if (update_lobby)
        trigger_lobby_update();

    auto res = custom_props.find(key);
    if (res == custom_props.end())
        return ser::Value(); // null
    return res->second;
}

ser::Hashtable Game::get_lobby_game_props() {
    ser::Hashtable fres;
    fres[GameProps::PlayerCount] = static_cast<uint8_t>(peers.size());
    fres[GameProps::IsOpen] = is_open;
    fres[GameProps::MaxPlayers] = max_peers;
    fres[GameProps::LobbyProperties] = lobby_props;
    return fres;
}

ser::Hashtable Game::get_game_props(bool no_custom) {
    auto fres = no_custom ? ser::Hashtable{} : custom_props;
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby) fres[GameProps::game_param] = static_cast<type>(var);
    PROP_MAP
#undef PROP_MAP_ENTRY
    fres[GameProps::PlayerCount] = static_cast<uint8_t>(peers.size());

    return fres;
}

ser::Hashtable Game::get_actor_props() {
    ser::Hashtable fres;
    for (const auto& game_peer : peers)
        fres[game_peer.actor_id] = game_peer.actor_props;
    return fres;
}

void Game::insert_game_props(ser::Hashtable update) {
    const bool delete_null = flags & GameFlags::DeleteNullProps;
    bool update_lobby = false;

    for (const auto& [key, value] : update) {
        if (key.is<uint8_t>()) {
            // Update built in props
            switch (key.get<uint8_t>()) {
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    case GameProps::game_param:                                                                                                                                \
        update_lobby |= value.store_if<type>(var) && updates_lobby;                                                                                            \
        break;
                PROP_MAP
#undef PROP_MAP_ENTRY
            }
        } else {
            // Update custom props
            if (!key.is_null()) {
                custom_props[key] = value;
            } else if (delete_null) {
                auto res = custom_props.find(key);
                if (res != custom_props.end())
                    custom_props.erase(res);
            }
        }
    }

    if (update_lobby)
        trigger_lobby_update();
}

bool Game::expect_game_props(ser::Hashtable expected) {
    bool ok = true;
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    if (expected.contains(GameProps::game_param))                                                                                                              \
        ok &= expected[GameProps::game_param].is_equal<type>(var);
    PROP_MAP
#undef PROP_MAP_ENTRY
    if (!ok)
        return false;

    for (const auto& [key, value] : expected) {
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    if (key == GameProps::game_param)                                                                                                                          \
        continue;
        PROP_MAP
#undef PROP_MAP_ENTRY

        if ((!custom_props.contains(key) && (!value.is_null() || !(flags & GameFlags::DeleteNullProps))) || custom_props.at(key) != value)
            return false;
    }

    return true;
}

bool Game::insert_actor_props(int32_t actor_id, const ser::Hashtable& update) {
    auto *game_peer = find_peer(actor_id);
    if (!game_peer)
        return false;

    auto& actor_props = game_peer->actor_props;

    const bool delete_null = flags & GameFlags::DeleteNullProps;
    for (const auto& [key, value] : update) {
        if (!key.is_null()) {
            actor_props[key] = value;
        } else if (delete_null) {
            auto res = actor_props.find(key);
            if (res != actor_props.end())
                actor_props.erase(res);
        }
    }

    return true;
}

bool Game::expect_actor_props(int32_t actor_id, const ser::Hashtable& expected) {
    const auto& actor_props = find_peer(actor_id)->actor_props;
    for (const auto& [key, value] : expected)
        if ((!actor_props.contains(key) && (!value.is_null() || !(flags & GameFlags::DeleteNullProps))) || actor_props.at(key) != value)
            return false;

    return true;
}

bool Game::matches_filter(const ser::Value& event_data, const ser::Hashtable& filter) {
    // If filter is empty, it's a match
    if (filter.empty())
        return true;

    // If event has no data but filter is not empty, no match
    if (!event_data.is<ser::HashtablePtr>())
        return false;

    const auto& data_ptr = event_data.get<ser::HashtablePtr>();
    if (!data_ptr)
        return false;
    const auto& data = *data_ptr;

    // Subset check
    for (const auto& [key, val] : filter) {
        if (!data.contains(key))
            return false;
        if (data.at(key) != val)
            return false;
    }

    return true;
}
} // namespace server
