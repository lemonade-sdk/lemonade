# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** C++ Server Core — `lemond`, router, APIs, WebSocket

## Core Context

Owns the HTTP/WebSocket surface. During the UI POC, `lemond` is OFF LIMITS — advisory only.
Key files: `src/cpp/server/{server,router,model_manager,anthropic_api,ollama_api}.cpp`,
`src/cpp/include/lemon/{wrapped_server,server_capabilities,websocket_server}.h`.

Critical invariants to defend: quad-prefix routes, NPU exclusivity, subprocess model,
many-clients-one-server topology, API key passthrough.

## Learnings

### 2026-05-15T18:00:00Z — UI-server contract enumeration

- Verified the quad-prefix invariant by reading `Server::setup_routes` in `src/cpp/server/server.cpp` (lines ~287-300). The `register_get` / `register_post` lambdas register every core endpoint under `/api/v0/`, `/api/v1/`, `/v0/`, `/v1/`. Also: POST endpoints register matching GETs that return 405, so HEAD/method-probe requests don't 404.
- `/live` is a SINGLE registration (not quad-prefixed). Liveness probe only — separate from `/health`.
- `models/{id}` and `slots/{id}` are regex routes registered four times manually (not via the lambda) because httplib regex routes can't share a helper.
- `handle_health` (line 1280) returns `websocket_port` ONLY when the WS server is running. New UI must guard for missing field.
- Ollama routes live under `/api/` with NO version prefix (`OllamaApi::register_routes` in `ollama_api.cpp` line 142). The 501 stubs (`/api/create`, `/api/copy`, `/api/push`, `/api/blobs/.+`) are intentional — keep them; some clients probe.
- Anthropic route is `POST /v1/messages` only (`anthropic_api.cpp` line 152). NOT quad-registered. The endpoint is also separate from the OpenAI chat/completions handler — it converts Anthropic → OpenAI internally.
- The injected `window.api` mock for the web build is in `server.cpp` ~line 611-670. It writes a `<script>` block just before `</head>` and is intentionally a strict subset of the Tauri shim — no clipboard, no resize dragging, no marketplace URL, no nav events. New UI either: (a) keeps the shim contract and gets the mock for free, or (b) builds its own browser adapter calling `fetch` directly. Either works.
- `tauriShim.ts` event channel names mirror constants in `src/app/src-tauri/src/events.rs`. The constants used: `settings-updated`, `connection-settings-updated`, `server-port-updated`, `maximize-change`, `navigate`. Keep them in sync on the Tauri side.
- The `cancelled` flag pattern in `tauriShim.ts` `on()` handles the unmount-before-listen-resolves race. Any new event subscription helper must replicate this — React strict-mode double-mount will leak otherwise.
- CORS is wide open (`*`) with `Authorization` in allowed headers (line 792-797). Necessary for the browser web app. Don't tighten it without coordinating with the UI.
- Settings live in `app_settings.json` for Tauri (read/written via `invoke('get_app_settings')` / `invoke('save_app_settings')`) and `localStorage['lemonade-settings']` for web. There is NO server-side settings endpoint and there must never be one — that's invariant #11.
- Wrote canonical contract doc to `.squad/decisions/inbox/liebergot-ui-server-contract.md` as the source of truth for Mattingly's POC.


### 2026-05-16T20:00:00Z — CLI syntax verification for PR #1914 (bug-report template)

- Verified the canonical CLI surface for setting log level at runtime while reviewing PR #1914:
  - `lemonade config set log_level=debug` — subcommand defined at `src/cpp/cli/main.cpp:1041`. Uses `allow_extras(true)` to accept arbitrary `key=value` tokens, so future runtime keys can be added without CLI changes.
  - Runtime config key registry: `src/cpp/server/runtime_config.cpp:351`. `log_level` is snake_case (NOT `log-level`) — matches `config_file.cpp:187`.
  - Hot-reload path: `reconfigure_application_logging()` in `src/cpp/server/logging_config.cpp:137`. Confirms no server restart is required to change log level — relevant when documenting any runtime-config workflow.
- `lemonade backends` is the canonical inspection command — defined at `src/cpp/cli/main.cpp:1031`. The old name `lemonade recipes` is no longer current (recipes are now a property of backends/models in the registry, not a top-level command).
- Useful reference if I'm asked to advise on CLI docs, `docs/server/` content, or any future runtime-config surface work.