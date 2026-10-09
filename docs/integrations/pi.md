# Pi

Pi is a terminal-based coding agent by [earendil-works](https://github.com/earendil-works/pi). With Lemonade, you can run Pi against local models through Lemonade's OpenAI-compatible API.

This guide focuses on the most common launch flows.

## Prerequisites

1. Install Pi:

    ```bash
    npm install -g @earendil-works/pi-coding-agent
    ```

2. Make sure Lemonade Server is running (`lemond`).

## Launch Pi with Lemonade

Use:

```bash
lemonade launch pi [options]
```

Lemonade automatically configures Pi to use your local server by writing:
- `~/.pi/agent/models.json` — Registers the Lemonade provider with your local server's base URL and selected model
- `~/.pi/agent/settings.json` — Sets the default provider to Lemonade so Pi launches straight into your local model

## Use Case 1: First-time user (discover + import + launch)

If you are not sure which model to use yet, start with:

```bash
lemonade launch pi
```

You will get an interactive menu where you can:

- Select a recipe to import and launch.
- Browse downloaded models.
- Browse recommended llama.cpp models (download may be required), then launch.

All remote recipes in this flow are sourced from:
`https://github.com/lemonade-sdk/recipes`

## Use Case 2: You already know the model

If you already downloaded a model or already imported the recipe, skip the interactive flow:

```bash
lemonade launch pi -m Qwen3.5-35B-A3B-GGUF
```

Equivalent long form:

```bash
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF
```

When `--model` is provided, launch goes straight to starting the agent and loading that model.

## Passing Pi arguments with `--agent-args`

You can pass any extra Pi CLI flags through Lemonade:

```bash
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF --agent-args "-e npm:@foo/my-extension"
```

If Pi supports a flag, you can pass it through `--agent-args`.

## Lemonade tools in Pi (MCP)

Add `--mcp` to give Pi Lemonade's [MCP tools](../api/mcp.md): list models, chat with another local model, generate images, transcribe audio, and search the Lemonade docs.

```bash
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF --mcp
```

This adds a `lemonade` server to `~/.pi/agent/mcp.json`. It stays there for later launches, and each launch updates its URL and API key reference. Run `pi mcp list` to check the connection. To remove it again:

```bash
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF --no-mcp
```

`--no-mcp` removes only the `lemonade` server; your other MCP servers are left alone.

Pi shows generated images inline only in terminals that support the kitty or iTerm2 image protocol, such as Ghostty, kitty, WezTerm and iTerm2, because it draws images with those protocols' escape sequences. Other terminals, and terminal multiplexers like tmux that don't pass those sequences through, show a placeholder such as `[Image: [image/png] 512x512]` instead. The model still receives the image if it accepts image input. To keep a copy on disk, ask Pi to save it, for example "generate an image of a red dog and save it to disk". The tool then writes the PNG to Lemonade's MCP image folder (`mcp-images` in the Lemonade cache directory, or `LEMONADE_MCP_IMAGE_DIR` if set) and returns its path instead of the inline image.

## Codemode

Add `--codemode` to turn on Pi's [codemode](https://pi.dev/docs/latest/codemode) tool for that launch. The model can then write scripts that call Pi's tools. With `--mcp` as well, scripts can call Lemonade's tools, for example to generate an image and show it:

```bash
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF --mcp --codemode
```

```js
const result = await tools.mcp__lemonade__lemonade_generate_image({ prompt: "a red apple", model: "SD-Turbo-GGUF" });
image(result.content.find((block) => block.type === "image"));
```

Download an image model first, for example `lemonade pull SD-Turbo-GGUF`. `--codemode` does not change Pi's settings.

## Images and thinking

- Models labeled `vision` accept images: paste one, or reference it with `@image.png`. Other models get a note that the image was left out.
- Models labeled `reasoning` show their thinking, and Pi's thinking level switches it between `off` and `medium`.
- Launch warns when the model is not labeled `tool-calling`. Pi needs tool calls to read and edit files.

## How it works

`lemonade launch pi` writes these files in Pi's agent directory, `~/.pi/agent/` (or `$PI_CODING_AGENT_DIR`):

**`models.json`** registers the Lemonade provider with every downloaded chat model:

```json
{
  "providers": {
    "Lemonade": {
      "baseUrl": "http://localhost:13305/v1",
      "api": "openai-completions",
      "apiKey": "lemonade",
      "compat": {
        "supportsStore": false,
        "supportsDeveloperRole": false,
        "supportsReasoningEffort": false,
        "maxTokensField": "max_tokens"
      },
      "models": [
        {
          "id": "Qwen3.5-35B-A3B-GGUF",
          "contextWindow": 32768,
          "maxTokens": 32768,
          "input": ["text", "image"]
        }
      ]
    }
  }
}
```

- `contextWindow` and `maxTokens` come from the model's saved `ctx_size`, or 40960 when none is saved. If the model runs with a different context size, save it with `lemonade load <model> --ctx-size N --save-options` so Pi compacts the conversation before it overflows.
- `apiKey` is the placeholder `lemonade` when Lemonade has no API key. When it has one (`--api-key`, `LEMONADE_API_KEY` or `LEMONADE_ADMIN_API_KEY`), the value is `${LEMONADE_API_KEY}`: launch passes the key to Pi in that environment variable, and it is never written to disk. Running `pi` directly then needs `LEMONADE_API_KEY` set.

**`settings.json`** makes Lemonade Pi's default provider, only if Pi has no default yet:

```json
{
  "defaultProvider": "Lemonade",
  "defaultModel": "Qwen3.5-35B-A3B-GGUF"
}
```

**`mcp.json`** (with `--mcp`) adds the `lemonade` server:

```json
{
  "mcpServers": {
    "lemonade": {
      "url": "http://localhost:13305/mcp",
      "headers": { "Authorization": "Bearer ${LEMONADE_API_KEY}" },
      "exposure": "direct",
      "description": "Lemonade local AI server: ..."
    }
  }
}
```

The `headers` entry is only written when Lemonade has an API key.

If these files already exist, Lemonade updates only its own entries and keeps your other providers, servers and settings.

## Switching models

You can switch models inside Pi with `/model` or `Ctrl+P`, or launch with a different model:

```bash
lemonade launch pi --model Gemma-4-E2B-it-GGUF
```

## Related CLI Docs

For more launch examples and full option details, see:
[docs/guide/cli.md](../guide/cli.md)

For Pi product details, see the official docs:
https://pi.dev/docs/latest/
