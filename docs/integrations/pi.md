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

## How it works

`lemonade launch pi` writes two configuration files:

**`~/.pi/agent/models.json`** (provider registration):
```json
{
  "providers": {
    "Lemonade": {
      "baseUrl": "http://localhost:13305/v1",
      "api": "openai-completions",
      "apiKey": "lemonade",
      "models": [
        { "id": "Qwen3.5-35B-A3B-GGUF" }
      ]
    }
  }
}
```

**`~/.pi/agent/settings.json`** (default provider/model):
```json
{
  "defaultProvider": "Lemonade",
  "defaultModel": "Qwen3.5-35B-A3B-GGUF"
}
```

If these files already exist, Lemonade merges the `Lemonade` provider into your existing configuration while preserving other providers and settings.

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
