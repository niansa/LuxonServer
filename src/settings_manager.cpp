// Copyright (c) 2026, the Luxon Server contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "settings_manager.hpp"

#include <vector>
#include <algorithm>
#include <system_error>
#include <luxon/flat_map.hpp>

namespace server {
namespace {
struct Migration {
    int version;
    std::string sql;
};

// List of automatic migrations, future schema updates should be appended here
const std::vector<Migration> MIGRATIONS = {
    // Migration 1
    {1,
     R"(
        CREATE TABLE app_settings (
            appid TEXT PRIMARY KEY,
            auth_mode INTEGER NOT NULL DEFAULT 1, -- 0=Weak, 1=Anonymous, 2=Strict
            allow_find_friends INTEGER NOT NULL DEFAULT 1,
            custom_anonymous_uid_mode INTEGER NOT NULL DEFAULT 2, -- 0=Allow 1=AllowWithPrefix 2=ForceRandom
            anonymous_uid_prefix TEXT NOT NULL DEFAULT 'anon-',
            max_peers INTEGER NOT NULL DEFAULT 0,
            max_peers_per_game INTEGER NOT NULL DEFAULT 0,
            max_game_count INTEGER NOT NULL DEFAULT 0
        );

        CREATE TABLE auth_providers (
            appid TEXT NOT NULL,
            auth_type INTEGER NOT NULL,
            is_allowed INTEGER NOT NULL DEFAULT 1,
            secret TEXT,
            auth_url TEXT,
            PRIMARY KEY (appid, auth_type),
            FOREIGN KEY (appid) REFERENCES app_settings(appid) ON DELETE CASCADE
        );

        CREATE INDEX idx_app_settings_appid ON app_settings(appid);
        CREATE INDEX idx_auth_providers_appid_type ON auth_providers(appid, auth_type);
        )"},

    // Migration 2
    {2, R"(
        ALTER TABLE app_settings ADD COLUMN allow_change_master INTEGER NOT NULL DEFAULT 1;
    )"},

    // Migration 3
    {3, R"(
        ALTER TABLE app_settings ADD COLUMN max_room_footprint_bytes INTEGER;
        ALTER TABLE app_settings ADD COLUMN max_room_footprint_complexity INTEGER;
        ALTER TABLE app_settings ADD COLUMN max_room_footprint_exceed_action INTEGER;
    )"}};
} // anonymous namespace

SettingsManager::SettingsManager(const std::filesystem::path& db_path) {
    std::string path_str = db_path.string();

    // Set initial state
    is_read_only_ = !check_is_writable(db_path);
    int flags = is_read_only_ ? SQLITE_OPEN_READONLY : (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);

    // Define suffix handlers
    const flat_map<std::string, std::function<void()>> suffix_handlers = {{"ro",
                                                                           [&]() {
                                                                               flags &= ~(SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
                                                                               flags |= SQLITE_OPEN_READONLY;
                                                                               is_read_only_ = true;
                                                                           }},
                                                                          {"rw",
                                                                           [&]() {
                                                                               flags &= ~SQLITE_OPEN_READONLY;
                                                                               flags |= (SQLITE_OPEN_READWRITE | SQLITE_OPEN_CREATE);
                                                                               is_read_only_ = false;
                                                                           }},
                                                                          {"nocreate", [&]() { flags &= ~SQLITE_OPEN_CREATE; }},
                                                                          {"nomutex",
                                                                           [&]() {
                                                                               flags &= ~SQLITE_OPEN_FULLMUTEX;
                                                                               flags |= SQLITE_OPEN_NOMUTEX;
                                                                           }},
                                                                          {"fullmutex",
                                                                           [&]() {
                                                                               flags &= ~SQLITE_OPEN_NOMUTEX;
                                                                               flags |= SQLITE_OPEN_FULLMUTEX;
                                                                           }},
                                                                          {"sharedcache",
                                                                           [&]() {
                                                                               flags &= ~SQLITE_OPEN_PRIVATECACHE;
                                                                               flags |= SQLITE_OPEN_SHAREDCACHE;
                                                                           }},
                                                                          {"privatecache",
                                                                           [&]() {
                                                                               flags &= ~SQLITE_OPEN_SHAREDCACHE;
                                                                               flags |= SQLITE_OPEN_PRIVATECACHE;
                                                                           }},
                                                                          {"memory", [&]() { flags |= SQLITE_OPEN_MEMORY; }}};

    // Parse and strip suffixes
    std::vector<std::function<void()>> actions_to_apply;

    while (true) {
        size_t last_colon = path_str.find_last_of(':');
        if (last_colon == std::string::npos)
            break;

        std::string suffix = path_str.substr(last_colon + 1);
        auto it = suffix_handlers.find(suffix);

        if (it != suffix_handlers.end()) {
            // Store lambda to apply later
            actions_to_apply.push_back(it->second);
            // Strip recognized suffix from path string
            path_str.erase(last_colon);
        } else {
            break;
        }
    }

    // Apply suffixes from left to right
    for (auto it = actions_to_apply.rbegin(); it != actions_to_apply.rend(); ++it)
        (*it)();

    // Create cleaned path
    std::filesystem::path clean_db_path(path_str);

    // Initialize wrapper
    db_ = std::make_unique<sqlite3pp::database>(clean_db_path.string().c_str(), flags);

    if (!is_read_only_) {
        // Enforce foreign key constraints
        db_->execute("PRAGMA foreign_keys = ON;");
        apply_migrations();
    }
}

bool SettingsManager::check_is_writable(const std::filesystem::path& db_path) const {
    std::error_code ec;

    if (std::filesystem::exists(db_path, ec)) {
        auto perms = std::filesystem::status(db_path, ec).permissions();
        return (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
    } else {
        // File doesn't exist, check if directory can be written to for database creation
        auto parent = db_path.parent_path();
        if (parent.empty())
            parent = std::filesystem::current_path();

        auto perms = std::filesystem::status(parent, ec).permissions();
        return (perms & std::filesystem::perms::owner_write) != std::filesystem::perms::none;
    }
}

void SettingsManager::apply_migrations() {
    sqlite3pp::transaction xact(*db_, true); // Immediate transaction

    // Ensure migration tracking table exists
    db_->execute("CREATE TABLE IF NOT EXISTS schema_migrations (version INTEGER PRIMARY KEY);");

    int current_version = 0;
    sqlite3pp::query q(*db_, "SELECT MAX(version) FROM schema_migrations");
    if (auto it = q.begin(); it != q.end())
        current_version = (*it).get<int>(0);

    for (const auto& migration : MIGRATIONS) {
        if (migration.version > current_version) {
            db_->execute(migration.sql.c_str());

            sqlite3pp::command cmd(*db_, "INSERT INTO schema_migrations (version) VALUES (?)");
            cmd.bind(1, migration.version);
            cmd.execute();
        }
    }

    xact.commit();
}

std::optional<AppSettings> SettingsManager::get_app_settings(const std::string& appid) {
    sqlite3pp::query q(*db_, "SELECT auth_mode, allow_find_friends, "
                             "custom_anonymous_uid_mode, "
                             "anonymous_uid_prefix, max_peers, "
                             "max_peers_per_game, max_game_count, "
                             "allow_change_master, "
                             "max_room_footprint_bytes, "
                             "max_room_footprint_complexity, "
                             "max_room_footprint_exceed_action "
                             "FROM app_settings WHERE appid = ?");
    q.bind(1, appid.c_str(), sqlite3pp::copy_semantic::nocopy);

    auto it = q.begin();
    if (it == q.end()) {
        if (appid == "unknown")
            return std::nullopt;
        return get_app_settings("unknown");
    }

    AppSettings settings;
    settings.appid = appid;

    settings.auth_mode = std::clamp((*it).get<int>(0), 0, 2);
    settings.allow_find_friends = (*it).get<int>(1) != 0;
    settings.custom_anonymous_uid_mode = std::clamp((*it).get<int>(2), 0, 2);
    settings.anonymous_uid_prefix = (*it).get<std::string>(3);
    settings.max_peers = (*it).get<int>(4);
    settings.max_peers_per_game = (*it).get<int>(5);
    settings.max_game_count = (*it).get<int>(6);
    settings.allow_change_master = (*it).get<int>(7) != 0;
    if ((*it).column_type(8) != SQLITE_NULL)
        settings.max_room_footprint_bytes = std::max((*it).get<int>(8), 0);
    if ((*it).column_type(9) != SQLITE_NULL)
        settings.max_room_footprint_complexity = std::max((*it).get<int>(9), 0);
    if ((*it).column_type(10) != SQLITE_NULL)
        settings.max_room_footprint_exceed_action = std::clamp((*it).get<int>(10), 0, 2);

    return settings;
}

std::optional<AuthProviderSettings> SettingsManager::get_auth_provider(const std::string& appid, uint8_t auth_type) {
    sqlite3pp::query q(*db_, "SELECT is_allowed, secret, auth_url "
                             "FROM auth_providers WHERE appid = ? AND auth_type = ?");
    q.bind(1, appid.c_str(), sqlite3pp::copy_semantic::nocopy);
    q.bind(2, auth_type);

    auto it = q.begin();
    if (it == q.end()) {
        if (appid == "unknown")
            return std::nullopt;
        return get_auth_provider("unknown", auth_type);
    }

    AuthProviderSettings auth;
    auth.appid = appid;
    auth.auth_type = auth_type;
    auth.is_allowed = (*it).get<int>(0) != 0;

    if (const char *secret_val = (*it).get<const char *>(1))
        auth.secret = secret_val;

    if (const char *url_val = (*it).get<const char *>(2))
        auth.auth_url = url_val;

    return auth;
}
} // namespace server
