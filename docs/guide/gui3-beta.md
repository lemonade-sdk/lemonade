# GUI3 Beta app

GUI3 is Lemonade's graphical client for finding models, configuring inference,
and using a Lemonade Server. The desktop and browser builds share the same
React interface. The server owns model files, loaded models, backends, and
persistent load configuration; each GUI client owns its connection, layout,
chat history, and chat preferences.

This guide describes the GUI3 beta experience. To build GUI3 from source, start
with the [developer app guide](../dev/app.md#building).

![GUI3 Chat workspace](https://raw.githubusercontent.com/lemonade-sdk/assets/39bdd476eced437d135a971d893a99f917117a64/docs/guides/gui3-guide/01-chat.png)

*These screenshots use the current GUI3 renderer. Numbered yellow callouts
identify the controls described in the text.*

The Chat view shows the workspace navigation (**1**), selected model (**2**),
and **Effective settings** button (**3**).

## Build GUI3 Beta from source

On Windows, clone the current GUI3 beta branch, configure the repository, and
build the Tauri desktop app:

```powershell
git clone --branch kpoineal-sync-main-backend https://github.com/lemonade-sdk/lemonade.git
cd lemonade
.\setup.ps1
cmake --build --preset windows --target tauri-app --parallel 4
```

The build requires Node.js 20 or later, Rust/Cargo, Visual Studio with CMake,
and WebView2. The executable is written to
`build\app\lemonade-app.exe`; start `lemond.exe` separately before launching
the desktop app.

## Open the app

When building from source, configure and build the repository first. Start the
built Lemonade Server, then launch GUI3 or open the browser build at the
server's URL.

- **Windows source build:** start the built `lemond.exe`, then launch the
  built `lemonade-app.exe`. An installed build can instead be opened from the
  Lemonade tray menu; its server continues running when the app closes.
- **macOS source build:** start the built `lemond`, then launch the built
  `lemonade-app.app`. An installed build can instead be opened from
  Applications.
- **Linux source build or browser:** start the built `lemond`, then open
  `http://127.0.0.1:13305`. The GUI is served at `/app`; `/web-app` remains
  available as a backward-compatible URL.

The desktop app uses a saved endpoint when one exists and can discover a local
server through Lemonade's UDP beacon. For a browser build served by `lemond`,
use the server origin.

The status dot beside the Lemonade name is green when the configured server is
reachable. If it is gray, open **Settings > Server** and check the endpoint and
API key.

## Navigate GUI3

The title bar is available from every workspace:

| Control | Purpose |
| --- | --- |
| **Chat** | Converse with chat models and use supported media models. |
| **Models** | Search, download, load, inspect, and configure models. |
| **Backends** | Install and update inference runtimes. |
| **Apps** | Browse applications that can use Lemonade. |
| **Monitor** | Inspect performance, request telemetry, and server logs. |
| **Settings** | Configure the connection, client preferences, and server settings. |
| Search | Search pages, models, backends, apps, Settings sections, and Monitor sections. |
| Theme | Switch between the light and dark themes. |
| Downloads | Open the model and backend download manager. A badge shows active downloads. |

Select Search or press `Ctrl+K` (`Cmd+K` on macOS). Use the arrow keys to move
through results, `Enter` to open one, and `Escape` to close the results.
Selecting a model opens its Models detail panel; other results open the
relevant workspace or section.

Most workspaces have a collapsible left rail. On a narrow window it becomes a
menu or bottom sheet.

## Connect to a server

Open **Settings > Server**, enter the full `http://` or `https://` server URL,
optionally enter an API key, and select **Connect**. Connection errors include
the endpoint that failed.

The endpoint and API key belong to this GUI client, not to `lemond`. The
desktop host can persist an API key only when **Remember API key** is selected.
When the host cannot store it, the key remains session-only. **Clear permitted
local data** clears this client's browser data and saved connection settings;
it does not delete server models or configuration.

For a remote server, prefer HTTPS and configure the server's allowed browser
origins when using the web build.

## Find and prepare models

Open **Models**. The left rail provides:

- **All Models**, **Downloaded**, and client-local **Favorites**;
- task filters for **Chat**, **Omni**, **Router**, **Image**, **Audio**,
  **Music & SFX**, **TTS**, **3D**, **Embed**, and **Classify**;
- backend and model-family tag filters;
- **Hugging Face** and **ModelScope** switches for online search; and
- a model-storage meter.

The middle pane searches the local catalog and, when enabled, online
catalogs. It can sort by name, size, last use, or download count and groups
results under **Pinned**, **Downloaded**, and **Local Catalog** as applicable.
Favorites are local to this client. Pinning is a server-side property for a
loaded model and protects it from automatic eviction.

Select a model to open its detail panel. The header shows its state, recipe,
capability, size, and source when available. The tabs provide:

- **Configuration:** settings for the next load or reload;
- **README:** model documentation; and
- **Files:** the files reported for the model.

For a model that is not downloaded, select **Get & Load** to download and load
it, or **Download** to keep it on disk. For a downloaded model, select
**Load**. A running model offers **Unload** and, when eligible, **Pin**.
**Favorite** is client-local. **Delete** removes downloaded files or a custom
model definition after confirmation.

Download and load errors appear in the detail panel. Download progress also
appears in the title-bar download manager.

### Configure a model before it loads

Select the model's **Configuration** tab (**1**). **Load settings** (**2**) can
include context size and automatic context tuning, backend and device, typed
load-time sampling fields, and additional backend CLI arguments. The controls
come from the selected recipe, so not every model shows every field.

![A selected model's Configuration tab, with callouts for the tab, load settings, and Save](https://raw.githubusercontent.com/lemonade-sdk/assets/39bdd476eced437d135a971d893a99f917117a64/docs/guides/gui3-guide/02-models.png)

The values shown in the form are sent by **Load** or **Get & Load**, even if
they have not been saved. **Save** is callout **3**; it and the other actions
at the bottom have distinct effects:

- **Reload model** appears only when the model is running and the shown values
  differ from its running values. It restarts the model with the shown values
  but does not save them.
- **Save** writes changed values to the connected server for future loads and
  reloads. Other clients connected to that server can observe them.
- **Discard changes** appears when the form differs from the saved settings. It
  restores those saved values without reloading.
- **Reset to defaults** fills the form with the defaults reported by `lemond`.
  It does not write or reload anything. **Load** or **Reload model** uses those
  defaults once; **Save** clears the corresponding saved overrides.

These are persistent, pre-load model settings. They are not the same as the
request-time controls in Chat.

### Register custom models, collections, and routers

Select **Open custom models** in the Models toolbar to register a checkpoint
that is not in the catalog. Choose the capability and a compatible recipe,
enter the checkpoint and any required companion artifacts, then save. The
server registers it with a `user.<name>` identifier.

The custom-model editor also creates an **Omni Collection**. Choose its planner
and any compatible vision, image, transcription, or speech components. A
collection remains editable after it is saved or downloaded, but is omitted
from the Chat picker if a required component is no longer registered.

Use the separate **Open router editor** toolbar action to build a Router across
registered chat models. The custom-model editor's **Import** and **Export**
actions move custom model and Omni definitions as JSON; the Router editor has
its own import flow.

## Use Chat

The model picker above the composer shows running models that GUI3 can use and
Lemonade's default-model shortcuts. Selecting a downloaded shortcut loads it;
selecting one that is not downloaded starts its download and load. The row
action can eject a running model.

Select **New chat** in the History rail to start another conversation. Each
history row can be deleted. Conversations remain usable while another
conversation streams, and navigation to another workspace does not cancel an
active response.

Type a prompt and press `Enter` to send it; use `Shift+Enter` for a newline.
While a response streams, the Send button becomes **Stop**. Message actions
include:

- **Edit & resend** on a text user message;
- **Copy** and **Retry** on an assistant response; and
- **Read aloud** when a compatible TTS model is available.

The assistant response can also show reasoning, tool calls, generated media,
throughput, time to first token, and token count.

The **Thinking** menu switches supported chat requests between reasoning and a
direct answer. **Logs** opens a resizable live-log pane beside the conversation.
The plus menu offers:

- **Add files** for image or audio attachments supported by the selected model;
  images can also be pasted or dropped into the composer; and
- **Tools** to enable built-in Lemonade tools and choose tools from available
  external MCP servers for that chat.

Attachment types and limits depend on the model. Chat can also present
model-specific composers for image generation and editing, transcription,
music and sound effects, TTS and voice cloning, and 3D generation. Embedding,
reranking, and classification models are managed in Models but are not
selectable in the Chat composer.

Chat history is client-side. It survives app reload only when **Settings >
Chat > Save chat history in this browser** is enabled; attached media is never
persisted.

## Inspect Effective settings

Open **Effective settings** for the selected concrete model. The dialog is
separate from **Models > Configuration** and does not make persistent model
changes.

1. **Settings by source** shows known resolved values and where each came from.
   Sources can include runtime resolution, direct configuration, a recipe
   default, optimization, or the Chat plus menu.
2. **Chat sampling** stores request-time values for this model and GUI client.
3. **Effective load command** is the authoritative command reported by the
   running server.

![Effective settings, with callouts for resolved sources, chat sampling, and the running command](https://raw.githubusercontent.com/lemonade-sdk/assets/39bdd476eced437d135a971d893a99f917117a64/docs/guides/gui3-guide/07-effective-settings.png)

Select **Save sampling** to store temperature, top-p, top-k, min-p, and repeat
penalty for future chat requests from this client. An empty field omits that
parameter so the backend uses its default. Saving sampling does not reload the
model and does not change the load-time sampling arguments under **Models >
Configuration**.

The effective command is available only while the model is loaded. It is the
exact command for the current backend process, so a pending configuration
change does not appear there until the model reloads.

### Apply a session-only backend override

For a supported backend, the bottom of Effective settings exposes a locked raw
argument editor:

1. Review the running command (**1**).
2. Select **I know what I am doing** (**2**) to unlock the editor.
3. Edit the session-only argument string (**3**).

![The Effective settings backend override, with callouts for the running command, unlock control, and session-only editor](https://raw.githubusercontent.com/lemonade-sdk/assets/39bdd476eced437d135a971d893a99f917117a64/docs/guides/gui3-guide/08-session-override.png)

The buttons depend on runtime state:

- **Apply & reload** stores the override in this app session and immediately
  reloads a running model with it.
- **Apply for next load** stores the override for the next load when the model
  is not running.
- **Reset override** clears the session override. If the model is running, it
  also reloads with the normally resolved settings.

The raw override replaces the resolved backend argument string for that load.
It is never written to disk and disappears when the app reloads. Use **Models >
Configuration** for changes that should persist on the server.

Effective settings is not shown for image models or collection wrappers.

## Manage inference backends

**Backends** groups runtime variants by capability. The rail filters
**All Backends**, **Installed**, **Available**, **Updates**, and
**Experimental**. It can also show technical details, unsupported backends,
and backend logos.

Use **Install**, **Update**, **Update all**, or **Uninstall** as available. A
backend that needs manual setup provides **Setup guide** instead. Installation
progress and failures appear on the backend card and in Downloads.

Some installed variants provide a terminal button for persistent,
backend-wide CLI arguments. Those defaults are different from a model's
**Configuration** and from the session-only Effective settings override.

## Browse applications

**Apps** loads Lemonade's curated marketplace and groups entries by category.
Featured apps appear first. Use **Visit** to open an app or its site, **Guide**
for setup instructions, and **Video** when one is provided. These actions open
external links; GUI3 does not install third-party applications.

The title-bar search searches app names, descriptions, and categories and
prioritizes app results while Apps is open.

## Monitor the server

Monitor data describes the connected server, not only this GUI window.

- **Performance** shows server health and uptime, prompt and generation
  throughput, active inference slots, system CPU/RAM/GPU/VRAM, cache use, last
  inference details, loaded models, and configured model limits. **Pause**
  freezes dashboard updates; **Resume** restarts them.
- **Telemetry** captures request traces. Search or filter by request kind or
  errors, inspect overview and messages, replay a request with changed
  parameters, compare results, or use **Improve** with an eligible model.
  Requests can also be created manually, deleted, cleared, or exported to the
  clipboard as JSON.
- **Logs** streams server output. Filter by text, minimum level, capture level,
  or source; clear the displayed buffer or reconnect when the stream drops.

Some cards remain empty until a compatible model is loaded or a request has
run. Telemetry capture and server logging can include prompt content; review
your privacy requirements before enabling or exporting them.

## Configure Settings

Settings is divided by purpose:

- **Server:** endpoint, optional API key, connection status, and local-data
  cleanup.
- **Chat:** optional browser history, collapsed-reasoning default, default TTS
  model, and automatic speech playback mode.
- **Memory:** server-owned automatic eviction, eviction threshold, and maximum
  loaded models per type. Pinned models are protected from eviction.
- **Model storage:** the server's model-cache and external custom-model
  directories, plus the server-startup model-update check.
- **Cloud providers:** server-owned OpenAI-compatible provider definitions.
  Provider keys come from `LEMONADE_<PROVIDER>_API_KEY` environment variables
  or ephemeral server memory and are not written to `config.json`.
- **MCP gateway:** the built-in Lemon-Tools MCP endpoint and discovered tools.
  The current GUI reports external MCP administration as unavailable while its
  local client host is being introduced.
- **Help & support:** documentation, release notes, GitHub, and Discord.

**Save settings** applies the current Settings section. In Chat,
**Reset defaults** resets the local draft; save it to persist the reset. In
Memory and model-update settings, **Discard changes** reloads the server's
current values.

Theme is a title-bar control and is local to the GUI client. GUI3 does not
currently expose a zoom setting in Settings.

## Downloads, progress, and errors

Open the title-bar download icon to see active, paused, completed, cancelled,
and failed model or backend downloads. Rows show file progress, bytes, speed,
and estimated time when the server supplies them. Expand a row for file
details.

Depending on state, a row can be paused, resumed, cancelled, retried, removed
from the list, or have its partial files deleted. **Clear completed** removes
terminal history rows. Finalization can continue briefly after the byte count
reaches 100%.

Model preparation also appears inline in Chat, and model, backend, connection,
MCP, and download failures appear near the control that initiated them. GUI3
does not currently provide a general-purpose Jobs workspace; use the
[Job Engine API](../api/lemonade.md#job-engine-api) for server job sequences.

## Remote servers and multiple clients

A single `lemond` can serve several desktop or browser clients. Server-owned
state is shared:

- downloaded and loaded models, pin state, and backend installations;
- persistent **Models > Configuration** values;
- memory, storage, model-update, and cloud-provider configuration; and
- Monitor data and server logs.

Client-owned state is independent:

- endpoint, session API key, layout, theme, and navigation state;
- Favorites, chat history, and Chat display or speech preferences;
- request-time Chat sampling and tool selection; and
- session-only Effective settings backend overrides.

One client's load, unload, delete, configuration, or server Settings action can
therefore affect other clients. Its theme, history, Favorites, and chat
sampling do not.

## Troubleshooting and limitations

1. **The status dot is not green:** verify the full URL and API key under
   **Settings > Server**, then select **Connect**. For a browser client, also
   check the server's allowed origins.
2. **A model cannot load:** confirm that its files are downloaded and its
   required backend is installed. Check the model detail error and **Monitor >
   Logs**.
3. **A backend is missing:** enable **Show unsupported backends** to inspect
   compatibility. Unsupported hardware does not become compatible by showing
   the entry.
4. **The catalog is empty:** reconnect, clear restrictive filters, and enable
   an online catalog for remote search.
5. **An attachment or mode is unavailable:** the selected model does not
   advertise that input or capability. Choose a compatible loaded model or
   Omni Collection.
6. **A setting did not persist:** use **Save** in **Models > Configuration**
   for persistent load values. Chat sampling and raw Effective settings
   overrides are client-side; raw overrides also end when the app reloads.
7. **Inference is slow or fails:** inspect **Monitor > Telemetry** and
   **Monitor > Logs**, then compare the running command in Effective settings
   with the persistent model configuration.

### Report a GUI3 issue

For a problem specific to GUI3, use the repository's
[issue forms](https://github.com/lemonade-sdk/lemonade/issues/new/choose) and
choose **Bug Report**. Include:

- OS, GUI3 app version, and Lemonade Server version;
- steps to reproduce;
- expected and actual behavior;
- relevant logs or screenshots; and
- whether it also happens with the stable GUI against the stable server.

For a preview build, select the **GUI3 Beta** milestone if GitHub allows you to
assign it. If you do not have milestone permission, mention that you are using
the GUI3 beta build and leave the milestone assignment to a maintainer. Use
the normal issue process for unrelated server, backend, model, or API issues.

For server behavior, see [server concepts](concepts.md),
[configuration](configuration/README.md), and the [API reference](../api/README.md).
