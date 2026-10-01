# Junie

[Junie](https://junie.jetbrains.com/) is JetBrains' coding agent, available both inside JetBrains IDEs and as a standalone CLI. With Lemonade, you can run the Junie CLI against local models through Lemonade's OpenAI-compatible API.

This guide focuses on the most common launch flows.

## Prerequisites

1. Install the Junie CLI.

    macOS and Linux:

    ```bash
    curl -fsSL https://junie.jetbrains.com/install.sh | bash
    ```

    Windows:

    ```powershell
    powershell -NoProfile -ExecutionPolicy Bypass -Command "iex (irm 'https://junie.jetbrains.com/install.ps1')"
    ```

    Both installers put a `junie` launcher in `~/.local/bin`. See the [Junie CLI docs](https://junie.jetbrains.com/docs/junie-cli.html) for details.

2. Make sure Lemonade Server is running (`lemond`).

## Launch Junie with Lemonade

Use:

```bash
lemonade launch junie [options]
```

Lemonade automatically writes a [custom model profile](https://junie.jetbrains.com/docs/custom-llm-models.html) at `~/.junie/models/lemonade.json` pointing Junie at your local server, and starts Junie with `--model custom:lemonade`. If `JUNIE_HOME` is set, the profile is written under that directory instead.

A custom model profile is one of Junie's supported ways to authenticate, so no JetBrains account or subscription is required for this flow.

## Use Case 1: First-time user (discover + import + launch)

If you are not sure which model to use yet, start with:

```bash
lemonade launch junie
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
lemonade launch junie -m Qwen3.5-35B-A3B-GGUF
```

Equivalent long form:

```bash
lemonade launch junie --model Qwen3.5-35B-A3B-GGUF
```

When `--model` is provided, launch goes straight to starting the agent and loading that model.

## Passing Junie arguments with `--agent-args`

You can pass any extra Junie CLI flags through Lemonade:

```bash
lemonade launch junie --model Qwen3.5-35B-A3B-GGUF --agent-args "--brave"
```

If Junie supports a flag, you can pass it through `--agent-args`.

## How it works

Junie discovers custom model profiles from `~/.junie/models/*.json`, where each file *is* one profile and the file name is the profile ID. `lemonade launch junie` writes a single file:

**`~/.junie/models/lemonade.json`**:
```json
{
  "id": "Qwen3.5-35B-A3B-GGUF",
  "displayName": "Qwen3.5-35B-A3B-GGUF (Lemonade)",
  "providerName": "Lemonade",
  "baseUrl": "http://localhost:13305/v1/chat/completions",
  "apiType": "OpenAICompletion",
  "maxContextLength": 40960,
  "apiKey": "lemonade"
}
```

`maxContextLength` comes from the model's configured `ctx_size`; change it with `lemonade load <model> --ctx-size N --save-options`.

Lemonade rewrites this file on every launch, so hand edits to it are lost. Profiles in other files, such as `~/.junie/models/my-model.json`, are left untouched.

## Switching models

The `lemonade` profile always carries the model you launched with, so switch models by relaunching:

```bash
lemonade launch junie --model Gemma-4-E2B-it-GGUF
```

## Related CLI Docs

For more launch examples and full option details, see:
[docs/guide/cli.md](../guide/cli.md)

For Junie product details, see the official docs:
[https://junie.jetbrains.com/docs/](https://junie.jetbrains.com/docs/)
