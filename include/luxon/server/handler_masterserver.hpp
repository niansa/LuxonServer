// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "global.hpp"
#include "handler_base.hpp"

#include <unordered_set>
#include <functional>
#include <optional>
#include <commoncpp/timer.hpp>
#include <luxon/ser_types.hpp>

namespace server {
struct Lobby;

class MasterServerHandler : public HandlerBase {
public:
    using HandlerBase::HandlerBase;

    void HandleSlowUpdate() override;
    void HandleOperationRequest(const ser::OperationRequestMessage& req, bool is_encrypted, const enet::EnetCommandHeader& cmd_header) override;

protected:
    struct JoinedLobby {
        std::shared_ptr<Lobby> lobby;
        std::list<GameListUpdateHandler>::iterator game_list_update_handler;

        JoinedLobby(std::shared_ptr<Lobby>, GameListUpdateHandler&&);
        ~JoinedLobby();

        bool operator==(const std::shared_ptr<Lobby>& o) const { return lobby == o; }

        JoinedLobby(const JoinedLobby&) = delete;
        JoinedLobby(JoinedLobby&&) = delete;
        JoinedLobby& operator=(const JoinedLobby&) = delete;
        JoinedLobby& operator=(JoinedLobby&&) = delete;
    };

    std::optional<JoinedLobby> joined_lobby_;
    common::Timer last_app_stats_;
    bool wants_app_stats_ = false;

    std::expected<std::shared_ptr<Lobby>, ser::OperationResponseMessage> get_requested_lobby(const ser::OperationRequestMessage& req);

    void join_lobby(std::shared_ptr<Lobby> lobby);
    void leave_lobby() { joined_lobby_.reset(); }
    std::string_view get_joined_lobby_name() const { return joined_lobby_.has_value() ? joined_lobby_->lobby->name : std::string_view{}; }

    void send_app_stats();
    ser::Dictionary get_lobby_stats(std::function<bool(const Lobby&)> lobby_filter = nullptr);
    void send_lobby_stats();

    ser::HashtablePtr get_game_list(Lobby& lobby, std::function<bool(const Game&)> game_filter = nullptr);
    void send_game_list();
};
} // namespace server
