// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "handler_masterserver.hpp"
#include "global.hpp"
#include "data_model.hpp"
#include "handler_gameserver.hpp"
#include "server_manager.hpp"
#include "authentication.hpp"
#include "lobby.hpp"

#include <string>
#include <random>
#include <algorithm>
#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>

// This is a very valuable ressource: https://doc.photonengine.com/realtime/current/lobby-and-matchmaking/matchmaking-and-lobby (2026-02-12)
// http://web.archive.org/web/20260212131901/https://doc.photonengine.com/realtime/current/lobby-and-matchmaking/matchmaking-and-lobby

namespace server {
namespace {
std::string generate_game_id(std::string prefix) {
    static std::mt19937 gen{std::random_device{}()};
    const std::string_view charset = "0123456789";
    std::uniform_int_distribution<size_t> dist(0, charset.size() - 1);

    std::string suffix(4, '\0');
    std::ranges::generate(suffix, [&] { return charset[dist(gen)]; });
    return prefix + '#' + suffix;
}
} // namespace

namespace models {
using namespace DictKeyCodes;

using ClientSettings = Model<Parameter<bool, AuthAndLobby::LobbyStats, true>>;

using LobbyId = Model<Parameter<std::string, AuthAndLobby::LobbyName, false, DefaultString<"">>,
                      Parameter<LobbyType::Enum, AuthAndLobby::LobbyType, false, DefaultConst<LobbyType::Default>>>;

using CreateGame = Model<Parameter<std::string, GameAndActor::GameId, false, DefaultString<"">>>;
using JoinGame = ExtendedModel<CreateGame, Parameter<uint8_t, AuthAndLobby::CreateIfNotExists, false, DefaultConst<false>>>;

using JoinRandomGame = Model<Parameter<MatchmakingType::Enum, LoadBalancing::MatchmakingType, false, DefaultInit>,
                             Parameter<ser::HashtablePtr, DictKeyCodes::Properties::GameProperties, false, DefaultInit>>;
} // namespace models

void MasterServerHandler::HandleSlowUpdate() {
    if (wants_app_stats_ && last_app_stats_.get() > 8000) {
        send_app_stats();
        last_app_stats_.reset();
    }

    HandlerBase::HandleSlowUpdate();
}

void MasterServerHandler::HandleOperationRequest(const ser::OperationRequestMessage& req, bool is_encrypted, const enet::EnetCommandHeader& cmd_header) {
    if (cmd_header.channel_id != 0)
        return HandlerBase::HandleOperationRequest(req, is_encrypted, cmd_header);

    if (!peer_->is_authenticated()) {
        switch (req.operation_code) {

        case OpCodes::Auth::Authenticate:
        case OpCodes::Auth::AuthenticateOnce: {
            const auto params = models::ClientSettings::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                return;
            }

            // Does the client want lobby stats?
            const bool wants_lobby_stats = params->get<DictKeyCodes::AuthAndLobby::LobbyStats>().value_or(true);

            // Try to authenticate
            auto resp = authenticate(server_manager_, *peer_, req, cmd_header);

            // Add details if authentication was successful
            if (resp.return_code == ErrorCodes::Core::Ok)
                resp.parameters[DictKeyCodes::LoadBalancing::Position] = static_cast<int32_t>(0);

            // Send response
            send(proto_->Serialize(resp, is_encrypted));

            // Handle successful authentication
            if (peer_->is_authenticated()) {
                auto& app = peer_->persistent->app;

                // Fully remove player's reference to current game
                peer_->persistent->current_game.reset();

                // Send stats once if requested
                wants_app_stats_ = wants_lobby_stats;
                if (wants_lobby_stats)
                    send_app_stats();
            }
            return;
        }
        }
    } else {
        switch (req.operation_code) {

        case OpCodes::Lobby::JoinLobby: {
            // Get lobby
            auto joined_lobby = get_requested_lobby(req);
            if (!joined_lobby) {
                send(proto_->Serialize(joined_lobby.error()));
                return;
            }

            // Join the lobby
            join_lobby(std::move(*joined_lobby));
            peer_->log->info("Joined lobby: {}", joined_lobby_->lobby->name.empty() ? "(unnamed)" : joined_lobby_->lobby->name);
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::JoinLobby, .return_code = ErrorCodes::Core::Ok};
            send(proto_->Serialize(resp));

            // Send game list
            send_game_list();
            return;
        }

        case OpCodes::Lobby::LeaveLobby: {
            // Try to leave lobby
            std::shared_ptr<Lobby> lobby;
            if (joined_lobby_.has_value()) {
                lobby = joined_lobby_->lobby;
                leave_lobby();
            }

            // Send response (code is always "Ok")
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::LeaveLobby, .return_code = ErrorCodes::Core::Ok};
            send(proto_->Serialize(resp));
            if (lobby)
                peer_->log->info("Left lobby: {}", lobby->name.empty() ? "(unnamed)" : lobby->name);
            else
                resp.debug_message = "Lobby not joined";
            send(proto_->Serialize(resp));
            return;
        }

