# OpenCode

[OpenCode](https://opencode.ai) is a terminal-based coding agent. With Lemonade, you can run OpenCode against local models through Lemonade's OpenAI-compatible API.

## Prerequisites

1. Install OpenCode:

    ```bash
    curl -fsSL https://opencode.ai/install | bash
    ```

    Launch works with OpenCode 1 and 2. `--codemode` needs OpenCode 2. Check with `opencode --version`.

2. Make sure Lemonade Server is running (`lemond`).

## Launch OpenCode with Lemonade

```bash
lemonade launch opencode --model Qwen3.5-35B-A3B-GGUF
```

Without `--model`, launch shows an interactive menu to pick or import a model. Launch writes the Lemonade provider to `~/.config/opencode/opencode.json` (or `$XDG_CONFIG_HOME/opencode/opencode.json`) and starts OpenCode on the chosen model. See [How it works](#how-it-works).

If the model's context size is automatic, launch waits for the model to load so it can give OpenCode the size Lemonade picked; otherwise it starts OpenCode while the model loads.

OpenCode 2 normally attaches to a shared background service that ignores the launch environment, so launch runs it with `--standalone`.

## Passing OpenCode arguments with `--agent-args`

```bash
lemonade launch opencode --model Qwen3.5-35B-A3B-GGUF --agent-args "--continue"
```

Subcommands work too, for example `--agent-args "run 'explain this repo'"`. Launch adds `--standalone` only where OpenCode 2 accepts it.

## Lemonade tools in OpenCode (MCP)

Add `--mcp` to give OpenCode Lemonade's [MCP tools](../api/mcp.md): list models, chat with another local model, generate images, transcribe audio, and search the Lemonade docs.

```bash
lemonade launch opencode --model Qwen3.5-35B-A3B-GGUF --mcp
```

This adds a `lemonade` server to `opencode.json`. It stays there for later launches, and each launch updates its URL and API key reference. To remove it again:

```bash
lemonade launch opencode --model Qwen3.5-35B-A3B-GGUF --no-mcp
```

`--no-mcp` removes only the `lemonade` server; your other MCP servers are left alone. If the server can't be removed, launch stops instead of starting OpenCode.

OpenCode 2's `opencode run` doesn't give MCP tools to the model ([anomalyco/opencode#33027](https://github.com/anomalyco/opencode/issues/33027)); the interactive interface does.

## Code Mode

OpenCode 2's [Code Mode](https://opencode.ai/v2/docs/mcp-servers) lets the model write a script that calls MCP tools. Launch turns it off for the `lemonade` server so its tools stay on the model's normal tool list. Add `--codemode` to turn it on for one launch:

```bash
lemonade launch opencode --model Qwen3.5-35B-A3B-GGUF --mcp --codemode
```

Code Mode only covers MCP tools, so `--codemode` has no effect without the `lemonade` server. It needs OpenCode 2: launch stops with OpenCode 1, and skips it with a warning when it can't tell the version.

## Images and tool calling

- Models labeled `vision` accept images. Other models are marked text-only, so OpenCode doesn't send them images.
- Launch warns when the model is not labeled `tool-calling`. OpenCode needs tool calls to read and edit files.
- Launch also warns when the model name is not on the Lemonade server, for example a typo.

## How it works

`lemonade launch opencode` writes the Lemonade provider into `opencode.json`, with every downloaded chat model plus the launched model if it is not downloaded yet:

```json
{
  "$schema": "https://opencode.ai/config.json",
  "provider": {
    "Lemonade": {
      "npm": "@ai-sdk/openai-compatible",
      "name": "Lemonade Server (local)",
      "options": {
        "baseURL": "http://localhost:13305/v1",
        "apiKey": "lemonade"
      },
      "models": {
        "Qwen3.5-35B-A3B-GGUF": {
          "name": "Qwen3.5-35B-A3B-GGUF (local)",
          "limit": { "context": 32768, "output": 10922 },
          "modalities": { "input": ["text", "image"], "output": ["text"] }
        }
      }
    }
  }
}
```

- `limit.context` comes from the model's saved `ctx_size`, or 40960 when none is saved or it is automatic (`-1`). For the launched model, `--ctx-size` on `lemonade launch` takes precedence, and an automatic size is read from the loaded model. If that load fails, launch shows the error and doesn't start OpenCode.
- `apiKey` is the placeholder `lemonade` when Lemonade has no API key. When it has one (`--api-key`, `LEMONADE_API_KEY` or `LEMONADE_ADMIN_API_KEY`), the value is `{env:LEMONADE_API_KEY}`: launch passes the key to OpenCode in that environment variable, and it is never written to disk. Running `opencode` directly then needs `LEMONADE_API_KEY` set, and OpenCode 2's background service needs it in its own environment. If launch can't read or update `opencode.json` while a key is set, it stops instead of starting OpenCode, so the key is never sent to the server in the old config.
- The launched model is selected through `OPENCODE_CONFIG_CONTENT`, so your default model in `opencode.json` is unchanged.

With `--mcp`, the `lemonade` server is written where your OpenCode version reads it: `mcp.servers.lemonade` for OpenCode 2, and `mcp.lemonade` for OpenCode 1, which rejects `mcp.servers`.

```json
{
  "mcp": {
    "servers": {
      "lemonade": {
        "type": "remote",
        "url": "http://localhost:13305/mcp",
        "headers": { "Authorization": "Bearer {env:LEMONADE_API_KEY}" },
        "oauth": false,
        "codemode": false
      }
    }
  }
}
```

The `headers` entry is only written when Lemonade has an API key. Lemonade updates only its own entries and keeps your other providers, servers and settings.

## Related CLI Docs

For more launch examples and full option details, see [docs/guide/cli.md](../guide/cli.md).
