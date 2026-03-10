# Claude Code

Claude Code is a high-performance agentic coding CLI from Anthropic that can reason about your codebase, execute commands, and edit files. While it is natively designed for Anthropic's hosted models, **Lemonade Server** allows you to use Claude Code with **local models** by emulating the Anthropic API.

This setup provides a private, offline-capable coding assistant that lives in your terminal and has full access to your local development environment.

## Prerequisites

### 1. Install Claude Code
Claude Code can be installed via npm or using the official installation script:

**Using curl:**
```bash
curl -fsSL https://claude.ai/install.sh | bash
```

**Using npm:**
```bash
npm install -g @anthropic-ai/claude-code
```

### 2. Lemonade Server
Ensure you have Lemonade Server installed. If you haven't set it up yet, refer to the [Getting Started guide](../../dev-getting-started.md).

### 3. Download a Coding Model
Claude Code requires a model with strong instruction-following and tool-use capabilities. We recommend **Qwen3.5-35B-A3B-GGUF** or **GLM-4.7-Flash-GGUF**.

```bash
# Recommended for most coding tasks
lemonade-server pull Qwen3.5-35B-A3B-GGUF

# High-performance alternative
lemonade-server pull GLM-4.7-Flash-GGUF
```

## Launching Claude Code

Lemonade provides a specialized `launch` command that streamlines the connection between the Claude Code CLI and your local server. It handles all necessary environment variables, including API redirection and performance optimizations.

### Step 1: Start Lemonade Server
In one terminal window, ensure the server is running:
```bash
lemonade-server serve --ctx-size 32768
```

We recommend starting the server with a context window size starting at 32768 tokens to accomodate for Claude Code's system prompt (20k+ tokens). Note that you might need to change this value depending on your hardware and project size.

### Step 2: Launch the Agent
Navigate to your project directory in another terminal and run:
```bash
lemonade-server launch claude -m Qwen3.5-35B-A3B-GGUF
```

**What happens under the hood?**
When you execute the `launch` command, Lemonade Server initiates a **concurrent load** of the specified model on the backend. This means the server starts loading the model into memory in a background thread, allowing the Claude Code CLI interface to start instantly without blocking.

Additionally, the command automatically configures several critical environment variables:
- `ANTHROPIC_BASE_URL`: Redirects Claude Code to your local Lemonade instance.
- `ANTHROPIC_API_KEY`: Sets a local placeholder key.
- `ANTHROPIC_AUTH_TOKEN`: Sets a local placeholder token.
- `CLAUDE_CODE_ATTRIBUTION_HEADER`: Set to `0` to prevent KV cache invalidation, ensuring maximum inference speed.
- `CLAUDE_CODE_DISABLE_NONESSENTIAL_TRAFFIC`: Set to `1` to minimize telemetry and unnecessary external calls.

## Performance Expectations

### Concurrent Loading and Initial Delay
Because the model is loaded concurrently on the backend, if you send a query immediately after launching the CLI, you may experience an initial delay while the model finishes loading into memory.

### System Prompt Processing
The first query you send to Claude Code will take longer than subsequent ones, typically **30-40 seconds on a Strix Halo system**, though this varies based on your hardware and model selected.

This delay occurs because Claude Code sends a massive system prompt (20,000+ tokens) that defines its agentic behavior and tool-use capabilities. Once Lemonade processes this prompt, it is cached in the KV (Key-Value) cache, and subsequent queries will respond significantly faster as long as the session remains active.

## Best Practices for Strix Halo (128GB)

If you are using a **Strix Halo** system with **128GB of RAM**, you have a top-tier environment for local AI! You can run larger models with significantly expanded context windows.

### Recommended Model Choices - MoE models with strong agentic capabilities
*   **Qwen3.5-35B-A3B-GGUF**: A Mixture of Experts (MoE) model that excels at agentic tasks. It has 35B parameters, 3B of which are active at a time.
*   **Qwen3-Coder-Next**: An MoE model designed specifically for coding agents and local development with 80B total parameters, 3B of which are activated
*   **GLM-4.7-Flash-GGUF**: An excellent alternative for rapid iterations and complex instruction following, a 30B-A3B MoE model

