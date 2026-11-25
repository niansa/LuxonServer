# Database Documentation

This document outlines the SQLite database schema used by the Luxon Server settings manager.
The database primarily stores configuration details for applications and their supported authentication methods.

Point the `SettingsDatabase` configuration key to a database file to use this.
If that file doesn't exist yet, it will be created automatically.
If it already exists but is outdated, it will be migrated to the latest version automatically.

## Tables

### 1. `app_settings`
This table stores the core configuration settings for each application managed by the server.

* **`appid`** (TEXT): The unique identifier for the application. This serves as the Primary Key.
* **`auth_mode`** (INTEGER): Defines how strict the server is when authenticating users for this app. It defaults to `1`.
    * `0` = Weak: Any authentication method will always succeed, no matter what
    * `1` = Anonymous: Anonymous authentication method will always succeed, no matter what
    * `2` = Strict: Anonymous authentication will be disabled
* **`allow_find_friends`** (INTEGER): A boolean-like flag where `1` means `FindFriends` is allowed and `0` means it is disabled. It defaults to `1`.
* **`allow_change_master`** (INTEGER): A boolean-like flag where `1` means setting `MasterClientId` is allowed and `0` means it is disabled. It defaults to `1`.
* **`custom_anonymous_uid_mode`** (INTEGER): Controls how user IDs (UIDs) are generated for anonymous users. It defaults to `2`.
    * `0` = Allow: Allows anonymous users to have any user ID
    * `1` = AllowWithPrefix: Allows anonymous users to have any user ID, but the `anonymous_uid_prefix` will be prepended
    * `2` = ForceRandom: Forces anonymous users to have a random user ID
* **`anonymous_uid_prefix`** (TEXT): The prefix text applied to generated anonymous user IDs (see above). It defaults to `'anon-'`.
* **`max_peers`** (INTEGER): The maximum number of concurrent users (peers) allowed across the entire application. It defaults to `0` (infinite).
* **`max_peers_per_game`** (INTEGER): The maximum number of users allowed within a single game session. It defaults to `0` (255).
* **`max_game_count`** (INTEGER): The maximum number of active game sessions allowed at one time. It defaults to `0` (infinite).
* **`max_room_footprint_bytes`** (INTEGER): The maximum allowed footprint in bytes for a single room. `NULL` (default) inherits the global `MaxRoomFootprintBytes` setting, and `0` disables the limit (infinite).
* **`max_room_footprint_complexity`** (INTEGER): The maximum allowed footprint complexity for a single room. `NULL` (default) inherits the global `MaxRoomFootprintComplexity` setting, and `0` disables the limit (infinite).
* **`max_room_footprint_exceed_action`** (INTEGER): Defines the action taken when a room exceeds its footprint limits. `NULL` (default) or `0` inherits the global `MaxRoomFootprintExceedKill` configuration.
    * `0` = Default: Uses the global configuration
    * `1` = CloseRoom: Closes the room
    * `2` = KillRoom: Kills the room

### 2. `auth_providers`
This table configures third-party or custom authentication providers linked to specific applications.

* **`appid`** (TEXT): The application identifier this provider belongs to. This acts as both a part of the Primary Key and a Foreign Key referencing `app_settings(appid)`. If the parent app is deleted, these records cascade and are deleted automatically.
* **`auth_type`** (INTEGER): A numeric code representing the specific type of authentication provider. This forms the second part of the Primary Key alongside `appid`.
* **`is_allowed`** (INTEGER): A boolean-like flag indicating if this authentication method is currently active or permitted (`1` for true, `0` for false). It defaults to `1`.
* **`secret`** (TEXT): An optional secret key or token required to communicate securely with the authentication provider.
* **`auth_url`** (TEXT): An optional web address (URL) used to contact the external authentication provider.

I know of the following `auth_type` values:

* `1`: Steam
* `2`: Facebook
* `3`: Oculus
* `10`: Viveport
* `12`: PlayStation

You can usually figure out the authentication type used by a game by looking at the `217` (`AuthAndLobby::ClientAuthenticationType`) parameter of the `230` (`Auth::Authenticate`) operation request it sends.

### 3. `schema_migrations`
This is an internal tracking table used by the application to manage automated database updates over time. DO NOT touch this.

* **`version`** (INTEGER): The version number of the database schema that has been successfully applied. This is the Primary Key.
