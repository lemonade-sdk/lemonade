# Embeddable Lemonade: Runtime

This guide will show you how to operate Embeddable Lemonade in your app at runtime.

Contents:

- [Launching](#launching)
- [Authenticating Requests](#authenticating-requests)
- [Runtime Model and Backend Management](#runtime-model-and-backend-management)
- [Runtime Settings and Shutdown](#runtime-settings-and-shutdown)

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

See the [Endpoints Spec](../api/README.md) for full request/response details.

## Runtime Settings and Shutdown

Your app can manage its `lemond` instance at runtime by using `/internal` endpoints. See the [Internal API](../api/internal.md) for their request and response formats.

| Endpoint | Description |
|----------|-------------|
| [`POST /internal/set`](../api/internal.md#post-internalset) | Change `config.json` settings without restarting `lemond` |
| [`GET /internal/config`](../api/internal.md#get-internalconfig) | Read the current settings |
| [`GET /internal/config/defaults`](../api/internal.md#get-internalconfigdefaults) | Read the factory default settings |
| [`POST /internal/pin`](../api/internal.md#post-internalpin) | Pin or unpin a loaded model so eviction skips it |
| [`POST /internal/shutdown`](../api/internal.md#post-internalshutdown) | Unload every model and stop `lemond` |

See the [Configuration Guide](../guide/configuration/README.md) for every setting.
