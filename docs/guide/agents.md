# Agents

Lemonade can install and configure coding agents for you, starting with [Pi](../integrations/pi.md). It installs the version Lemonade pins, checks the download against a SHA-256 shipped with Lemonade, and keeps the agent pointed at your Lemonade server.

```bash
lemonade agents install pi
lemonade launch pi -m Qwen3.5-35B-A3B-GGUF
```

`lemonade launch pi` also offers to install Pi when it isn't installed.

## What gets installed where

| | Linux | macOS | Windows |
|---|---|---|---|
| Agent | `${XDG_DATA_HOME:-~/.local/share}/lemonade/agents/pi/<version>/` | `~/Library/Application Support/lemonade/agents/pi/<version>/` | `%LOCALAPPDATA%\lemonade\agents\pi\<version>\` |
| Shim | `${XDG_BIN_HOME:-~/.local/bin}/pi` | `~/.local/bin/pi` | `%USERPROFILE%\.local\bin\pi.cmd` |
| Agent host state | `${XDG_STATE_HOME:-~/.local/state}/lemonade/agent-host/` | `~/Library/Application Support/lemonade/agent-host/` | `%LOCALAPPDATA%\lemonade\agent-host\` |

Pi's own settings stay in `~/.pi/agent` (or `$PI_CODING_AGENT_DIR`), where Lemonade writes its provider and, with `--mcp`, its MCP server.

The shim is a small script that runs the installed Pi, so `pi` in a terminal runs the same version `lemonade launch pi` does. Lemonade only writes and removes a shim that carries its marker line; if `~/.local/bin/pi` is someone else's file, Lemonade leaves it alone and warns. Lemonade never edits your shell profile or `PATH`: `lemonade agents list` tells you when `~/.local/bin` isn't on your `PATH`.

Pi's own update command is redirected to Lemonade (`PI_SELF_UPDATE_COMMAND`), so `pi update` points you at `lemonade agents install pi`, which moves to the version the installed Lemonade pins.

## Commands

| Command | What it does |
|---|---|
| `lemonade agents` | List agents: the pinned version, the installed one, which `pi` is in use and where it is |
| `lemonade agents install pi [--force]` | Install or move to the pinned version; `--force` downloads it again, replacing the files of the installed version, so close a Pi running from it first. Older versions are removed, except one a Pi is still running from, which goes on a later install |
| `lemonade agents uninstall pi` | Remove the installed versions and the shim. Pi's settings in `~/.pi/agent` stay, including the Lemonade provider |
| `lemonade agents configure pi [--mcp\|--no-mcp]` | Point Pi at this server again: rewrite the Lemonade provider with every chat model, and add or remove Lemonade's MCP server |
| `lemonade agents enable` / `disable` | Start the agent host with your session (systemd `--user` on Linux, a LaunchAgent on macOS) |

A Pi you installed yourself, for example with npm, keeps working: `lemonade launch pi` uses it when Lemonade hasn't installed one.

## The agent host

Agents are installed and configured by `lemonade-agent-host`, a small process that runs as you. lemond never touches your files: it passes `/v1/agents` requests to your agent host, which connects to lemond over a WebSocket on lemond's port.

- `lemonade` starts the host when an `agents` command or `launch pi` needs it, and the host exits after 10 idle minutes. `lemonade agents enable` keeps one running instead.
- When lemond requires an API key, the host presents the same key: the one `lemonade` was given when it starts the host, `LEMONADE_API_KEY` from `~/.config/lemonade/lemond.conf` or `~/.config/lemonade/agent-host.env` for the systemd and launchd host. Lemonade never writes the key to disk. On macOS the LaunchAgent reads those files with `/bin/sh`, so quote a value that has spaces or shell characters (`LEMONADE_API_KEY='…'`); systemd reads the same quoting.
- One host serves one lemond. Agents need lemond on the same machine; against a remote server, `lemonade launch pi` runs the Pi on your `PATH` as before.
- The host's log is `agent-host.log` in its state directory.

### Trust model

The host acts for whoever can read its token file, which only you can read, and reach lemond (with its API key, when it has one). With both, a program can install the pinned Pi, rewrite Pi's Lemonade settings and get the command `launch` would run. It can't make the host run anything: the host returns a command and `lemonade` runs it. Downloads are only installed when they match the checksum pinned in `agent_versions.json`.
