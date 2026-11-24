// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "handler_gameserver.hpp"
#include "game.hpp"
#ifdef LUXON_SERVER_ENABLE_PLUGINS
#include "game_plugin_registry.hpp"
#include "game_plugin_base.hpp"
#endif
#include "global.hpp"
#include "data_model.hpp"
#include "authentication.hpp"
#include "server_manager.hpp"

#include <ranges>
#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>
#include <tracy/Tracy.hpp>

namespace server {
namespace models {
using namespace DictKeyCodes;

using RaiseEvent = Model<Parameter<std::vector<int32_t>, GameAndActor::ActorList, true>,
                         Parameter<ReceiverGroup::Enum, RoutingAndEvents::ReceiverGroup, false, DefaultConst<ReceiverGroup::Others>>,
                         Parameter<uint8_t, RoutingAndEvents::InterestGroup, false, DefaultConst<0>>,
                         Parameter<CacheOperation::Enum, RoutingAndEvents::Cache, false, DefaultConst<CacheOperation::DoNotCache>>,
                         Parameter<uint8_t, RoutingAndEvents::Code, false, DefaultConst<200>>>;

using JoinOrCreateGame = Model<Parameter<std::string, GameAndActor::GameId>, Parameter<bool, RoutingAndEvents::Broadcast, false, DefaultConst<true>>,
                               Parameter<uint8_t, AuthAndLobby::CreateIfNotExists, false, DefaultConst<false>>,
                               Parameter<std::vector<std::string>, RpcAndPlugins::Plugins, false, DefaultInit>,
                               Parameter<ser::HashtablePtr, Properties::GameProperties, false, DefaultInit>,
                               Parameter<ser::HashtablePtr, Properties::ActorProperties, false, DefaultInit>, Parameter<int32_t, GameSettings::PlayerTTL, true>,
                               Parameter<int32_t, GameSettings::EmptyRoomTTL, true>, Parameter<int32_t, GameSettings::GameFlags, true>,
                               Parameter<bool, GameSettings::CheckUserOnJoin, true>, Parameter<bool, RoutingAndEvents::SuppressRoomEvents, true>,
                               Parameter<bool, RoutingAndEvents::PublishUserId, true>>;

using SetProperties = Model<Parameter<ser::HashtablePtr, Properties::Properties, false>,
                            Parameter<ser::HashtablePtr, Properties::ExpectedValues, false, DefaultInit>,
                            Parameter<bool, RoutingAndEvents::Broadcast, false, DefaultConst<true>>,
                            Parameter<int32_t, GameAndActor::ActorNo, false, DefaultConst<0>>>;

using ChangeInterestGroups = Model<Parameter<ser::ByteArray, RoutingAndEvents::Add, true>, Parameter<ser::ByteArray, RoutingAndEvents::Remove, true>>;
} // namespace models

Awaitable<> GameServerHandler::HandleDisconnect() {
    ZoneScoped;

    if (auto& game = current_game_) {
        if (game_peer_) {

            // Cleanup cache if enabled
            if (game->flags & DictKeyCodes::RoutingAndEvents::CleanupCacheOnLeave)
                game->event_cache.remove_if([&](const Event& cached_event) { return cached_event.sender_actor_id == game_peer_->actor_id; });

            if (!has_left_) {
                // Call into plugins
                GAME_PLUGINS_INVOKE({
                    OnLeaveGameCallInfo info{.leaver = game_peer_};
                    ser::OperationRequestMessage req{.operation_code = OpCodes::Lite::Leave};
                    lco_await game->execute_plugin_chain(&PluginBase::OnLeave, req, info);
                });
            }

            // Remove peer
            peer_->log->info("Removing peer from game...");
            const int32_t actor_id = game_peer_ ? game_peer_->actor_id : 0;
            const bool was_master = actor_id == game->master_actor;
            if (!game->remove_peer(peer_))
                peer_->log->warn("Failed to remove peer from game");

            // Broadcast leave event
            if (!(game->flags & GameFlags::SuppressRoomEvents)) {
                std::vector<int32_t> actor_ids;
                for (auto& game_peer : game->peers)
                    actor_ids.push_back(game_peer.actor_id);

                Event event{.code = EventCodes::Leave, .sender_actor_id = actor_id, .receivers = ReceiverGroup::All};
                event.top_params[DictKeyCodes::GameAndActor::ActorNo] = actor_id;
                event.top_params[DictKeyCodes::GameAndActor::ActorList] = &actor_ids;
                if (was_master)
                    event.top_params[DictKeyCodes::MetadataAndMisc::MasterClientId] = game->master_actor;
                current_game_->broadcast_event(event);
            }
        }

        game.reset();
    }

    lco_await HandlerBase::HandleDisconnect();
}

Awaitable<> GameServerHandler::HandleOperationRequest(ser::OperationRequestMessage&& req, bool is_encrypted, const enet::EnetCommandHeader& cmd_header) {
    ZoneScoped;

    const auto ensure_is_master = [&]() {
        const bool is_master = game_peer_ && game_peer_->actor_id == current_game_->master_actor || current_game_->peers.size() == 0;
        if (!is_master) {
            const ser::OperationResponseMessage resp{
                .operation_code = req.operation_code, .return_code = ErrorCodes::Core::OperationNotAllowedInCurrentState, .debug_message = "Must be master"};
            send(proto_->Serialize(resp), enet::EnetSendOptions{cmd_header.channel_id});
            return false;
        }
        return true;
    };
    const auto ensure_joined_state = [&](bool joined = true) {
        if ((game_peer_ && game_peer_->actor_id != 0) != joined) {
            const ser::OperationResponseMessage resp{
                .operation_code = req.operation_code, .return_code = ErrorCodes::Core::OperationNotAllowedInCurrentState, .debug_message = "Must join first"};
            send(proto_->Serialize(resp), enet::EnetSendOptions{cmd_header.channel_id});
            return false;
        }
        return true;
    };

    if (!peer_->is_authenticated()) {
        if (cmd_header.channel_id != 0)
            lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);

        switch (req.operation_code) {

        case OpCodes::Auth::Authenticate:
        case OpCodes::Auth::AuthenticateOnce: {
            ZoneScopedN("HandleOperationRequest_Authenticate");

            // Try to authenticate
            auto resp = lco_await authenticate(server_manager_, *peer_, req, cmd_header, false);

            // Add position parameter if authentication was successful
            if (resp.return_code == ErrorCodes::Core::Ok)
                resp.parameters[DictKeyCodes::LoadBalancing::Position] = static_cast<int32_t>(0);

            // Send response
            send(proto_->Serialize(resp, is_encrypted));

            // Disconnect on error
            if (!peer_->is_authenticated()) {
                peer_->disconnect();
                lco_return;
            }

            // Set current game
            auto& pp = *peer_->persistent;
            auto expected_game = server_manager_.get_game(pp.get_invitation());
            pp.reset_game(); // Effectively expire peer's game invitation and memory ownership
            if (expected_game) {
                if (server_manager_.is_game_external(**expected_game)) {
                    peer_->log->error("Peer tried to join an external game! Forcing disconnect.");
                    const ser::OperationResponseMessage resp{.operation_code = req.operation_code,
                                                             .return_code = ErrorCodes::Matchmaking::ServerForbidden,
                                                             .debug_message = "Not invited to server"};
                    send(proto_->Serialize(resp), enet::EnetSendOptions{.channel = cmd_header.channel_id});
                    peer_->disconnect();
                    lco_return;
                } else {
                    current_game_ = std::move(*expected_game);
                }
            } else {
                peer_->log->error("Persistent peer doesn't have valid game assigned! Forcing disconnect.");
                const ser::OperationResponseMessage resp{.operation_code = req.operation_code,
                                                         .return_code = ErrorCodes::Matchmaking::ServerForbidden,
                                                         .debug_message = "Not invited to any game server"};
                send(proto_->Serialize(resp), enet::EnetSendOptions{.channel = cmd_header.channel_id});
                peer_->disconnect();
                lco_return;
            }

            lco_return;
        }
        }
    } else if (auto game = current_game_) {

        if (req.operation_code == OpCodes::Lite::RaiseEvent) {
            ZoneScopedN("HandleOperationRequest_RaiseEvent");

            using namespace DictKeyCodes::RoutingAndEvents;
            using DictKeyCodes::GameAndActor::ActorList;

            if (!ensure_joined_state())
                lco_return;

            const auto params = models::RaiseEvent::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            const auto cache_op = params->get<Cache>();

            // Build event
            Event event;
            event.sender_actor_id = game_peer_->actor_id;
            event.code = params->get<Code>();
            if (auto it = req.parameters.find(Data); it != req.parameters.end())
                event.data = std::move(it->second);
            event.delivery_mode = enet::FlagsToEnetDeliveryMode(cmd_header.flags);
            event.interest_group = params->get<InterestGroup>();
            event.channel = cmd_header.channel_id;
            if (const auto *actors = params->get<ActorList>())
                event.receivers = *actors | std::ranges::to<std::unordered_set>();
            else
                event.receivers = params->get<DictKeyCodes::RoutingAndEvents::ReceiverGroup>();

            // Call into plugins
            GAME_PLUGINS_INVOKE({
                OnRaiseEventCallInfo info{.raiser = game_peer_, .event = event, .cache_op = cache_op};
                const Result res = lco_await game->execute_plugin_chain(&PluginBase::OnRaiseEvent, req, info);

                if (res == Result::Cancel)
                    lco_return;
                if (res == Result::Fail) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Lite::RaiseEvent,
                                                             .return_code = ErrorCodes::Matchmaking::PluginReportedError};
                    send(proto_->Serialize(resp), enet::EnetSendOptions{.channel = cmd_header.channel_id});
                    lco_return;
                }
            });

