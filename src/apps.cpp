// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "apps.hpp"
#include "lobby.hpp"
#include "game.hpp"
#include "peer_persistence.hpp"
#include "server_manager.hpp"
#include "hookpoints.hpp"
#include "string_hash.hpp"

#include <unordered_map>
#include <utility>
#include <tracy/Tracy.hpp>
#include <luxon/common_codes.hpp>

namespace server {
void AppInfo::encode_app_info(ser::ParameterList& params) const {
    if (has_app_info()) {
        params[DictKeyCodes::LoadBalancing::ApplicationId] = std::string(id);
        params[DictKeyCodes::LoadBalancing::AppVersion] = std::string(version);
    }
}

void LobbyInfo::encode_lobby_info(ser::ParameterList& params) const {
    if (has_lobby_info()) {
        params[DictKeyCodes::AuthAndLobby::LobbyName] = std::string(name);
        params[DictKeyCodes::AuthAndLobby::LobbyType] = type;
    }
    app.encode_app_info(params);
}

size_t LobbyIdHash::operator()(const LobbyId& k) const noexcept {
    std::size_t h1 = std::hash<std::string_view>{}(k.first);
    std::size_t h2 = std::hash<unsigned>{}(k.second); // avoid uint8_t quirks
    // hash combine
    return h1 ^ (h2 + 0x9e3779b97f4a7c15ull + (h1 << 6) + (h1 >> 2));
}

void AppSettings::enforce_global_config(ServerManager& server_manager) {
    const auto apply_default = [](unsigned& value, unsigned global_default, unsigned sentinel = 0) {
        if (value == sentinel)
            value = global_default;
    };

    apply_default(max_peers_per_game, server_manager.get_max_game_peers());
    apply_default(max_room_footprint_bytes, server_manager.get_max_room_footprint_bytes(), InheritGlobal);
    apply_default(max_room_footprint_complexity, server_manager.get_max_room_footprint_complexity(), InheritGlobal);
    apply_default(max_room_footprint_exceed_action,
                  server_manager.get_max_room_footprint_exceed_kill() ? KillRoom : CloseRoom, Default);
}

App::App(ServerManager& server_manager, std::string_view id, std::string_view version) : server_manager(server_manager), id(id), version(version) {}

bool App::load_app_settings() {
    // Get settings
    const bool fres = [this]() -> bool {
        // Try hookpoint
        bool hookpoint_success = true;
        LUXON_SERVER_HOOKPOINT_CSM_SYNC(server_manager, App_load_app_settings, settings_, hookpoint_success) hookpoint_success;

        // Try database
#ifdef LUXON_SERVER_ENABLE_SETTINGS_DATABASE
        if (server_manager.settings_manager) {
            if (const auto settings = server_manager.settings_manager->get_app_settings(std::string(id)))
                settings_ = *settings;
            else
                return false;
        }

#endif
        return true;
    }();

    // Enforce globally configured caps
    if (fres)
        settings_.enforce_global_config(server_manager);

    return fres;
}

size_t App::get_game_count() const {
    size_t fres = 0;

    for (const auto& [name, weak_lobby] : lobbies_)
        if (auto lobby = weak_lobby.lock())
            fres += lobby->games.size();

    return fres;
}

size_t App::get_peer_count() const {
    size_t fres = 0;
    for (const auto& pp : server_manager.peer_persistent_data)
        fres += !!(pp->app.get() == this);
    for (const auto& [lobby_name, weak_lobby] : lobbies_)
        if (auto lobby = weak_lobby.lock())
            fres += lobby->get_peer_count();
    return fres;
}

void App::add_app_info(ser::ParameterList& params) const {
    params[DictKeyCodes::LoadBalancing::ApplicationId] = std::string(id);
    params[DictKeyCodes::LoadBalancing::AppVersion] = std::string(version);
}

AppInfo App::get_app_info() const {
    AppInfo fres;
    fres.id = id;
    fres.version = version;
    return fres;
}

std::shared_ptr<Lobby> App::get_lobby(LobbyId id) {
    ZoneScoped;

    // Try to find lobby first
    if (auto res = lobbies_.find(id); res != lobbies_.end())
        if (auto lobby = res->second.lock())
            return lobby;

    // Create lobby
    std::shared_ptr<Lobby> fres(new Lobby(get_shared(), std::string(id.first), id.second), [this](Lobby *ptr) {
        lobbies_.erase(LobbyId{ptr->name, ptr->type});
        delete ptr;
    });
    lobbies_[{fres->name, fres->type}] = fres;
    return fres;
}

std::shared_ptr<App> App::try_get(ServerManager& server_manager, const std::string& id, const std::string& version) {
    ZoneScoped;
    if (auto res = server_manager.apps.find({id, version}); res != server_manager.apps.end()) {
        if (auto fres = res->second.lock())
            return fres;
        else
            server_manager.apps.erase(res);
    }

    return nullptr;
}

std::shared_ptr<App> App::get(ServerManager& server_manager, const std::string& id, const std::string& version) {
    ZoneScoped;

    if (auto res = server_manager.apps.find({id, version}); res != server_manager.apps.end()) {
        if (auto fres = res->second.lock())
            return fres;
        else
            server_manager.apps.erase(res);
    }

    auto res = server_manager.apps.emplace(std::pair<std::string, std::string>(id, version), std::weak_ptr<App>());
    const auto& [allocated_id, allocated_version] = res.first->first;
    std::shared_ptr<App> fres(new App(server_manager, allocated_id, allocated_version), [&server_manager](App *ptr) {
        if (auto it = server_manager.apps.find({std::string(ptr->id), std::string(ptr->version)}); it != server_manager.apps.end())
            server_manager.apps.erase(it);
        delete ptr;
    });

    if (!fres)
        return nullptr;

    if (!fres->load_app_settings())
        return nullptr;

    res.first->second = fres;
    return fres;
}

std::vector<std::shared_ptr<App>> App::get_all(ServerManager& server_manager) {
    ZoneScoped;

    std::vector<std::shared_ptr<App>> fres;
    for (auto& [_, weak_app] : server_manager.apps)
        if (auto app = weak_app.lock())
            fres.emplace_back(std::move(app));
    return fres;
}

AppInfo App::decode_app_info(const ser::ParameterList& params) {
    AppInfo fres;
    for (const auto& [key, val] : params) {
        if (key == DictKeyCodes::LoadBalancing::ApplicationId)
            fres.id = val.get<std::string>();
        if (key == DictKeyCodes::LoadBalancing::AppVersion)
            fres.version = val.get<std::string>();
    }
    return fres;
}
} // namespace server
