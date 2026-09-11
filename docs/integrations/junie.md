# Junie

Junie is JetBrains' coding agent CLI. With Lemonade, you can run Junie against local models through Lemonade's OpenAI-compatible API.

This guide focuses on the most common launch flows.

## Prerequisites

1. Install Junie:

    ```bash
    curl -fsSL https://junie.jetbrains.com/install.sh | bash
    ```

    See [https://junie.jetbrains.com/](https://junie.jetbrains.com/) for details and other platforms.

2. Make sure Lemonade Server is running (`lemond`).

## Launch Junie with Lemonade

Use:

```bash
lemonade launch junie [options]
```

Lemonade automatically writes a custom model profile at `~/.junie/models/lemonade.json` pointing Junie at your local server, and starts Junie with `--model custom:lemonade`. If `JUNIE_HOME` is set, the profile is written under that directory instead.

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

When `--model` is provided, launch goes straight to starting the agent and loading that model. The selected model name is stored as the `id` inside `~/.junie/models/lemonade.json`.

## Passing Junie arguments with `--agent-args`

You can pass any extra Junie CLI flags through Lemonade:

```bash
lemonade launch junie --model Qwen3.5-35B-A3B-GGUF --agent-args "--brave"
```

If Junie supports a flag, you can pass it through `--agent-args`.

## Related CLI Docs

For more launch examples and full option details, see:
`docs/lemonade-cli.md`

For Junie product details, see JetBrains' docs:
https://junie.jetbrains.com/
