# Backends, apps, monitoring, and settings

[App overview](README.md)

## Manage inference backends

**Backends** groups runtime variants by capability. The rail filters **All Backends**, **Installed**, **Available**, **Updates**, and **Experimental**. It can also show technical details, unsupported backends, and backend logos.

Use **Install**, **Update**, **Update all**, or **Uninstall** as available. A backend that needs manual setup provides **Setup guide** instead. Installation progress and failures appear on the backend card and in Downloads.

Some installed variants provide a terminal button for persistent, backend-wide CLI arguments. Those defaults are different from a model's **Configuration** and from the session-only Effective settings override.

## Browse applications

**Apps** loads Lemonade's curated marketplace and groups entries by category. Featured apps appear first. Use **Visit** to open an app or its site, **Guide** for setup instructions, and **Video** when one is provided. These actions open external links; The app does not install third-party applications.

The title-bar search searches app names, descriptions, and categories and prioritizes app results while Apps is open.

## Monitor the server

Monitor data describes the connected server, not only this GUI window.

- **Performance** shows server health and uptime, prompt and generation throughput, active inference slots, system CPU/RAM/GPU/VRAM, cache use, last inference details, loaded models, and configured model limits. **Pause** freezes dashboard updates; **Resume** restarts them.
- **Telemetry** captures request traces. Search or filter by request kind or errors, inspect overview and messages, replay a request with changed parameters, compare results, or use **Improve** with an eligible model. Requests can also be created manually, deleted, cleared, or exported to the clipboard as JSON.
- **Logs** streams server output. Filter by text, minimum level, capture level, or source; clear the displayed buffer or reconnect when the stream drops.

Some cards remain empty until a compatible model is loaded or a request has run. Telemetry capture and server logging can include prompt content; review your privacy requirements before enabling or exporting them.

## Configure Settings

Settings is divided by purpose:

- **Server:** endpoint, optional API key, connection status, and local-data cleanup.
- **Chat:** optional browser history, collapsed-reasoning default, default TTS model, and automatic speech playback mode.
- **Memory:** server-owned automatic eviction, eviction threshold, and maximum loaded models per type. Pinned models are protected from eviction.
- **Model storage:** the server's model-cache and external custom-model directories, plus the server-startup model-update check.
- **Cloud providers:** server-owned OpenAI-compatible provider definitions. Provider keys come from `LEMONADE_<PROVIDER>_API_KEY` environment variables or ephemeral server memory and are not written to `config.json`.
- **MCP gateway:** the built-in Lemon-Tools MCP endpoint and discovered tools. The current GUI reports external MCP administration as unavailable while its local client host is being introduced.
- **Help & support:** documentation, release notes, GitHub, and Discord.

**Save settings** applies changes in Chat, Memory, and the model-update controls under Model storage. Server connection and directory changes instead use **Connect** and **Save directories**. In Chat, **Reset defaults** resets the local draft; save it to persist the reset. In Memory and model-update settings, **Discard changes** restores the server's current values.

Theme is a title-bar control and is local to the GUI client. The app does not currently expose a zoom setting in Settings.

## Downloads, progress, and errors

Open the title-bar download icon to see active, paused, completed, cancelled, and failed model or backend downloads. Rows show file progress, bytes, speed, and estimated time when the server supplies them. Expand a row for file details.

Depending on state, a row can be paused, resumed, cancelled, retried, removed from the list, or have its partial files deleted. **Clear completed** removes terminal history rows. Finalization can continue briefly after the byte count reaches 100%.

Model preparation also appears inline in Chat, and model, backend, connection, MCP, and download failures appear near the control that initiated them. Lemonade App does not currently provide a general-purpose Jobs workspace; use the [Job Engine API](../../api/lemonade.md#job-engine-api) for server job sequences.

## Remote servers and multiple clients

A single `lemond` can serve several desktop or browser clients. Server-owned state is shared:

- downloaded and loaded models, pin state, and backend installations;
- persistent **Models > Configuration** values;
- memory, storage, model-update, and cloud-provider configuration; and
- Monitor data and server logs.

Client-owned state is independent:

- endpoint, session API key, layout, theme, and navigation state;
- Favorites, chat history, and Chat display or speech preferences;
- request-time Chat sampling and tool selection; and
- session-only Effective settings backend overrides.

One client's load, unload, delete, configuration, or server Settings action can therefore affect other clients. Its theme, history, Favorites, and chat sampling do not.
