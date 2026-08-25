// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "game.hpp"
#include "global.hpp"
#include "logger.hpp"
#include "server_manager.hpp"
#include "peer_persistence.hpp"
#include "ipc_codes.hpp"
#include "coro_support.hpp"

#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>
#include <tracy/Tracy.hpp>

namespace server {
void GameInfo::encode_game_info(ser::ParameterList& params) const {
    if (has_game_info()) {
        params[DictKeyCodes::GameAndActor::GameId] = std::string(id);
        params[DictKeyCodes::LoadBalancing::Address] = std::string(server_address);
    }
    lobby.encode_lobby_info(params);
}

bool GamePeer::disconnect() {
    if (auto peer_ = peer.lock()) {
        peer_->disconnect();
        return true;
    }

    return false;
}

std::expected<ser::ByteArray, ser::Error> Event::get_cached_data(ser::IProtocol& protocol) const {
    ZoneScoped;

    const auto protocol_index = static_cast<size_t>(protocol.GetProtcolImplID());

    // Return the already serialized network packet for this protocol
    ser::ByteArray *cache_ptr{};
    if (protocol_index < cached_data.size()) {
        auto& cache = cached_data[protocol_index];
        if (!cache.empty())
            return cache;
        cache_ptr = &cache;
    } else {
        create_logger("Event::get_cached_data")
            ->warn("Trying to use protocol index {}, but maximum cache index is {}! THIS IS A BUG IN LUXON SERVER, PLEASE REPORT!", protocol_index,
                   cached_data.size());
    }

    // Build the base event message
    ser::EventMessage event_data{.event_code = code, .parameters = top_params};
    event_data.parameters[DictKeyCodes::GameAndActor::ActorNo] = static_cast<int32_t>(sender_actor_id);

    if (!data.is_null())
        event_data.parameters[DictKeyCodes::RoutingAndEvents::Data] = data.as_ref();

    // Serialize the complete packet
    auto expected_payload = protocol.Serialize(event_data, false);
    if (!expected_payload)
        return std::unexpected(expected_payload.error());

    // Cache it if it's a known protocol
    if (cache_ptr)
        *cache_ptr = *expected_payload;

    return *expected_payload;
}

Game::~Game() {
    // Call into plugins
    GAME_PLUGINS_INVOKE({
        OnCloseGameCallInfo info{.failed_on_create = !is_created};
        execute_plugin_chain(&PluginBase::OnCloseGame, info);
    });

    auto& server_manager = get_server_manager();
#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    ser::EventMessage ipc_event;
    ipc_event.event_code = IPCEventCodes::GameDelete;
    add_game_info(ipc_event.parameters);
    server_manager.ipc_broadcast(ipc_event);
#endif

    server_manager.get_logger().info("Game '{}' in app '{}' is being deleted", lobby->app->id, id);
}

Game::Game(std::shared_ptr<Lobby> lobby, std::string id, std::string_view server_address)
    : lobby(std::move(lobby)), id(std::move(id)), server_address(get_server_manager().get_static_endpoint_address_str(server_address)) {}

void Game::add_game_info(ser::ParameterList& params) const {
    params[DictKeyCodes::GameAndActor::GameId] = id;
    params[DictKeyCodes::LoadBalancing::Address] = std::string(server_address);
    lobby->add_lobby_info(params);
}

GameInfo Game::get_game_info() const {
    GameInfo fres(lobby->get_lobby_info());
    fres.id = id;
    fres.server_address = server_address;
    return fres;
}

bool Game::matches_game_info(const GameInfo& info) const {
    if (info.id != id)
        return false;

    if (info.lobby.app.id != lobby->app->id)
        return false;

    if (info.lobby.app.version != lobby->app->version)
        return false;

    return true;
}

GamePeer Game::create_peer(std::shared_ptr<Peer> peer) {
    ZoneScoped;

    GamePeer fres{.peer = std::move(peer)};

    if (!peer)
        return {};
    if (!peer->persistent)
        return {};

    // Find free actor number
    int start_id = last_actor_id;

    do {
        // Wrap around to 1
        if (last_actor_id >= 0xfe)
            last_actor_id = 0;
        ++last_actor_id;

        // Prevent infinite loop
        if (last_actor_id == start_id)
            return {};
    } while (find_peer(last_actor_id));

    fres.actor_id = last_actor_id;

    // Add user id
    if (flags & GameFlags::PublishUserId)
        fres.actor_props[ActorProps::UserId] = peer->persistent->user_id;

    return fres;
}

GamePeer *Game::add_peer(GamePeer&& game_peer) {
    ZoneScoped;

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
    ZoneScoped;

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
            BeforeCloseGameCallInfo info{.failed_on_create = !is_created};
            execute_plugin_chain(&PluginBase::BeforeCloseGame, info);
        });
    } else if (leaving_actor_id == master_actor) {
        // Lobby is not empty yet, but there's no master assigned anymore, so assign a new one
        master_actor = peers.front().actor_id;
    }

    return true;
}