        case OpCodes::Lobby::LobbyStats: { // TODO: This looks really unclean. What is going on?
            // Get filters
            const auto& lobby_name_param = req.parameters[DictKeyCodes::AuthAndLobby::LobbyName];
            const auto& lobby_type_param = req.parameters[DictKeyCodes::AuthAndLobby::LobbyType];

            // Build response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::LobbyStats};
            resp.parameters = get_lobby_stats([&](const Lobby& lobby) {
                if (lobby_name_param.is<std::string>() && lobby.name != lobby_name_param)
                    return false;
                if (lobby_type_param.is<std::string>() && lobby.type != lobby_type_param)
                    return false;
                return true;
            });

            // Send response
            send(proto_->Serialize(resp));
            return;
        }

        case OpCodes::Lobby::GetGameList: {
            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                return;
            }

            // Error out for non-sql lobbies
            if (lobby.value()->type == LobbyType::Default) {
                ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::GetGameList,
                                                   .return_code = ErrorCodes::Core::OperationInvalid,
                                                   .debug_message = "Lobby must be non-default lobby type"};
                send(proto_->Serialize(resp));
                return;
            }

            // Build response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Lobby::GetGameList};
            resp.parameters[DictKeyCodes::LoadBalancing::GameList] = get_game_list(**lobby);

            // Send response
            send(proto_->Serialize(resp));
            return;
        }

        case OpCodes::Matchmaking::CreateGame: {
            const auto params = models::CreateGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                return;
            }

            std::string game_id = params->get<DictKeyCodes::GameAndActor::GameId>();

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                return;
            }

            // Generate game ID if empty
            if (game_id.empty())
                game_id = generate_game_id(peer_->persistent->user_id);

            // Make sure no game with given ID already exists
            if (lobby.value()->games.contains(game_id)) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::CreateGame,
                                                         .return_code = ErrorCodes::Matchmaking::GameIdAlreadyExists,
                                                         .debug_message = "Game ID already exists"};
                send(proto_->Serialize(resp));
                return;
            }

            // Create new game with given ID
            peer_->log->info("Creating game: {}", game_id);
            auto game = lobby.value()->create_game(std::move(game_id));

            // Join the game
            peer_->persistent->current_game = game;

            // Build and send response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::CreateGame, .return_code = ErrorCodes::Core::Ok};
            resp.parameters[DictKeyCodes::LoadBalancing::Address] = server_manager_.get_endpoint_of(ServerType::GameServer);
            resp.parameters[DictKeyCodes::GameAndActor::GameId] = game->id;
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;

            send(proto_->Serialize(resp));
            peer_->log->info("Joining newly created game: {}", game->id);

            return;
        }

        case OpCodes::Matchmaking::JoinGame: {
            const auto params = models::JoinGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                return;
            }

            const std::string& game_id = params->get<DictKeyCodes::GameAndActor::GameId>();

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                return;
            }

            // Find game with given ID
            peer_->log->info("Finding game: {}", game_id);
            auto res = lobby.value()->games.find(game_id);

            std::shared_ptr<Game> game;
            bool is_new = false;
            if (res == lobby.value()->games.end()) {
                if (!params->get<DictKeyCodes::AuthAndLobby::CreateIfNotExists>()) {
                    const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                             .return_code = ErrorCodes::Matchmaking::GameIdNotExists,
                                                             .debug_message = "Game ID does not exist"};
                    send(proto_->Serialize(resp));
                    return;
                }

                // Generate game ID if empty
                std::string new_game_id;
                if (game_id.empty())
                    new_game_id = generate_game_id(peer_->persistent->user_id);
                else
                    new_game_id = game_id;

                game = lobby.value()->create_game(std::move(new_game_id));
                is_new = true;
            } else {
                game = res->second.lock();
            }

            // Make sure game hasn't expired
            if (!game) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame,
                                                         .return_code = ErrorCodes::Core::InternalServerError,
                                                         .debug_message = "Game has expired"};
                send(proto_->Serialize(resp));
                return;
            }

            // Validate join
            const int16_t join_validation_code = game->validate_join(peer_->persistent->user_id);
            if (join_validation_code != ErrorCodes::Core::Ok) {
                const ser::OperationResponseMessage resp{
                    .operation_code = OpCodes::Matchmaking::JoinGame, .return_code = join_validation_code, .debug_message = "Game closed or full"};
                send(proto_->Serialize(resp));
                return;
            }

            // Make token valid for this game
            peer_->persistent->current_game = game;

            // Expect user
            game->expected_users.emplace(peer_->persistent->user_id);

            // Build and send response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinGame, .return_code = ErrorCodes::Core::Ok};
            resp.parameters[DictKeyCodes::LoadBalancing::Address] = server_manager_.get_endpoint_of(ServerType::GameServer);
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;

            send(proto_->Serialize(resp));
            peer_->log->info("Joining {} game: {}", is_new ? "newly created" : "existing", game->id);
            return;
        }

        case OpCodes::Matchmaking::JoinRandomGame: {
            const auto params = models::JoinRandomGame::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                return;
            }

            // Get lobby
            auto lobby = get_requested_lobby(req);
            if (!lobby) {
                send(proto_->Serialize(lobby.error()));
                return;
            }

            ser::Hashtable expected_props;
            if (auto p = params->get<DictKeyCodes::Properties::GameProperties>())
                expected_props = *p;

            // Collect candidates
            std::vector<std::shared_ptr<Game>> candidates;

            // Better to allocate more than less?
            candidates.reserve(lobby.value()->games.size());

            for (auto& [id, weak_game] : lobby.value()->games) {
                auto game = weak_game.lock();
                if (!game)
                    continue;

                // Make sure game is joinable  TODO: Pass expected user count too
                if (!game->validate_join(peer_->persistent->user_id))
                    continue;

                // Property filter
                if (!game->expect_game_props(expected_props))
                    continue;

                candidates.push_back(std::move(game));
            }

            if (candidates.empty()) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinRandomGame,
                                                         .return_code = ErrorCodes::Matchmaking::NoRandomMatchFound,
                                                         .debug_message = "No matching game found"};
                send(proto_->Serialize(resp));
                return;
            }

            // The previous allocation might've been quite a bit overzealous, fix that
            candidates.shrink_to_fit();

            // Select Game based on matchmaking type
            std::shared_ptr<Game> selected_game;

            switch (params->get<DictKeyCodes::LoadBalancing::MatchmakingType>()) {
            case MatchmakingType::SerialMatching: {
                // Priorize games with fewer players
                std::ranges::sort(candidates, [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) { return a->peers.size() < b->peers.size(); });
                selected_game = candidates.front();
            } break;
            case MatchmakingType::FillRoom: {
                // Priorize games with more players
                std::ranges::sort(candidates, [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) { return a->peers.size() > b->peers.size(); });
                selected_game = candidates.front();

            } break;
            case MatchmakingType::RandomMatching: {
                // Uniform distribution
                static std::mt19937 rng(peer_->enet_peer->bytes_out());
                std::uniform_int_distribution<size_t> dist(0, candidates.size() - 1);
                selected_game = candidates[dist(rng)];
            } break;
            }

            // Return error if no game was selected
            if (!selected_game) {
                const ser::OperationResponseMessage resp{.operation_code = OpCodes::Matchmaking::JoinRandomGame,
                                                         .return_code = ErrorCodes::Matchmaking::NoRandomMatchFound,
                                                         .debug_message = "No match found"};
                send(proto_->Serialize(resp));
                return;
            }

            // Make token valid for this game
            peer_->persistent->current_game = selected_game;

            // Expect users  TODO: expect all given users
            selected_game->expected_users.emplace(peer_->persistent->user_id);

            // Send Response
            ser::OperationResponseMessage resp;
            resp.operation_code = OpCodes::Matchmaking::JoinRandomGame;
            resp.return_code = ErrorCodes::Core::Ok;

            // Payload similar to Create/Join Game
            resp.parameters[DictKeyCodes::LoadBalancing::Address] = server_manager_.get_endpoint_of(ServerType::GameServer);
            resp.parameters[DictKeyCodes::GameAndActor::GameId] = selected_game->id;
            resp.parameters[DictKeyCodes::LoadBalancing::Token] = peer_->persistent->token;

            send(proto_->Serialize(resp));
            peer_->log->info("Matchmaking success. Joining game: {}", selected_game->id);
            return;
        }

        case OpCodes::RpcAndMisc::Settings: {
            const auto params = models::ClientSettings::decode(req);
            if (!params) {
                send(proto_->Serialize(params.error()));
                return;
            }

            // Does the client want lobby stats?
            wants_app_stats_ = params->get<DictKeyCodes::AuthAndLobby::LobbyStats>().value_or(true);

            // No response
            return;
        }

        case OpCodes::Social::FindFriends: {
            // TODO: Stub. Reverse engineer and implement properly
            ser::OperationResponseMessage resp;
            resp.operation_code = OpCodes::Social::FindFriends;
            resp.return_code = ErrorCodes::Core::Ok;

            resp.parameters[DictKeyCodes::AuthAndLobby::FindFriendsResponseOnlineList] = std::vector<bool>{false};
            resp.parameters[DictKeyCodes::AuthAndLobby::FindFriendsResponseRoomIdList] = std::vector<std::string>{""};

            send(proto_->Serialize(resp));
            return;
        }
        }
    }

    return HandlerBase::HandleOperationRequest(req, is_encrypted, cmd_header);
}

