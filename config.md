# Configuration Documentation

## `Servers`

Defines the server endpoints to run, grouped by server type: `NameServer`, `MasterServer`, or `GameServer`. Each type holds a list of endpoint entries with the following keys:

* **`port`** (integer, required): Local port the server listens on.
* **`external_address`** (string, optional): Externally reachable UDP `host:port` advertised to clients.
* **`allow_unsolicited`** (boolean, default `false`): Allows unsolicited/unauthenticated connections on this GameServer.
* **`subprocess`** (boolean, default `false`): Runs this GameServer endpoint in a subprocess. *(Requires `LUXON_SERVER_ENABLE_MULTIPROCESSING`.)*
* **`stun_server`** (string or map, optional): STUN server used for NAT traversal. Can be a hostname string (uses default port `19302`) or a map with:
  * **`host`** (string, required): STUN server hostname.
  * **`port`** (integer, default `19302`): STUN server port.
* **`proxies`** (list, optional): Alternative external endpoints advertised to clients for non-UDP protocols. Each entry supports:
  * **`protocol`** (string, default `TCP`): Proxy protocol (`UDP`, `TCP`, or `WebSocket`).
  * **`address`** (string, required): Externally reachable `host:port` of the proxy.

## General Settings

* **`EnableIPv6`** (boolean, default `true`): Enables IPv6 support.
* **`NoBanner`** (boolean, default `false`): Suppresses startup license banner.
* **`MaxConnections`** (integer, default `0`): Maximum number of concurrent connections allowed (`0` = unlimited).
* **`MaxGamePeers`** (integer, default `0`): Global default maximum peers per game (`0` or `>= 255` = unlimited).
* **`TickTimeBudget`** (integer, default `2000`): Maximum time in milliseconds spent servicing peers per main loop tick (`0` = unlimited). Lower values reduce starvation across many clients at the cost of higher processing latency.

## Room Footprint Limits

Global defaults for per-room memory and complexity limits. Individual apps can override these in the settings database (including setting `0` to explicitly disable a limit).

* **`MaxRoomFootprintBytes`** (integer, default `750000`): Maximum allowed footprint in bytes for a single room (`0` = unlimited).
* **`MaxRoomFootprintComplexity`** (integer, default `75000`): Maximum allowed footprint complexity for a single room (`0` = unlimited).
* **`MaxRoomFootprintExceedKill`** (boolean, default `false`): Controls the default action when a room exceeds its footprint limits (`false`: room is closed; `true`: room is killed).

## `Regions`

* **`Regions`** (list of strings, optional): List of regions exposed to clients. If omitted, all built-in regions are exposed.
  * **Friendly names:** `europe`, `australia`, `canada-east`, `china`, `hong-kong`, `india`, `japan`, `south-africa`, `south-america`, `south-korea`, `turkey`, `us-west`, `us-southcentral`
  * **Internal codes:** `asia`, `au`, `cae`, `cn`, `eu`, `hk`, `in`, `jp`, `za`, `sa`, `kr`, `tr`, `uae`, `us`, `usw`, `ussc`

## `SettingsDatabase`

*Requires `LUXON_SERVER_ENABLE_SETTINGS_DATABASE`, ignored if not compiled in.*

* **`SettingsDatabase`** (string, optional): Path to the SQLite database storing per-app settings and auth providers. Created and migrated automatically if writable.
  * Supports optional colon-separated flags appended to the path: `:ro`, `:rw`, `:nocreate`, `:nomutex`, `:fullmutex`, `:sharedcache`, `:privatecache`, `:memory`.

See [settings_database.md](settings_database.md) for more info.

## `HTTP`

*Requires `LUXON_SERVER_ENABLE_WEBSERVER`, ignored if not compiled in.*

Configures the embedded web interface and HTTP API:

* **`enabled`** (boolean, default `false`): Enables the embedded HTTP server.
* **`address`** (string, default `"0.0.0.0"`): IP address to bind the HTTP server to.
* **`port`** (integer, default `5088`): Port to bind the HTTP server to.