### Custom Tuning with `llamacpp-args`
By default, Lemonade uses the coding defaults (`-b 4096 -ub 1024 -fa on`). Using the `--llamacpp-args` flag, you can customize the parameters passed to llama-server when the model is being loaded.
```bash
lemonade-server launch claude -m Qwen3.5-35B-A3B-GGUF --llamacpp-args "-b 1024 -ub 1024 -fa on"
```

- `-b` and `-ub`: These control the batch size and physical batch size. While the llama.cpp default is `512`, increasing these (e.g., to `1024` or `2048`) helps saturate memory bandwidth on high-end hardware like Strix Halo, though it increases RAM/VRAM consumption at the same time.
- `-fa on`: Enables Flash Attention for optimized performance

### Using Model Recipes
While you can manually pass arguments with `--llamacpp-args`, a more scalable approach is to use the model's saved configuration by passing `--use-recipe`.

```bash
lemonade-server launch claude -m Qwen3.5-35B-A3B-GGUF --use-recipe
```

**How Recipes Work**
When `--use-recipe` is invoked, Lemonade skips the default `launch` arguments and instead reads from your `recipe_options.json` file. This file stores per-model runtime settings (like context window size, batch size, and hardware acceleration backends).

- **Location:** You can find or edit this file directly in your Lemonade cache directory:
  - **Linux/macOS:** `~/.cache/lemonade/recipe_options.json`
  - **Windows:** `%USERPROFILE%\.cache\lemonade\recipe_options.json`
- **Keys:** Entries in this JSON file use the full prefixed model name (e.g., `"user.Qwen3.5-35B-A3B-GGUF"`).

**Importing via Web UI**
Instead of manually editing the JSON file, you can also easily add recipes using the Lemonade Web Interface:
1. Open the Lemonade Web Interface (usually `http://localhost:8000`).
2. Navigate to the model management section.
3. Click on **"Import a model"**.
4. Upload the recipe configuration.

**Settings Priority**
When loading a model for a launched agent, Lemonade Server resolves settings in this order (highest priority first):
1. Explicit values passed in the load request (e.g., using `--llamacpp-args` via CLI).
2. Per-model values defined in `recipe_options.json` (used when `--use-recipe` is active).
3. Global environment variables (e.g., `LEMONADE_MAX_LOADED_MODELS`).
4. Hardcoded system defaults.

For community-tested configurations optimized for specific hardware setups, check out the [Lemonade Recipes Wiki](https://github.com/lemonade-sdk/lemonade/wiki/Recipes) (Note: these are currently a work in progress).

## What's realistic to achieve?

Local agents are incredibly useful, but they have different strengths than the giant models running in the cloud.

**Where they work well:**
Local models work well for focused, well-defined tasks. If you need to refactor a specific module or generate some boilerplate they work great.

**Where they fall short:**
Local models aren't quite ready to handle massive, project-wide refactors that touch dozens of interdependent files. They can also sometimes lose their way if they hit an unexpected error halfway through a complex task.

## Troubleshooting

### Login Prompt appears
If Claude Code asks you to log in to Anthropic, it means the environment variables weren't picked up correctly. Ensure you are using `lemonade-server launch claude` rather than calling `claude` directly.

### Performance is Slow
- Verify that `Flash Attention` is enabled in your `llamacpp-args`.
- Check that no other heavy applications are consuming your GPU resources.
- If you are using a very large context (`-c`), the "prefill" time (time to first token) will increase.

### "Permission Denied" when editing files
Claude Code may ask for permission to run commands or edit files. You can run it with the `--dangerously-skip-permissions` flag if you trust the model in a sandbox, but manual approval is recommended for safety.

---
*For more information on Claude Code's capabilities, visit the [Anthropic Documentation](https://docs.anthropic.com/en/docs/agents-and-tools/claude-code).*