bool Game::flood_peer(GamePeer *game_peer) {
    ZoneScoped;

    // Send cached events to the new peer
    if (auto peer = game_peer->peer.lock()) {
        size_t count = 0;

        for (const auto& event : event_cache) {
            // Actor events don't go back to the sender
            if (event.sender_actor_id != 0 && event.sender_actor_id == game_peer->actor_id)
                continue;

            // Cached events are re-sent as if they were fresh to the joining player
            const auto expected_event_payload = event.get_cached_data(*peer->protocol);
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
    ZoneScoped;

    for (auto& game_peer : peers)
        if (game_peer.actor_id == actor_id)
            return &game_peer;
    return nullptr;
}

GamePeer *Game::find_peer(const std::shared_ptr<Peer>& peer) {
    ZoneScoped;

    for (auto& game_peer : peers)
        if (game_peer.peer.lock() == peer)
            return &game_peer;
    return nullptr;
}

void Game::broadcast_event(Event& event) {
    ZoneScoped;

    enet::EnetSendOptions send_options{.channel = event.channel, .mode = event.delivery_mode};

    // Set default recipients
    if (event.receivers.index() == 0)
        event.receivers = ReceiverGroup::Others;

    // Dispatcher
    const auto dispatch = [&](GamePeer& game_peer) {
        if (auto peer = game_peer.peer.lock()) {
            const auto expected_event_payload = event.get_cached_data(*peer->protocol);
            if (!expected_event_payload)
                peer->log->warn("Failed to serialize event: {}", expected_event_payload.error().message);
            else
                peer->send(*expected_event_payload, send_options);
        }
    };

    // Send to all recipients
    if (auto *receiver_group = std::get_if<uint8_t>(&event.receivers)) {
        if (*receiver_group == ReceiverGroup::MasterClient) {
            // Send to master client
            if (auto *game_peer = find_peer(master_actor))
                if (game_peer->interest_groups.test(event.interest_group))
                    dispatch(*game_peer);
        } else {
            // Send to others (or all)
            for (auto& game_peer : peers) {
                if (*receiver_group == ReceiverGroup::Others && game_peer.actor_id == event.sender_actor_id)
                    continue;
                if (!game_peer.interest_groups.test(event.interest_group))
                    continue;
                dispatch(game_peer);
            }
        }
    } else if (auto *actors = std::get_if<std::unordered_set<int32_t>>(&event.receivers)) {
        // Send to given actors
        for (const int32_t actor_id : *actors) {
            auto *game_peer = find_peer(actor_id);
            if (!game_peer)
                continue;
            if (!game_peer->interest_groups.test(event.interest_group))
                continue;
            dispatch(*game_peer);
        }
    }
}

std::pair<int16_t, std::string_view> Game::validate_join(const std::string& user_id, size_t new_expected_users_count) const {
    ZoneScoped;

    // Return error if game hasn't been created yet
    if (!is_created)
        return {ErrorCodes::Matchmaking::GameIdNotExists, "Game does not exist"};

    // Return error if game is closed
    if (!is_open)
        return {ErrorCodes::Matchmaking::GameClosed, "Game is closed"};

    // Check capacity (peers + expected users)
    if (max_peers > 0) {
        const size_t current_count = peers.size() + dummy_peer_count;
        const size_t reserved_count = expected_users.size();

        // Expected users don't consume at slot
        const bool joining_user_is_reserved = expected_users.contains(user_id);

        // Calculate total needed slots
        size_t needed_slots = (joining_user_is_reserved ? 0 : 1) + new_expected_users_count;

        // Calculate amount peers that have to be allowed
        const auto final_peer_count = current_count + reserved_count + needed_slots;

        // Return error if game is full
        if (final_peer_count > max_peers)
            return {ErrorCodes::Matchmaking::GameFull, "Game is full"};

        // Return error if limit of peers per game is reached
        if (const auto max_game_peers = lobby->app->get_settings().max_peers_per_game)
            if (final_peer_count > max_game_peers)
                return {ErrorCodes::Matchmaking::GameFull, "Game is full"};

        // Return error if actor list is full
        if (current_count + needed_slots > 0xfe)
            return {ErrorCodes::Matchmaking::ActorListFull, "Game is full"};
    }

    // Check user id uniqueness if enabled
    if (flags & GameFlags::CheckUserOnJoin)
        for (const auto& that_game_peer : peers)
            if (auto that_peer = that_game_peer.peer.lock())
                if (that_peer->persistent && that_peer->persistent->user_id == user_id)
                    return {ErrorCodes::Matchmaking::JoinFail::JoinFailedPeerAlreadyJoined, "Game already joined"};

    return {ErrorCodes::Core::Ok, {}};
}

void Game::trigger_lobby_update() {
    ZoneScoped;

    // Call handlers
    if (is_visible) {
        auto shared_this = shared_from_this();
        for (auto& handler : lobby->game_list_update_handlers)
            handler.game_update(shared_this);
    }

#ifdef LUXON_SERVER_ENABLE_MULTIPROCESSING
    // Send IPC event
    ser::EventMessage ipc_event;
    ipc_event.event_code = IPCEventCodes::GameUpdate;
    add_game_info(ipc_event.parameters);
    ipc_event.parameters[DictKeyCodes::Properties::GameProperties] = std::make_shared<ser::Hashtable>(get_lobby_game_props());
    if (!server_address.empty())
        ipc_event.parameters[DictKeyCodes::LoadBalancing::Address] = std::string(server_address);
    get_server_manager().ipc_broadcast(ipc_event);
#endif
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
    ZoneScoped;

    bool update_lobby = false;

    if (key.is<uint8_t>()) {
        switch (key.get<uint8_t>()) {
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    case GameProps::game_param:                                                                                                                                \
        return var;
            PROP_MAP
#undef PROP_MAP_ENTRY
        case GameProps::PlayerCount:
            return static_cast<uint8_t>(peers.size() + dummy_peer_count);
        }
    }

    if (auto res = custom_props.find(key); res != custom_props.end())
        return res->second;
    return ser::Value(); // null
}

ser::Hashtable Game::get_lobby_game_props() const {
    ZoneScoped;

    ser::Hashtable fres;
    fres[GameProps::PlayerCount] = static_cast<uint8_t>(peers.size() + dummy_peer_count);
    fres[GameProps::IsOpen] = is_open;
    fres[GameProps::MaxPlayers] = max_peers;

    for (const auto& key : lobby_props)
        if (custom_props.contains(key))
            fres.emplace(key, custom_props.at(key));

    return fres;
}

ser::Hashtable Game::get_game_props(bool no_custom) {
    ZoneScoped;

    auto fres = no_custom ? ser::Hashtable{} : custom_props;
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby) fres[GameProps::game_param] = static_cast<type>(var);
    PROP_MAP
#undef PROP_MAP_ENTRY
    fres[GameProps::PlayerCount] = static_cast<uint8_t>(peers.size() + dummy_peer_count);

    return fres;
}

ser::Hashtable Game::get_actor_props() {
    ZoneScoped;

    ser::Hashtable fres;
    for (const auto& game_peer : peers)
        fres[game_peer.actor_id] = game_peer.actor_props;
    return fres;
}

void Game::insert_game_props(ser::Hashtable update) {
    ZoneScoped;

    const bool delete_null = flags & GameFlags::DeleteNullProps;
    bool update_lobby = false;
    bool delete_from_lobby = false;

    for (const auto& [key, value] : update) {
        if (key.is<uint8_t>()) {
            // Update built in props
            switch (key.get<uint8_t>()) {
#define PROP_MAP_ENTRY(game_param, type, var, updates_lobby)                                                                                                   \
    case GameProps::game_param:                                                                                                                                \
        /* Delete game from lobby list if freshly made invisible */                                                                                            \
        if constexpr (GameProps::game_param == GameProps::IsVisible)                                                                                           \
            if (is_visible && value.is<bool>())                                                                                                                \
                delete_from_lobby = !value.get<bool>();                                                                                                        \
        /* If changing master isn't allowed, just ignore the attempt */                                                                                        \
        if constexpr (GameProps::game_param == GameProps::MasterClientId)                                                                                      \
            if (!lobby->app->get_settings().allow_change_master)                                                                                               \
                break;                                                                                                                                         \
        update_lobby |= value.store_if<type>(var) && updates_lobby;                                                                                            \
        break;
                PROP_MAP
#undef PROP_MAP_ENTRY
            }
        } else {
            // Update custom props
            if (!key.is_null())
                custom_props[key] = value;
            else if (delete_null)
                if (auto res = custom_props.find(key); res != custom_props.end())
                    custom_props.erase(res);
        }
    }

    // Delete game from lobby list if made invisible
    if (delete_from_lobby)
        for (auto& handler : lobby->game_list_update_handlers)
            handler.game_delete(this);

    if (update_lobby)
        trigger_lobby_update();
}

bool Game::expect_game_props(ser::Hashtable expected) {
    ZoneScoped;

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
    ZoneScoped;

    auto *game_peer = find_peer(actor_id);
    if (!game_peer)
        return false;

    auto& actor_props = game_peer->actor_props;

    const bool delete_null = flags & GameFlags::DeleteNullProps;
    for (const auto& [key, value] : update) {
        if (!key.is_null())
            actor_props[key] = value;
        else if (delete_null)
            if (auto res = actor_props.find(key); res != actor_props.end())
                actor_props.erase(res);
    }

    return true;
}

bool Game::expect_actor_props(int32_t actor_id, const ser::Hashtable& expected) {
    ZoneScoped;

    const auto& actor_props = find_peer(actor_id)->actor_props;
    for (const auto& [key, value] : expected)
        if ((!actor_props.contains(key) && (!value.is_null() || !(flags & GameFlags::DeleteNullProps))) || actor_props.at(key) != value)
            return false;

    return true;
}

Event Game::create_property_update_event(int32_t actor_id, ser::Hashtable props, int32_t target_actor_id) {
    Event event{.code = EventCodes::PropertiesUpdate,
                .sender_actor_id = actor_id,
                .receivers = (flags & GameFlags::BroadcastPropsChangeToAll) ? ReceiverGroup::All : ReceiverGroup::Others};
    if (target_actor_id)
        event.top_params[DictKeyCodes::GameAndActor::TargetActorNo] = target_actor_id;
    event.top_params[DictKeyCodes::Properties::Properties] = std::move(props);
    return event;
}

bool Game::matches_filter(const ser::Value& event_data, const ser::Hashtable& filter) {
    ZoneScoped;

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

GameInfo Game::decode_game_info(const ser::ParameterList& params) {
    GameInfo fres(Lobby::decode_lobby_info(params));
    for (const auto& [key, val] : params) {
        if (key == DictKeyCodes::GameAndActor::GameId)
            fres.id = val.get<std::string>();
        if (key == DictKeyCodes::LoadBalancing::Address)
            fres.server_address = val.get<std::string>();
    }
    return fres;
}
} // namespace server
