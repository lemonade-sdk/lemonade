# `lemonade` CLI

The `lemonade` CLI is the primary tool for interacting with Lemonade Server from the terminal. It allows you to manage models, recipes, and backends through a simple command-line interface.

**Contents:**

- [Commands](#commands)
- [Global Options](#global-options)
- [Options for list](#options-for-list)
- [Options for pull](#options-for-pull)
- [Options for import](#options-for-import)
- [Options for load](#options-for-load)
- [Options for run](#options-for-run)
- [Options for export](#options-for-export)
- [Options for backends](#options-for-backends)
- [Options for launch](#options-for-launch)
- [Options for scan](#options-for-scan)

## Commands

`lemonade` provides these utilities:

### Quick Start

| Command             | Description                         |
|---------------------|-------------------------------------|
| `run MODEL_NAME`    | Load a model for inference and open the web app in the browser. See command options [below](#options-for-run). |
| `launch AGENT`      | Launch an agent with a model. See command options [below](#options-for-launch). |

### Server

| Command             | Description                         |
|---------------------|-------------------------------------|
| `status`            | Check if server can be reached. If it is, prints server information. Use `--json` for machine-readable output. |
| `logs`              | Open server logs in the web UI. |
| `backends`          | List available recipes and backends. Use `install` or `uninstall` to manage backends. |
| `scan`              | Scan for network beacons on the local network. See command options [below](#options-for-scan). |

### Model Management

| Command             | Description                         |
|---------------------|-------------------------------------|
| `list`              | List all available models. |
| `pull MODEL_OR_CHECKPOINT` | Download a registered model, pull a Hugging Face checkpoint, or manually register a `user.*` model with `--checkpoint`/`--recipe`. See command options [below](#options-for-pull). |
| `import JSON_FILE`  | Import a model from a JSON configuration file. See command options [below](#options-for-import). |
| `delete MODEL_NAME` | Delete a model and its files from local storage. |
| `load MODEL_NAME`   | Load a model for inference. See command options [below](#options-for-load). |
| `unload [MODEL_NAME]` | Unload a model. If no model name is provided, unload all loaded models. |
| `export MODEL_NAME` | Export model information to JSON format. See command options [below](#options-for-export). |

### Global Flags

| Flag                | Description                         |
|---------------------|-------------------------------------|
| `--help`            | Display help information. |
| `--help-all`        | Display help information for all subcommands. |
| `--version`         | Print the `lemonade` CLI version. |

## Global Options

The following options are available for all commands:

| Option | Description | Default |
|--------|-------------|---------|
| `--host HOST` | Server host address | `127.0.0.1` |
| `--port PORT` | Server port number | `13305` |
| `--api-key KEY` | API key for authentication | None |

These options can also be set via environment variables:
- `LEMONADE_HOST` for `--host`
- `LEMONADE_PORT` for `--port`
- `LEMONADE_API_KEY` or `LEMONADE_ADMIN_API_KEY` for `--api-key`

**Examples:**

On Linux/macOS:
```bash
export LEMONADE_HOST=192.168.1.100
export LEMONADE_PORT=13305
export LEMONADE_API_KEY=your-api-key-here
lemonade list
```

On Windows (Command Prompt):
```cmd
set LEMONADE_HOST=192.168.1.100
set LEMONADE_PORT=13305
set LEMONADE_API_KEY=your-api-key-here
lemonade list
```

On Windows (PowerShell):
```powershell
$env:LEMONADE_HOST="192.168.1.100"
$env:LEMONADE_PORT="13305"
$env:LEMONADE_API_KEY="your-api-key-here"
lemonade list
```

**Admin API Key Example:**

To use the admin API key (which provides full access including internal endpoints):

On Linux/macOS:
```bash
export LEMONADE_ADMIN_API_KEY=admin-secret-key
lemonade list
```

On Windows (Command Prompt):
```cmd
set LEMONADE_ADMIN_API_KEY=admin-secret-key
lemonade list
```

On Windows (PowerShell):
```powershell
$env:LEMONADE_ADMIN_API_KEY="admin-secret-key"
lemonade list
```

```bash
# List all available models
lemonade list

# Pull a custom model with specific checkpoint
lemonade pull user.MyModel --checkpoint main org/model:Q4_K_M --recipe llamacpp

# Load a model with custom recipe options
lemonade load Qwen3-0.6B-GGUF --ctx-size 8192

# Install a backend for a recipe
lemonade backends install llamacpp:vulkan

# Export model info to JSON file
lemonade export Qwen3-0.6B-GGUF --output model-info.json
```

## Options for list

The `list` command displays available models. By default, it shows all models. Use the `--downloaded` flag to filter for downloaded models only:

```bash
lemonade list [options]
```

| Option                         | Description                         | Default |
|--------------------------------|-------------------------------------|---------|
| `--downloaded`                 | Show only downloaded models | False |

## Options for pull

The `pull` command downloads and installs models. The single positional argument can be either:

1. **A registered model name** from the [Lemonade Server registry](https://lemonade-server.ai/models.html), e.g. `Qwen3-0.6B-GGUF`.
2. **A Hugging Face checkpoint** of the form `owner/repo`, with an optional `:variant` suffix, e.g. `unsloth/Qwen3-8B-GGUF` or `unsloth/Qwen3-8B-GGUF:Q4_K_M`. When the variant is omitted (or doesn't match), Lemonade fetches the repository, lists the available quantizations (including sharded folder variants), auto-detects any `mmproj-*.gguf` files for vision models, infers labels from the repo id (`embed`/`rerank`), and presents an interactive menu.
3. **A custom `user.*` model name** when you want to manually register a model with explicit checkpoints, recipe, and optional labels.

```bash
lemonade pull MODEL_OR_CHECKPOINT [--checkpoint TYPE CHECKPOINT] [--recipe RECIPE] [--label LABEL]
```

| Option | Description | Required |
|--------|-------------|----------|
| `MODEL_OR_CHECKPOINT` | Registered model name, or `owner/repo[:variant]` Hugging Face checkpoint | Yes |
| `--checkpoint TYPE CHECKPOINT` | Manual registration: add a checkpoint entry. Repeat for multi-component models such as `main` + `mmproj` or `main` + `vae`. | No |
| `--recipe RECIPE` | Manual registration: recipe to associate with the new `user.*` model (`llamacpp`, `flm`, `ryzenai-llm`, `whispercpp`, `sd-cpp`, `kokoro`) | No |
| `--label LABEL` | Manual registration: add a label to the new model. Repeatable. Valid: `coding`, `embeddings`, `hot`, `reasoning`, `reranking`, `tool-calling`, `vision` | No |

**Happy path**

Most users only need one of these:

```bash
# Pull a registered model from the Lemonade Server registry
lemonade pull Qwen3-0.6B-GGUF

# Pull a Hugging Face GGUF — interactive variant menu appears
lemonade pull unsloth/Qwen3-8B-GGUF

# Pull a specific variant directly (no menu)
lemonade pull unsloth/Qwen3-8B-GGUF:Q4_K_M

# Vision model — mmproj is auto-detected and the `vision` label is auto-applied
lemonade pull ggml-org/gemma-3-4b-it-GGUF:Q4_K_M

# Sharded variant — all shards in the matching folder are downloaded
lemonade pull unsloth/Qwen3-30B-A3B-GGUF:Q4_K_M
```

**Manual registration from the same command**

When you need explicit multi-file checkpoints, a non-default recipe, or custom labels, use the same `pull` command with a `user.*` model name plus `--checkpoint` and `--recipe`:

```bash
lemonade pull user.NAME --checkpoint TYPE CHECKPOINT [--recipe RECIPE] [--label LABEL]
```

```bash
# Register and pull a custom GGUF model with main checkpoint
lemonade pull user.Phi-4-Mini-GGUF \
  --checkpoint main unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M \
  --recipe llamacpp

# Register and pull a vision model with main + mmproj
lemonade pull user.Gemma-3-4b \
  --checkpoint main ggml-org/gemma-3-4b-it-GGUF:Q4_K_M \
  --checkpoint mmproj ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf \
  --recipe llamacpp

# Register a model with multiple labels
lemonade pull user.MyCodingModel \
  --checkpoint main org/model:Q4_0 \
  --recipe llamacpp \
  --label coding \
  --label tool-calling
```

## Options for import

The `import` command supports two flows:
- Import from a local JSON file.
- Browse remote recipes from `lemonade-sdk/recipes` and import one interactively.

This is useful for importing models with complex configurations that would be cumbersome to specify via command-line options:

```bash
lemonade import [JSON_FILE] [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `JSON_FILE` | Path to a JSON configuration file | No |
| `--directory DIR` | Remote recipes directory to query (e.g., `coding-agents`) | No |
| `--recipe-file FILE` | Specific recipe JSON filename from the selected directory | No |
| `--skip-prompt` | Run non-interactively (requires `--directory` and `--recipe-file` for remote import) | No |
| `--yes` | Alias for `--skip-prompt` | No |

**Remote import notes:**
- Running `lemonade import` without `JSON_FILE` starts interactive recipe browsing from GitHub.
- You can skip recipe import during prompts and continue.
- In non-interactive mode, you must provide both `--directory` and `--recipe-file`.
- `--recipe-file` is only used for remote recipe import (with `--directory`).

**JSON File Format:**

The JSON file must contain the following fields:

| Field | Type | Description |
|-------|------|-------------|
| `model_name` | string | The model name (will be prepended with `user.` if not already present) |
| `recipe` | string | Inference recipe to use (e.g., `llamacpp`, `flm`, `sd-cpp`, `whispercpp`) |
| `checkpoint` | string | Single checkpoint in the format `org/model:variant` | **OR** |
| `checkpoints` | object | Multiple checkpoints as key-value pairs (e.g., `{"main": "org/model:Q4_0", "mmproj": "mmproj.gguf"}`) |

**Optional fields:**

| Field | Type | Description |
|-------|------|-------------|
| `labels` | array | Array of label strings (e.g., `["reasoning", "coding"]`) |
| `recipe_options` | object | Recipe-specific options (e.g., `{"ctx-size": 8192, "llamacpp": "vulkan"}`) |
| `image_defaults` | object | Image generation defaults for image models |
| `size` | string | Model size description |

**Notes:**
- The `model_name` field is required and must be a string
- The `recipe` field is required and must be a string
- Either `checkpoint` (string) or `checkpoints` (object) is required
- If both `checkpoint` and `checkpoints` are present, only `checkpoints` will be used
- The `id` field can be used as an alias for `model_name`
- Unrecognized fields are removed during validation

**Examples:**

`model.json`:
```json
{
  "model_name": "MyModel",
  "checkpoint": "unsloth/Qwen3-8B-GGUF:Q4_K_M",
  "recipe": "llamacpp",
  "labels": ["reasoning"]
}
```

```bash
# Import a model from a JSON file
lemonade import model.json

# Interactively browse and import a remote recipe
lemonade import

# Non-interactive remote import
lemonade import --directory coding-agents --recipe-file GLM-4.7-Flash-GGUF-NoThinking.json --yes
```

`model-with-multiple-checkpoints.json`:
```json
{
  "model_name": "MyMultimodalModel",
  "checkpoints": {
    "main": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
    "mmproj": "ggml-org/gemma-3-4b-it-GGUF:mmproj-model-f16.gguf"
  },
  "recipe": "llamacpp",
  "labels": ["vision", "reasoning"]
}
```

```bash
# Import a model with multiple checkpoints
lemonade import model-with-multiple-checkpoints.json
```

`model-with-id-alias.json`:
```json
{
  "id": "MyModel",
  "checkpoint": "unsloth/Qwen3-8B-GGUF:Q4_K_M",
  "recipe": "llamacpp"
}
```

```bash
# Import using 'id' as alias for model_name
lemonade import model-with-id-alias.json
```

## Options for load

The `load` command loads a model into memory for inference. It supports recipe-specific options that are passed to the backend server:

```bash
lemonade load MODEL_NAME [options]
```

### Recipe-Specific Options

The following options are available depending on the recipe being used:

#### Llama.cpp (`llamacpp` recipe)

| Option | Description | Default |
|--------|-------------|---------|
| `--ctx-size SIZE` | Context size for the model | `4096` |
| `--llamacpp BACKEND` | LlamaCpp backend to use | Auto-detected |
| `--llamacpp-args ARGS` | Custom arguments to pass to llama-server (must not conflict with managed args) | `""` |

#### FLM (`flm` recipe)

| Option | Description | Default |
|--------|-------------|---------|
| `--ctx-size SIZE` | Context size for the model | `4096` |
| `--flm-args ARGS` | Custom arguments to pass to flm serve (e.g., `"--socket 20 --q-len 15"`) | `""` |

#### RyzenAI LLM (`ryzenai-llm` recipe)

| Option | Description | Default |
|--------|-------------|---------|
| `--ctx-size SIZE` | Context size for the model | `4096` |

#### SD.cpp (`sd-cpp` recipe)

| Option | Description | Default |
|--------|-------------|---------|
| `--sdcpp BACKEND` | SD.cpp backend to use (`cpu` for CPU, `rocm` for AMD GPU) | Auto-detected |
| `--sdcpp-args ARGS` | Custom arguments to pass to sd-server (must not conflict with managed args) | `""` |
| `--steps N` | Number of inference steps for image generation | `20` |
| `--cfg-scale SCALE` | Classifier-free guidance scale for image generation | `7.0` |
| `--width PX` | Image width in pixels | `512` |
| `--height PX` | Image height in pixels | `512` |

#### Whisper.cpp (`whispercpp` recipe)

| Option | Description | Default |
|--------|-------------|---------|
| `--whispercpp BACKEND` | WhisperCpp backend to use | Auto-detected |

**Notes:**
- Use `--save-options` to persist your configuration for the model
- Unspecified options will use the backend's default values
- Backend options (`--llamacpp`, `--sdcpp`, `--whispercpp`) are auto-detected based on system capabilities

**Examples:**

```bash
# Load a model with default options
lemonade load Qwen3-0.6B-GGUF

# Load a model with custom context size
lemonade load Qwen3-0.6B-GGUF --ctx-size 8192

# Load a model and save options for future use
lemonade load Qwen3-0.6B-GGUF --ctx-size 4096 --save-options

# Load a llama.cpp model with custom backend
lemonade load Qwen3-0.6B-GGUF --llamacpp vulkan

# Load a llama.cpp model with custom arguments
lemonade load Qwen3-0.6B-GGUF --llamacpp-args "--flash-attn on --no-mmap"

# Load an image generation model with custom settings
lemonade load Z-Image-Turbo --sdcpp rocm --steps 8 --cfg-scale 1 --width 1024 --height 1024
```

## Options for run

The `run` command is similar to [`load`](#options-for-load) but additionally opens the web app in the browser after loading the model. It takes the same arguments as `load` and opens the URL of the Lemonade Web App in the default browser.

**Examples:**

```bash
# Load a model and open the web app in the browser
lemonade run Qwen3-0.6B-GGUF

# Load a model with custom context size and open the web app
lemonade run Qwen3-0.6B-GGUF --ctx-size 8192

# Load a model on a different host and open the web app
lemonade run Qwen3-0.6B-GGUF --host 192.168.1.100 --port 13305
```

## Options for export

The `export` command exports model information to JSON format. This is useful for backing up model configurations or sharing model metadata:

```bash
lemonade export MODEL_NAME [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `--output FILE` | Output file path. If not specified, prints to stdout | No |

**Notes:**
- The exported JSON includes model metadata such as `model_name`, `recipe`, `checkpoint`, and `labels`
- The CLI automatically prepends `user.` to model names if not already present
- Unrecognized fields in the model data are removed during export

**Examples:**

```bash
# Export model info to stdout
lemonade export Qwen3-0.6B-GGUF

# Export model info to a file
lemonade export Qwen3-0.6B-GGUF --output my-model.json

# Export and view the JSON output
lemonade export Qwen3-0.6B-GGUF --output model.json && cat model.json
```

## Options for backends

The `backends` command lists available recipes and their backends. Use the `install` and `uninstall` subcommands to manage them:

```bash
lemonade backends
lemonade backends install SPEC [--force]
lemonade backends uninstall SPEC
```

| Command | Description |
|--------|-------------|
| `lemonade backends` | List available recipes and backends |
| `lemonade backends install SPEC` | Install a backend. Format: `recipe:backend` (e.g., `llamacpp:vulkan`) |
| `lemonade backends uninstall SPEC` | Uninstall a backend. Format: `recipe:backend` (e.g., `llamacpp:cpu`) |
| `lemonade backends install SPEC --force` | Bypass hardware filtering and attempt the install anyway |

**Notes:**
- Available backends depend on your system and the recipe
- Use `lemonade backends` to list all available recipes and backends

**Examples:**

```bash
# List all available recipes and backends
lemonade backends

# Install Vulkan backend for llamacpp
lemonade backends install llamacpp:vulkan

# Uninstall CPU backend for llamacpp
lemonade backends uninstall llamacpp:cpu

# Install FLM backend
lemonade backends install flm:npu

# Install an otherwise filtered backend
lemonade backends install llamacpp:rocm --force
```

## Options for launch

The `launch` command launches an agent and triggers model loading asynchronously. If no model is provided, launch prompts for recipe/model selection before starting the agent:

```bash
lemonade launch AGENT [--model MODEL_NAME] [options]
```

| Option/Argument | Description | Required |
|-----------------|-------------|----------|
| `AGENT` | Agent name to launch. Supported agents: `claude`, `codex`, `opencode` | Yes |
| `--model MODEL_NAME` | Model name to launch with. If omitted, you will be prompted to select one. | No |
| `--directory DIR` | Remote recipes directory used only if you choose recipe import at prompt | No |
| `--recipe-file FILE` | Remote recipe JSON filename used only if you choose recipe import at prompt | No |
| `--provider,-p [PROVIDER]` | Codex only: select provider name for Codex config; Lemonade does not read or modify `config.toml` (defaults to `lemonade`) | No |
| `--agent-args ARGS` | Custom arguments to pass directly to the launched agent process | `""` |
| `--ctx-size SIZE` | Context size for the model | `4096` |
| `--llamacpp BACKEND` | LlamaCpp backend to use | Auto-detected |
| `--llamacpp-args ARGS` | Custom arguments to pass to llama-server (must not conflict with managed args) | `""` |

**Notes:**
- The model load request is asynchronous: launch starts the agent immediately while loading continues in the background.
- If a model is already provided, launch skips recipe import prompts.
- `--directory` and `--recipe-file` are only used for remote recipe import at prompt time.
- For local recipe files, run `lemonade import <LOCAL_RECIPE_JSON>` first, then launch with the imported model id.
- `--api-key` is propagated to the launched agent process.
- For `codex`, launch now injects a Lemonade model provider by default so host/port settings are honored.
- `--provider` is passed directly to Codex as `model_provider`; provider resolution/errors are handled by Codex.
- `--agent-args` is parsed and appended to the launched agent command.
- Supported agents: `claude`, `codex`, `opencode`
- `opencode` uses an auto-managed config file at `~/.config/opencode/opencode.json`.
- When no `--api-key` is provided, the generated opencode provider uses a default `apiKey` value of `lemonade`.

**Examples:**

```bash
# Launch an agent with default model settings
lemonade launch claude --model Qwen3.5-0.8B-GGUF

# Launch an agent with custom context size
lemonade launch claude --model Qwen3.5-0.8B-GGUF --ctx-size 32768

# Launch an agent with a specific llama.cpp backend
lemonade launch codex --model Qwen3.5-0.8B-GGUF --llamacpp vulkan

# Launch codex using provider from your Codex config.toml (default provider: lemonade)
lemonade launch codex --model Qwen3.5-0.8B-GGUF -p

# Launch codex using a custom provider name from your Codex config.toml
lemonade launch codex --model Qwen3.5-0.8B-GGUF --provider my-provider

# Launch an agent with custom llama.cpp arguments
lemonade launch claude --model Qwen3.5-0.8B-GGUF --ctx-size 32768 --llamacpp-args "--flash-attn on --no-mmap"

# Pass additional arguments directly to the agent
lemonade launch claude --model Qwen3.5-0.8B-GGUF --agent-args "--approval-mode never"

# Resume from previous session
lemonade launch codex --model Qwen3.5-0.8B-GGUF --agent-args "resume SESSION_ID"

lemonade launch claude --model Qwen3.5-0.8B-GGUF --agent-args "--resume SESSION_ID"

# Launch and allow optional prompt-driven recipe import using prefilled remote recipe flags
lemonade launch claude --directory coding-agents --recipe-file Qwen3.5-35B-A3B-NoThinking.json
```

## Options for scan

The `scan` command scans for network beacons on the local network. Beacons are UDP broadcasts sent by Lemonade Server instances to announce their presence. This command listens for these beacons and displays any discovered servers:

```bash
lemonade scan [options]
```

| Option | Description | Default |
|--------|-------------|---------|
| `--duration SECONDS` | Scan duration in seconds | `30` |

**Notes:**
- The scan listens on UDP port 13305 for beacon broadcasts
- Each beacon must contain `service`, `hostname`, and `url` fields in JSON format
- Duplicate beacons (same URL) are automatically filtered out
- The scan runs for the specified duration, collecting all beacons during that time

**Examples:**

```bash
# Scan for beacons for the default duration (30 seconds)
lemonade scan

# Scan for beacons for a custom duration
lemonade scan --duration 5
```

## Next Steps

The [Lemonade Server API documentation](./server/server_spec.md) provides more information about the endpoints that the CLI interacts with. For details on model formats and recipes, see the [custom model guide](./server/custom-models.md).