std::expected<std::shared_ptr<Lobby>, ser::OperationResponseMessage> MasterServerHandler::get_requested_lobby(const ser::OperationRequestMessage& req) {
    const auto lobby_id = models::LobbyId::decode(req);
    if (!lobby_id)
        return std::unexpected(lobby_id.error());
    const std::string& lobby_name = lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyName>();

    if (lobby_name.empty() && joined_lobby_)
        return joined_lobby_->lobby;

    return peer_->persistent->app->get_lobby({lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyName>(), lobby_id->get<DictKeyCodes::AuthAndLobby::LobbyType>()});
}

void MasterServerHandler::join_lobby(std::shared_ptr<Lobby> lobby) {
    joined_lobby_.emplace(
        std::move(lobby),
        GameListUpdateHandler{
            .game_create =
                [this](const std::shared_ptr<Game>& game) {
                    // Send game creation
                    ser::EventMessage event;
                    event.event_code = EventCodes::GameList;
                    event.parameters[DictKeyCodes::LoadBalancing::GameList] = get_game_list(*game->lobby, [game](const Game& o) { return &o == game.get(); });

                    send(proto_->Serialize(event));
                },
            .game_change =
                [this](const std::shared_ptr<Game>& game) {
                    // Send game property change
                    ser::EventMessage event;
                    event.event_code = EventCodes::GameList;
                    event.parameters[DictKeyCodes::LoadBalancing::GameList] = get_game_list(*game->lobby, [game](const Game& o) { return &o == game.get(); });

                    send(proto_->Serialize(event));
                },
            .game_delete =
                [this](Game *game) {
                    // Send game removal
                    ser::EventMessage event;
                    event.event_code = EventCodes::GameList;
                    auto& game_list = *(event.parameters[DictKeyCodes::LoadBalancing::GameList] = std::make_shared<ser::Hashtable>()).get<ser::HashtablePtr>();
                    auto& game_props = *(game_list[game->id] = std::make_shared<ser::Hashtable>()).get<ser::HashtablePtr>();
                    game_props[GameProps::Removed] = true;

                    send(proto_->Serialize(event));
                }});
}