            // Make sure client isn't attempting to raise a Photon event
            if (event.code > 220) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Lite::RaiseEvent,
                                                         .return_code = ErrorCodes::Core::OperationInvalid,
                                                         .debug_message = "Not allowed to raise Photon events (codes higher than 220)"};
                send(proto_->Serialize(resp), enet::EnetSendOptions{.channel = cmd_header.channel_id});
                lco_return;
            }

            // RemoveFromRoomCache
            if (cache_op == CacheOperation::RemoveFromRoomCache) {
                std::vector<int32_t> filter_senders;
                // Use target actors option to specify the sender number
                if (const auto& actors = params->get<ActorList>())
                    filter_senders = *actors;

                ser::Hashtable filter_data;
                if (event.data.is<ser::HashtablePtr>())
                    if (auto ptr = event.data.get<ser::HashtablePtr>())
                        filter_data = *ptr;

                // Event code 0 is wildcard
                const bool wildcard_code = event.code == 0;

                game->event_cache.remove_if([&](const Event& cached_event) {
                    // Code Filter
                    if (!wildcard_code && cached_event.code != event.code)
                        return false;

                    // Sender Filter
                    if (!filter_senders.empty()) {
                        bool found = false;
                        for (int32_t id : filter_senders) {
                            if (id == cached_event.sender_actor_id) {
                                found = true;
                                break;
                            }
                        }
                        if (!found)
                            return false;
                    }

                    // Data filter (subset match)
                    if (!filter_data.empty())
                        if (!Game::matches_filter(cached_event.data, filter_data))
                            return false;

                    return true;
                });
                lco_return; // Do NOT broadcast removal
            }

            // RemoveFromCacheForActorsLeft
            if (cache_op == CacheOperation::RemoveFromCacheForActorsLeft) {
                game->event_cache.remove_if([&](const Event& cached_event) {
                    return game->find_peer(cached_event.sender_actor_id) == nullptr && cached_event.sender_actor_id != 0; // Don't remove global
                });
                lco_return;
            }

            // Add To Cache
            const bool can_cache = (cache_op == CacheOperation::AddToRoomCache || cache_op == CacheOperation::AddToRoomCacheGlobal);
            if (can_cache && params->get<ActorList>() == nullptr &&
                params->get<DictKeyCodes::RoutingAndEvents::ReceiverGroup>() != ReceiverGroup::MasterClient &&
                params->get<DictKeyCodes::RoutingAndEvents::InterestGroup>() == 0) {
                // Make copy to allow potential change below to happen non-destructively
                Event cached_copy = event;

                if (cache_op == CacheOperation::AddToRoomCacheGlobal)
                    cached_copy.sender_actor_id = 0; // Can not be traced back

                game->event_cache.emplace_back(std::move(cached_copy));
            }

            // Broadcast
            game->broadcast_event(event);
            lco_return;
        }

        if (cmd_header.channel_id != 0)
            lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);

        switch (req.operation_code) {

        case OpCodes::Matchmaking::CreateGame:
        case OpCodes::Matchmaking::JoinGame: {
            ZoneScopedN("HandleOperationRequest_JoinGame");

            // Common validation
            if (!ensure_joined_state(false))
                lco_return;

            const auto params = models::JoinOrCreateGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            const bool is_master = game->peers.empty();

            // Validate game ID
            if (params->get<DictKeyCodes::GameAndActor::GameId>() != game->id) {
                const ser::OperationResponseMessage resp{.operation_code = req.operation_code,
                                                         .return_code = ErrorCodes::Matchmaking::GameIdNotExists,
                                                         .debug_message = "Token not valid for this Game ID"};
                send(proto_->Serialize(resp));
                lco_return;
            }

            if (is_master) {
#ifdef LUXON_SERVER_ENABLE_PLUGINS
                if (current_game_->plugins.empty())
#endif
                {
                    // Load given plugins if creating room
                    for (const std::string& plugin_name : params->get<DictKeyCodes::RpcAndPlugins::Plugins>()) {
#ifdef LUXON_SERVER_ENABLE_PLUGINS
                        auto plugin = game_plugins::registry::instantiate(current_game_.get(), plugin_name);
                        if (!plugin) {
                            peer_->log->warn("Attempting to load unknown game plugin: {}", plugin_name);
                            continue;
                        }

                        lco_await current_game_->plugins.emplace_back(std::move(plugin))->OnAttach();
#else
                peer_->log->warn("Attempting to load game plugin when plugins are disabled: {}", plugin_name);
#endif
                    }
                }
            } else {
                // Verify join if joining existing room
                const auto [join_validation_code, join_validation_message] = game->validate_join(peer_->persistent->user_id);
                if (join_validation_code != ErrorCodes::Core::Ok) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                             .return_code = join_validation_code,
                                                             .debug_message = std::string(join_validation_message)};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            }

            // Call into plugins
            GAME_PLUGINS_INVOKE({
                auto& game = current_game_;
                Result res;

                if (!game->is_created) {
                    OnCreateGameCallInfo info{.creator = peer_,
                                              .is_join = req.operation_code == OpCodes::Matchmaking::JoinGame,
                                              .create_if_not_exist = static_cast<bool>(params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>())};
                    res = lco_await game->execute_plugin_chain(&PluginBase::OnCreateGame, req, info);
                } else {
                    BeforeJoinGameCallInfo info{.joiner = peer_};
                    res = lco_await game->execute_plugin_chain(&PluginBase::BeforeJoin, req, info);
                }

                if (res == Result::Fail) {
                    const ser::OperationResponseMessage resp{.operation_code = req.operation_code, .return_code = ErrorCodes::Matchmaking::PluginReportedError};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            });

            if (is_master) {
                // Mark game as created
                game->is_created = true;

                // Apply game settings
                if (auto player_ttl = params->get<DictKeyCodes::GameSettings::PlayerTTL>())
                    game->player_ttl = *player_ttl;
                if (auto empty_room_ttl = params->get<DictKeyCodes::GameSettings::EmptyRoomTTL>())
                    game->empty_game_ttl = *empty_room_ttl;

                if (auto flags = params->get<DictKeyCodes::GameSettings::GameFlags>())
                    game->flags = *flags;

                auto set_flag = [&](int32_t flag, std::optional<bool> value) {
                    if (!value)
                        return;
                    if (*value)
                        game->flags |= flag;
                    else
                        game->flags &= ~flag;
                };

                set_flag(GameFlags::CheckUserOnJoin, params->get<DictKeyCodes::GameSettings::CheckUserOnJoin>());
                set_flag(GameFlags::SuppressRoomEvents, params->get<DictKeyCodes::RoutingAndEvents::SuppressRoomEvents>());
                set_flag(GameFlags::PublishUserId, params->get<DictKeyCodes::RoutingAndEvents::PublishUserId>());
            }

            // We capture props here so the response only contains the list of OTHER players if joining
            auto all_actor_props = game->get_actor_props();

            // Create peer for game
            auto game_peer = current_game_->create_peer(peer_);
            if (!game_peer.is_valid()) {
                peer_->log->error("Game peer could not be created. Connection must terminate now.");
                peer_->disconnect();
                lco_return;
            }

            // Add peer to game
            game_peer_ = game->add_peer(std::move(game_peer));
            if (!game_peer_) {
                peer_->log->error("Player could not be added to game. Connection must terminate now.");
                peer_->disconnect();
                lco_return;
            }
            peer_->log->info("Successfully joined game: {}", game->id);

            // Call into plugins
            bool broadcast_actor_props = true;
            GAME_PLUGINS_INVOKE({
                OnJoinGameCallInfo info{.joiner = &game_peer};
                const Result res = lco_await game->execute_plugin_chain(&PluginBase::OnJoinGame, req, info);

                if (res == Result::Fail) {
                    peer_->log->info("Reverting join", game->id);
                    game->remove_peer(peer_);
                    game_peer_ = nullptr;

                    const ser::OperationResponseMessage resp{.operation_code = req.operation_code, .return_code = ErrorCodes::Matchmaking::PluginReportedError};
                    send(proto_->Serialize(resp));
                    lco_return;
                }

                if (!info.publish_user_id.value_or(true))
                    game_peer.actor_props.erase(ActorProps::UserId);

                broadcast_actor_props = info.broadcast_actor_props.value_or(true);
            });

            // Update properties
            if (const auto& actor_props = params->get<DictKeyCodes::Properties::ActorProperties>())
                game->insert_actor_props(game_peer_->actor_id, *actor_props);
            if (is_master)
                if (const auto& game_props = params->get<DictKeyCodes::Properties::GameProperties>())
                    game->insert_game_props(*game_props);

            // Construct response
            ser::OperationResponseMessage resp;
            resp.operation_code = req.operation_code;
            resp.return_code = ErrorCodes::Core::Ok;

            std::vector<int32_t> actor_ids;
            if (!(game->flags & GameFlags::SuppressRoomEvents))
                for (auto& game_peer : game->peers)
                    actor_ids.push_back(game_peer.actor_id);

            resp.parameters[DictKeyCodes::GameSettings::GameFlags] = static_cast<int32_t>(game->flags);
            if (!(game->flags & GameFlags::SuppressRoomEvents))
                resp.parameters[DictKeyCodes::GameAndActor::ActorList] = &actor_ids;
            resp.parameters[DictKeyCodes::Properties::GameProperties] = game->get_game_props();
            resp.parameters[DictKeyCodes::GameAndActor::ActorNo] = game_peer_->actor_id;
            if (broadcast_actor_props)
                resp.parameters[DictKeyCodes::Properties::ActorProperties] = &all_actor_props;

            send(proto_->Serialize(resp));

            // Broadcast Join Event
            if (!(game->flags & GameFlags::SuppressRoomEvents)) {
                Event event{.code = EventCodes::Join, .sender_actor_id = game_peer_->actor_id, .receivers = ReceiverGroup::All};
                event.top_params[DictKeyCodes::GameAndActor::ActorList] = &actor_ids;
                event.top_params[DictKeyCodes::GameAndActor::ActorNo] = game_peer_->actor_id;
                if (broadcast_actor_props && !(game->flags & GameFlags::SuppressPlayerInfo))
                    event.top_params[DictKeyCodes::Properties::ActorProperties] = &game_peer_->actor_props;

                game->broadcast_event(event);
            }

            // Flood the client with current state
            game->flood_peer(game_peer_);

            lco_return;
        }

        case OpCodes::Lite::Leave: {
            ZoneScopedN("HandleOperationRequest_Leave");

            // Call into plugins
            GAME_PLUGINS_INVOKE({
                OnLeaveGameCallInfo info{.leaver = game_peer_};
                const Result res = lco_await game->execute_plugin_chain(&PluginBase::OnLeave, req, info);

                if (res == Result::Fail) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Lite::Leave, .return_code = ErrorCodes::Matchmaking::PluginReportedError};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            });

            // Send success response
            const ser::OperationResponseMessage resp{.operation_code = OpCodes::Lite::Leave, .return_code = ErrorCodes::Core::Ok};
            send(proto_->Serialize(resp));

            // Disconnect, handler will do the rest
            has_left_ = true;
            peer_->disconnect();

            lco_return;
        }

        case OpCodes::Lite::SetProperties: {
            ZoneScopedN("HandleOperationRequest_SetProperties");

            if (!ensure_joined_state())
                lco_return;

            const auto params = models::SetProperties::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            bool broadcast = params->get<DictKeyCodes::RoutingAndEvents::Broadcast>();
            auto actor_id = params->get<DictKeyCodes::GameAndActor::ActorNo>();

            const auto& props = params->get<DictKeyCodes::Properties::Properties>();
            const auto& props_expected = params->get<DictKeyCodes::Properties::ExpectedValues>();

            // Call into plugins
            GAME_PLUGINS_INVOKE({
                BeforeSetPropertiesCallInfo info{
                    .setter = game_peer_, .broadcast = broadcast, .target_actor_id = actor_id, .update = props, .expected = props_expected};
                const Result res = lco_await game->execute_plugin_chain(&PluginBase::BeforeSetProperties, req, info);

                if (res == Result::Fail) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Lite::SetProperties,
                                                         .return_code = ErrorCodes::Matchmaking::PluginReportedError};
                    send(proto_->Serialize(resp));
                    lco_return;
                }

                broadcast = info.broadcast;
                actor_id = info.target_actor_id;
            });

            // Set actor or game properties
            bool ok = true;
            if (actor_id) {
                if (props_expected)
                    ok = game->expect_actor_props(actor_id, *props_expected);
                if (ok)
                    ok = game->insert_actor_props(actor_id, *props);
            } else {
                if (props_expected)
                    ok = game->expect_game_props(*props_expected);
                if (ok)
                    game->insert_game_props(*props);
            }

            // Check for bad actor
            if (!ok && !game->find_peer(actor_id)) {
                const ser::OperationResponseMessage resp{.operation_code = req.operation_code,
                                                         .return_code = ErrorCodes::Core::OperationInvalid,
                                                         .debug_message = "Given actor does not exist"};
                send(proto_->Serialize(resp));
                lco_return;
            }

            // Emtpy response
            ser::OperationResponseMessage resp;
            resp.operation_code = OpCodes::Lite::SetProperties;
            resp.return_code = ok ? ErrorCodes::Core::Ok : ErrorCodes::Core::OperationInvalid;
            send(proto_->Serialize(resp));

            // Broadcast property updates
            if (ok && broadcast) {
                auto event = game->create_property_update_event(game_peer_->actor_id, *props, actor_id);
                game->broadcast_event(event);
            }

            // Call into plugins
            GAME_PLUGINS_INVOKE({
                OnSetPropertiesCallInfo info{
                    .setter = game_peer_, .broadcast = broadcast, .target_actor_id = actor_id, .update = props, .expected = props_expected};
                const Result res = lco_await game->execute_plugin_chain(&PluginBase::OnSetProperties, req, info);

                if (res == Result::Fail)
                    peer_->log->error("Plugin reported error for SetProperties after properties were already set");
            });

            lco_return;
        }

        case OpCodes::Lite::ChangeInterestGroups: {
            ZoneScopedN("HandleOperationRequest_ChangeInterestGroups");

            if (!ensure_joined_state())
                lco_return;

            const auto params = models::ChangeInterestGroups::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            if (const auto *removes = params->get<DictKeyCodes::RoutingAndEvents::Remove>()) {
                if (removes->empty()) {
                    game_peer_->interest_groups.reset();
                } else {
                    for (const uint8_t group : *removes)
                        game_peer_->interest_groups.reset(group);
                }
            }
            if (const auto *adds = params->get<DictKeyCodes::RoutingAndEvents::Add>()) {
                if (adds->empty()) {
                    game_peer_->interest_groups.set();
                } else {
                    for (const uint8_t group : *adds)
                        game_peer_->interest_groups.set(group);
                }
            }

            lco_return;
        }
        }
    } else if (allow_unsolicited_) {
        // Unsolicited join or create
        if (req.operation_code == OpCodes::Matchmaking::JoinGame || req.operation_code == OpCodes::Matchmaking::CreateGame) {
            const auto params = models::JoinOrCreateGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                lco_return;
            }

            // Find or create game
            auto& app = *peer_->persistent->app;
            const auto& app_games = app.get_games();
            auto game_id = params->get<DictKeyCodes::GameAndActor::GameId>();

            std::shared_ptr<Game> target_game;
            if (auto game_res = app_games.find(game_id); game_res != app_games.end()) {
                target_game = game_res->second.lock();
            } else {
                bool should_create = (req.operation_code == OpCodes::Matchmaking::CreateGame) || params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>();

                if (should_create) {
                    auto new_game = app.get_lobby()->create_game(std::string(game_id), "", true);
                    if (!new_game) {
                        send(proto_->Serialize(new_game.error()));
                        lco_return;
                    }
                    target_game = *new_game;
                } else {
                    const ser::OperationResponseMessage resp{
                        .operation_code = req.operation_code, .return_code = ErrorCodes::Matchmaking::GameIdNotExists, .debug_message = "Game does not exist"};
                    send(proto_->Serialize(resp));
                    lco_return;
                }
            }

            // Set as current game and disallow unsolicited join to prevent infinite recursion
            peer_->persistent->invite(std::move(target_game), req.operation_code == OpCodes::Matchmaking::CreateGame);
            allow_unsolicited_ = false;

            // Now that invitation is populated it will execute main logic block
            lco_return lco_await HandleOperationRequest(std::move(req), is_encrypted, cmd_header);
        }
    }

    lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);
}
} // namespace server
