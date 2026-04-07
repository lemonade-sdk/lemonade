# Claude Code

Claude Code is Anthropic's coding agent CLI. With Lemonade, you can run Claude Code against local models through Lemonade's Anthropic-compatible API.

This guide focuses on the most common launch flows.

## Prerequisites

1. Install Claude Code:

```bash
curl -fsSL https://claude.ai/install.sh | bash
```

or:

```bash
npm install -g @anthropic-ai/claude-code
```

2. Make sure Lemonade Server is running (`lemond`).

## Launch Claude Code with Lemonade

Use:

```bash
lemonade launch claude [options]
```

Lemonade automatically configures Claude Code to use your local server.

## Use Case 1: First-time user (discover + import + launch)

If you are not sure which model to use yet, start with:

```bash
lemonade launch claude
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
lemonade launch claude -m Qwen3.5-35B-A3B-GGUF
```

Equivalent long form:

```bash
lemonade launch claude --model Qwen3.5-35B-A3B-GGUF
```

When `--model` is provided, launch goes straight to starting the agent and loading that model.

## Passing Claude arguments with `--agent-args`

You can pass any extra Claude CLI flags through Lemonade:

```bash
lemonade launch claude --model Qwen3.5-35B-A3B-GGUF --agent-args "--approval-mode never"
```

Resume a previous Claude session:

```bash
lemonade launch claude --agent-args="--resume 6a670b84-ac78-47e0-8c2a-c3e85efdd979"
```

If Claude supports a flag, you can pass it through `--agent-args`.

## Related CLI Docs

For more launch examples and full option details, see:
`docs/lemonade-cli.md`

For Claude Code product details, see Anthropic's docs:
https://docs.anthropic.com/en/docs/agents-and-tools/claude-code