void MasterServerHandler::send_app_stats() {
    ser::EventMessage event;

    event.event_code = EventCodes::AppStats;
    event.parameters[DictKeyCodes::LoadBalancing::GameCount] = [this]() {
        int32_t fres = 0;
        for (auto&& app : App::get_all(server_manager_))
            for (const auto& [lobby_name, weak_lobby] : app->get_lobbies())
                if (auto lobby = weak_lobby.lock())
                    fres += lobby->games.size();
        return fres;
    }();
    event.parameters[DictKeyCodes::LoadBalancing::PeerCount] = static_cast<int32_t>(server_manager_.get_connection_count<GameServerHandler>());
    event.parameters[DictKeyCodes::LoadBalancing::MasterPeerCount] = static_cast<int32_t>(server_manager_.get_connection_count<MasterServerHandler>());

    send(proto_->Serialize(event));
}

ser::Dictionary MasterServerHandler::get_lobby_stats(std::function<bool(const Lobby&)> lobby_filter) {
    ser::Dictionary fres;

    auto& peer_count_arr = (fres[DictKeyCodes::LoadBalancing::PeerCount] = ser::ObjectArray()).get<ser::ObjectArray>();
    auto& game_count_arr = (fres[DictKeyCodes::LoadBalancing::GameCount] = ser::ObjectArray()).get<ser::ObjectArray>();
    auto& lobby_type_arr = (fres[DictKeyCodes::AuthAndLobby::LobbyType] = ser::ByteArray()).get<ser::ByteArray>();
    auto& lobby_name_arr = (fres[DictKeyCodes::AuthAndLobby::LobbyName] = ser::ObjectArray()).get<ser::ObjectArray>();

    auto& app = *peer_->persistent->app;
    for (const auto& [lobby_name, weak_lobby] : app.get_lobbies()) {
        if (auto lobby = weak_lobby.lock()) {
            if (lobby_filter && !lobby_filter(*lobby))
                continue;

            lobby_name_arr.emplace_back(lobby->name);
            lobby_type_arr.emplace_back(lobby->type);
            game_count_arr.emplace_back(static_cast<int32_t>(lobby->games.size()));
            peer_count_arr.emplace_back(static_cast<int32_t>(lobby->get_peer_count()));
        }
    }

    return fres;
}

