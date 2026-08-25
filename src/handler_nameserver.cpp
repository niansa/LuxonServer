// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "handler_nameserver.hpp"
#include "global.hpp"
#include "server_manager.hpp"
#include "authentication.hpp"
#include "peer_persistence.hpp"

#include <luxon/ser_interface.hpp>
#include <luxon/common_codes.hpp>
#include <tracy/Tracy.hpp>

namespace server {
Awaitable<> NameServerHandler::HandleOperationRequest(ser::OperationRequestMessage&& req, bool is_encrypted, const enet::EnetCommandHeader& cmd_header) {
    ZoneScoped;

    if (cmd_header.channel_id != 0)
        lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);

    if (!peer_->is_authenticated()) {
        switch (req.operation_code) {

        case OpCodes::Auth::Authenticate:
        case OpCodes::Auth::AuthenticateOnce: {
            ZoneScopedN("HandleOperationRequest_Authenticate");

            // Try to authenticate
            auto resp = lco_await authenticate(server_manager_, *peer_, req, cmd_header);

            // Add details if authentication was successful
            if (resp.return_code == ErrorCodes::Core::Ok) {
                resp.parameters[DictKeyCodes::LoadBalancing::UserId] = peer_->persistent->user_id;
                resp.parameters[DictKeyCodes::LoadBalancing::Address] =
                    std::string(server_manager_.get_random_server_address(ServerType::MasterServer, peer_->transport_protocol));
            }

            // Send payload
            send(proto_->Serialize(resp, is_encrypted));

            // Disconnect on error
            if (!peer_->is_authenticated())
                peer_->disconnect();

            lco_return;
        }

        case OpCodes::RpcAndMisc::GetRegions: {
            ZoneScopedN("HandleOperationRequest_GetRegions");

            // Build dummy response with all regions unless overidden by config
            std::vector<std::string> regions = {"asia", "au", "cae", "cn", "eu", "hk", "in", "jp", "za", "sa", "kr", "tr", "uae", "us", "usw", "ussc"};
            const std::vector<std::string>& regions_from_yml = server_manager_.get_region_list();

            if( regions_from_yml.empty() == false ) {
                regions = regions_from_yml;
            }

            std::vector<std::string> addresses(regions.size());
            for (size_t i = 0; i < regions.size(); ++i)
                addresses[i] = std::string(server_manager_.get_random_server_address(ServerType::MasterServer, peer_->transport_protocol));

            // Give dummy response
            ser::OperationResponseMessage resp{.operation_code = OpCodes::RpcAndMisc::GetRegions, .return_code = 0};
            resp.parameters[DictKeyCodes::AuthAndLobby::Region] = std::move(regions);
            resp.parameters[DictKeyCodes::LoadBalancing::Address] = std::move(addresses);

            send(proto_->Serialize(resp));
            lco_return;
        }
        }
    }

    lco_return lco_await HandlerBase::HandleOperationRequest(std::move(req), is_encrypted, cmd_header);
}
} // namespace server
