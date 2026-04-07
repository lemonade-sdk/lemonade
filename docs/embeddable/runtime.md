# Embeddable Lemonade: Runtime

This guide will show you how to operate Embeddable Lemonade in your app at runtime.

Contents:

- [Launching](#launching)
- [Authenticating Requests](#authenticating-requests)
- [Runtime Model and Backend Management](#runtime-model-and-backend-management)
- [Runtime Settings Management](#runtime-settings-management)
  - [`GET /internal/config`](#get-internalconfig)
  - [`POST /internal/set`](#post-internalset)

## Launching

We recommend that your app launches `lemond` as a subprocess, using a command like this:

=== "Windows (cmd.exe)"

    ```cmd
    set LEMONADE_API_KEY=KEY && lemond.exe ./ --port PORT
    ```

=== "Linux (bash)"

    ```bash
    LEMONADE_API_KEY=KEY lemond ./ --port PORT
    ```

Breaking this down:
- `LEMONADE_API_KEY=KEY` sets an API key for `lemond` known only to your app. This locks out other apps, as well as users, from interfacing directly with `lemond`'s endpoints.
- The positional `./` is the working directory for `lemond`, where it will look for `config.json`, `bin/`, etc.
- `--port PORT` ensures that `lemond` launches on a specific port where your app will find it.

## Authenticating Requests

If you launch `lemond` with `LEMONADE_API_KEY` set, your app must send that same key on every HTTP request to Lemonade endpoints. Do this by setting an `Authorization` header with a Bearer token:

```http
Authorization: Bearer KEY
```

For example, with `curl`:

=== "Windows (cmd.exe)"

    ```cmd
    curl http://localhost:8000/v1/health ^
      -H "Authorization: Bearer KEY"
    ```

=== "Linux (bash)"

    ```bash
    curl http://localhost:8000/v1/health \
      -H "Authorization: Bearer KEY"
    ```

For JSON `POST` requests:

=== "Windows (cmd.exe)"

    ```cmd
    curl -X POST http://localhost:8000/internal/set ^
      -H "Authorization: Bearer KEY" ^
      -H "Content-Type: application/json" ^
      -d "{\"log_level\": \"debug\"}"
    ```

=== "Linux (bash)"

    ```bash
    curl -X POST http://localhost:8000/internal/set \
      -H "Authorization: Bearer KEY" \
      -H "Content-Type: application/json" \
      -d '{"log_level": "debug"}'
    ```

In JavaScript:

```js
await fetch("http://localhost:8000/v1/models", {
  headers: {
    Authorization: `Bearer ${apiKey}`,
  },
});
```

This matches the existing CLI, tray, app, and test implementations in this repo. If the header is missing or the key is wrong, `lemond` will reject the request with `401 Unauthorized`.

## Runtime Model and Backend Management

`lemond` provides a full set of endpoints for managing models and backends at runtime.

| Endpoint | Description |
|----------|-------------|
| `POST /v1/pull` | Download a model |
| `POST /v1/delete` | Delete a downloaded model |
| `POST /v1/load` | Load a model into memory |
| `POST /v1/unload` | Unload a model from memory |
| `POST /v1/install` | Install or update a backend |
| `POST /v1/uninstall` | Remove a backend |
| `GET /v1/models` | List available models |
| `GET /v1/health` | Server status and loaded models |

See the [Server Spec](../server/server_spec.md) for full request/response details.

## Runtime Settings Management

Your app can manage its `lemond` instance at runtime by using `/internal` endpoints.

| Method | Path | Description |
|--------|------|-------------|
| `POST` | `/internal/set` | Unified config setter (see below) |
| `GET`  | `/internal/config` | Returns the full runtime config snapshot |

The settings defined in `config.json` can all be changed at runtime without restarting `lemond` with the `/internal/set` endpoint. See the [Configuration Guide](../server/configuration.md) for details on all settings.

> Note: The `lemonade` CLI uses `/internal/set` and `/internal/config` internally for the `lemonade config` commands.

#### `GET /internal/config`

Returns the full runtime configuration as a flat JSON object containing all server-level and recipe option keys with their current values.

**Example:**
=== "Windows (cmd.exe)"

    ```cmd
    curl http://localhost:8000/internal/config
    ```

=== "Linux (bash)"

    ```bash
    curl http://localhost:8000/internal/config
    ```

#### `POST /internal/set`

Accepts a JSON object with one or more keys to update atomically. Returns `{"status":"success","updated":{...}}` on success, or `400` with an error message on validation failure.

**Server-level keys** (trigger immediate side effects):

| Key | Type | Side Effect |
|-----|------|-------------|
| `port` | int (1–65535) | HTTP rebind |
| `host` | string | HTTP rebind |
| `log_level` | string (`trace`, `debug`, `info`, `warning`, `error`, `fatal`, `none`) | Reconfigures log filter |
| `global_timeout` | int (positive) | Updates default HTTP client timeout |
| `no_broadcast` | bool | Stops or starts UDP beacon |
| `extra_models_dir` | string | Updates model manager search path |

**Deferred keys** (affect the next model load or eviction decision, no immediate side effect):

| Key | Type |
|-----|------|
| `max_loaded_models` | int (-1 or positive) |
| `ctx_size` | int (positive) |
| `llamacpp_backend` | string |
| `llamacpp_args` | string |
| `sdcpp_backend` | string |
| `whispercpp_backend` | string |
| `whispercpp_args` | string |
| `steps` | int (positive) |
| `cfg_scale` | number |
| `width` | int (positive) |
| `height` | int (positive) |
| `flm_args` | string |

**Example:**
=== "Windows (cmd.exe)"

    ```cmd
    curl -X POST http://localhost:8000/internal/set ^
      -H "Content-Type: application/json" ^
      -d "{\"ctx_size\": 8192, \"max_loaded_models\": 3, \"log_level\": \"debug\"}"
    ```

=== "Linux (bash)"

    ```bash
    curl -X POST http://localhost:8000/internal/set \
      -H "Content-Type: application/json" \
      -d '{"ctx_size": 8192, "max_loaded_models": 3, "log_level": "debug"}'
    ```