void MasterServerHandler::send_lobby_stats() {
    ser::EventMessage event;

    event.event_code = EventCodes::LobbyStats;
    event.parameters = get_lobby_stats();

    send(proto_->Serialize(event));
}

ser::HashtablePtr MasterServerHandler::get_game_list(Lobby& lobby, std::function<bool(const Game&)> game_filter) {
    // TODO: This is VERY slow. Maintain pre-sorted lists in Lobby?

    auto fres = std::make_shared<ser::Hashtable>();

    if (!joined_lobby_.has_value())
        return fres;

    // Collect valid games into a vector
    std::vector<std::shared_ptr<Game>> sorted_games;
    sorted_games.reserve(lobby.games.size());

    for (auto& [name, weak_game] : lobby.games) {
        auto game = weak_game.lock();
        if (!game)
            continue;
        if (game_filter && !game_filter(*game))
            continue;
        sorted_games.push_back(std::move(game));
    }

    // Sort them: Open > Full > Closed
    std::ranges::sort(sorted_games, [](const std::shared_ptr<Game>& a, const std::shared_ptr<Game>& b) {
        // Priority 1: Openness (isOpen && peers < max)
        bool a_open = a->is_open && a->peers.size() < a->max_peers;
        bool b_open = b->is_open && b->peers.size() < b->max_peers;
        if (a_open != b_open)
            return a_open > b_open; // Open comes first

        // Priority 2: Filled status (Not full > Full)
        bool a_full = a->peers.size() >= a->max_peers;
        bool b_full = b->peers.size() >= b->max_peers;
        if (a_full != b_full)
            return b_full > a_full; // Not full comes first

        return a->id < b->id; // Stable fallback
    });

    // Populate final list
    for (const auto& game : sorted_games)
        fres->emplace(game->id, std::make_shared<ser::Hashtable>(game->get_lobby_game_props()));

    return fres;
}

void MasterServerHandler::send_game_list() {
    if (!joined_lobby_)
        return;

    ser::EventMessage event;

    event.event_code = EventCodes::GameList;
    event.parameters[DictKeyCodes::LoadBalancing::GameList] = get_game_list(*joined_lobby_->lobby);

    send(proto_->Serialize(event));
}

MasterServerHandler::JoinedLobby::JoinedLobby(std::shared_ptr<Lobby> lobby_, GameListUpdateHandler&& handler) : lobby(std::move(lobby_)) {
    lobby->game_list_update_handlers.emplace_front(std::move(handler));
    game_list_update_handler = lobby->game_list_update_handlers.begin();
}

MasterServerHandler::JoinedLobby::~JoinedLobby() { lobby->game_list_update_handlers.erase(game_list_update_handler); }
} // namespace server
