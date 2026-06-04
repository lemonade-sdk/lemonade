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
- [Options for bench](#options-for-bench)
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

### Benchmarking

| Command             | Description                         |
|---------------------|-------------------------------------|
| `bench MODEL_NAME`  | Benchmark a model's chat completion performance across backends and context sizes. See command options [below](#options-for-bench). |

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

The `list` command displays all models. By default, the output is partitioned into **Local** (downloaded) and **Available for Download** sections. You can restrict the output to only local models by passing the `--downloaded` flag:

```bash
lemonade list [options]
```

| Option                         | Description                         | Default |
|--------------------------------|-------------------------------------|---------|
| `--downloaded`                 | Show only downloaded models | False |

## Options for pull

The `pull` command downloads and installs models. It can also register a custom `user.*` model when paired with manual configuration flags. For the full end-to-end custom model workflow, including checkpoint formats, omni collections, and `user_models.json`, see [Custom Model Configuration](./configuration/custom-models.md).

Common forms:

```bash
lemonade pull MODEL_OR_CHECKPOINT [--checkpoint TYPE CHECKPOINT] [--recipe RECIPE] [--label LABEL] [--components MODEL ...]
```

```bash
# Pull a registered model
lemonade pull Qwen3-0.6B-GGUF

# Pull a Hugging Face repo and choose a variant interactively
lemonade pull unsloth/Qwen3-8B-GGUF

# Pull a specific Hugging Face variant
lemonade pull unsloth/Qwen3-8B-GGUF:Q4_K_M

# Register and pull a custom model
lemonade pull user.MyModel --checkpoint main org/model:Q4_0 --recipe llamacpp
```

| Option | Description | Required |
|--------|-------------|----------|
| `MODEL_OR_CHECKPOINT` | Registered model name, or `owner/repo[:variant]` Hugging Face checkpoint | Yes |
| `--checkpoint TYPE CHECKPOINT` | Manual registration: add a checkpoint entry. Repeat for multi-component models such as `main` + `mmproj` or `main` + `vae`. | No |
| `--recipe RECIPE` | Manual registration: recipe to associate with the new `user.*` model (`llamacpp`, `flm`, `ryzenai-llm`, `vllm`, `whispercpp`, `sd-cpp`, `kokoro`, `collection.omni`) | No |
| `--label LABEL` | Manual registration: add a label to the new model. Repeatable. Valid: `coding`, `embeddings`, `hot`, `mtp`, `reasoning`, `reranking`, `tool-calling`, `vision` | No |
| `--components MODEL [MODEL ...]` | Omni-model registration: component names to bundle. Use with `--recipe collection.omni`. Components must already be registered (built-in or previously pulled `user.*`); any not-yet-downloaded components are pulled by the same call. | No |

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
| `--llamacpp-device DEVICE` | Comma-separated list of accelerator devices to use (e.g. Vulkan0) | (empty) |
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
| `--merge-args` / `--no-merge-args` | Merge global and model arguments when loading the model | `true` |

**Notes:**
- Use `--save-options` to persist your configuration for the model
- Unspecified options will use the backend's default values
- Backend options (`--llamacpp`, `--sdcpp`, `--whispercpp`) are auto-detected based on system capabilities
- `--merge-args` controls whether `*_args` from global config are merged with per-model args (default: merge). Use `--no-merge-args` to replace global args entirely with per-model args.

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

# Load a model without merging global args (per-model args replace global entirely)
lemonade load Qwen3-0.6B-GGUF --no-merge-args --llamacpp-args "--flash-attn on"

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
lemonade launch [AGENT] [--model MODEL_NAME] [options]
```

| Option/Argument | Description | Required |
|-----------------|-------------|----------|
| `AGENT` | Agent name to launch. Supported agents: `claude`, `codex`, `opencode`. If omitted, you will be prompted to select one. | No |
| `--model MODEL_NAME` | Model name to launch with. If omitted, you will be prompted to select one. | No |
| `--directory DIR` | Remote recipes directory used only if you choose recipe import at prompt | No |
| `--recipe-file FILE` | Remote recipe JSON filename used only if you choose recipe import at prompt | No |
| `--agent-args ARGS` | Custom arguments to pass directly to the launched agent process | `""` |

Codex-only option:

| Option/Argument | Description | Required |
|-----------------|-------------|----------|
| `--provider,-p [PROVIDER]` | Select provider name for Codex config; Lemonade does not read or modify `config.toml` (defaults to `lemonade`) | No |

**Notes:**
- The model load request is asynchronous: launch starts the agent immediately while loading continues in the background.
- If a model is already provided, launch skips recipe import prompts.
- `--directory` and `--recipe-file` are only used for remote recipe import at prompt time.
- For local recipe files, run `lemonade import <LOCAL_RECIPE_JSON>` first, then launch with the imported model id.
- `--api-key` is propagated to the launched agent process.
- For `codex`, launch now injects a Lemonade model provider by default so host/port settings are honored.
- `--provider` is accepted only by `lemonade launch codex` and is passed directly to Codex as `model_provider`; provider resolution/errors are handled by Codex.
- Existing `LEMONADE_*` recipe env vars such as `LEMONADE_CTX_SIZE` are still honored by `launch`.
- `--agent-args` is parsed and appended to the launched agent command.
- Supported agents: `claude`, `codex`, `opencode`
- `opencode` uses an auto-managed config file at `~/.config/opencode/opencode.json`.
- When no `--api-key` is provided, the generated opencode provider uses a default `apiKey` value of `lemonade`.

**Examples:**

```bash
# Launch an agent with default model settings
lemonade launch claude --model Qwen3.5-0.8B-GGUF

# Launch codex using provider from your Codex config.toml (default provider: lemonade)
lemonade launch codex --model Qwen3.5-0.8B-GGUF -p

# Launch codex using a custom provider name from your Codex config.toml
lemonade launch codex --model Qwen3.5-0.8B-GGUF --provider my-provider

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

## Options for bench

The `bench` command measures chat completion performance (TTFT and tokens-per-second) for a given model across one or more installed backends, context sizes, and scenario workloads. It sends `POST /api/v1/chat/completions` requests and extracts timing data from the server response.

```
lemonade bench [options] MODEL_NAME
```

### Required Arguments

| Argument | Description |
|----------|-------------|
| `MODEL_NAME` | Registered model name to benchmark |

### Options

| Option | Description | Default |
|--------|-------------|---------|
| `--backend BACKEND` | Backend to test (e.g., `vulkan`, `metal`, `cpu`). Repeat for multiple backends. | All installed backends |
| `--ctx-size SIZE` | Context size to test. Repeat for multiple sizes. | Model's default context size |
| `--runs N` | Number of measurement runs per scenario | `3` |
| `--warmup N` | Number of warmup runs per scenario (not included in stats) | `0` |
| `--scenarios NAME\|CATEGORY` | Scenario name(s) or category (e.g. `chat`, `coding`, `long-context`). Use `all` to include every scenario. Repeat for multiple. | All scenarios except `long-context` |
| `--scenario-file FILE` | Load scenarios from a single JSON file | Default bundled scenarios |
| `--scenario-dir DIR` | Load all `.json` scenario files from a directory | — |
| `--json` | Output results as JSON instead of a table | Table output |
| `--output FILE` | Write results to file (in addition to stdout) | — |
| `--compare FILE` | Compare results against a previously saved JSON file | — |
| `--auto-pull` | Automatically pull the model if not downloaded | False |
| `--no-memory` | Disable VRAM/RAM tracking | Tracking enabled |
| `--no-reload` | Skip model reload between scenarios (faster, but prompt cache may skew results) | Model reloaded |
| `--llamacpp-args ARGS` | Custom args for llama-server (e.g. `"-b 2048 -ub 1024"`). Repeat for multiple arg sets. | — |
| `--flm-args ARGS` | Custom args for flm serve. Repeat for multiple. | — |
| `--vllm-args ARGS` | Custom args for vllm-server. Repeat for multiple. | — |

### Scenario Files

Scenarios are defined in JSON files. Each file contains a `scenarios` array where each entry specifies a workload:

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Unique scenario name (required) |
| `category` | string | Category label for grouping (default: `"general"`) |
| `messages` | array | Chat messages in OpenAI format (required) |
| `max_tokens` | int | Maximum output tokens (default: `128`) |
| `warmup_runs` | int | Override warmup runs for this scenario (default: `0`) |
| `measurement_runs` | int | Override measurement runs for this scenario (default: `3`) |
| `context` | object | Optional context expansion block (see below) |

#### Context Expansion

The optional `context` block generates large input contexts by repeating a filler string to a target token count. This is useful for benchmarking long-context performance (e.g., needle-in-a-haystack tests):

| Field | Type | Description |
|-------|------|-------------|
| `filler` | string | Text to repeat to build context |
| `target_tokens` | int | Target token count (~4 chars per token for English) |
| `position` | string | Where to place the question relative to filler: `"end"` (default), `"start"`, or `"middle"` |

When `context` is present, the user message content is replaced with the expanded text. The question (extracted from the user message) is positioned relative to the filler according to `position`.

#### Default Scenarios

Lemonade ships with a bundled set of scenarios (`bench_scenarios.json`) covering:
- **Chat** — Short and long conversational turns
- **Coding** — Code generation, explanation, and debugging
- **Long-context** — 32K, 64K, 128K context windows and multi-turn conversation memory

You can override these with `--scenario-file` or `--scenario-dir`.

**Note:** Long-context scenarios (`context-32k`, `context-64k`, `context-128k`, `context-multi-turn`) are excluded by default because they run very long. Use `--scenarios long-context` to include them, or `--scenarios all` to run every scenario.

### Output

#### Table Output (default)

Results are printed as a table grouped by backend. Columns show TTFT (Time To First Token) and TPS (Tokens Per Second) with mean, min/max (or p50/p95 when `--runs >= 10`), and peak VRAM usage:

```
Benchmark: Qwen3-0.6B-GGUF
====================================================================================================

Backend: llamacpp/vulkan (ctx=4096)
----------------------------------------------------------------------------------------------------
Scenario            TTFT    min     max     TPS     min     max     VRAM (GB)
----------------------------------------------------------------------------------------------------
chat-short          45.2    42.1    48.3    185.3   178.2   192.1   1.2
chat-long-output    48.7    46.5    51.2    142.6   138.4   147.8   1.2
code-short          46.1    44.3    47.8    168.9   162.3   175.4   1.2
```

#### JSON Output

With `--json`, results are emitted as structured JSON. Use `--output FILE` to save them for later comparison with `--compare`.

### Comparison Mode

Pass `--compare PREVIOUS.json` to compare current results against a saved JSON file. This shows percentage change in TTFT and TPS, plus VRAM delta:

```bash
# Save a baseline
lemonade bench --output baseline.json Qwen3-0.6B-GGUF

# Later, compare after a change
lemonade bench --compare baseline.json Qwen3-0.6B-GGUF
```

The comparison table marks each scenario as:
- **matched** — compared against previous data
- **new** — no previous data available
- **removed** — was in previous run but not in current
- **failed** — all measurement runs errored
- **prev_failed** — previous run errored, no baseline

**Note:** TTFT change > 0 means slower (worse). TPS change > 0 means faster (better).

### Examples

```bash
# Benchmark with default scenarios and all installed backends
lemonade bench Qwen3-0.6B-GGUF

# Benchmark specific backends and context sizes
lemonade bench --backend vulkan --backend cpu --ctx-size 4096 8192 Qwen3-0.6B-GGUF

# Run with custom scenarios
lemonade bench --scenario-file my-scenarios.json Qwen3-0.6B-GGUF

# Run only specific scenarios by name
lemonade bench --scenarios chat-short --scenarios code-debug Qwen3-0.6B-GGUF

# Run all scenarios in a category
lemonade bench --scenarios coding Qwen3-0.6B-GGUF

# Include long-context scenarios (excluded by default)
lemonade bench --scenarios all Qwen3-0.6B-GGUF

# Benchmark with custom backend arguments (e.g., different batch sizes)
lemonade bench --llamacpp-args "-b 1024" "-b 2048" Qwen3-0.6B-GGUF

# Save results as JSON
lemonade bench --json --output results.json Qwen3-0.6B-GGUF

# Compare against a previous run
lemonade bench --compare results.json Qwen3-0.6B-GGUF

# Run more measurement iterations for statistical significance
lemonade bench --runs 20 Qwen3-0.6B-GGUF
```

### Example Custom Scenario File

This is an example of a scenario which can be passed `--scenario-file`:

```json
{
  "scenarios": [
    {
      "name": "hello-world",
      "category": "chat",
      "messages": [
        { "role": "system", "content": "You are a helpful assistant." },
        { "role": "user", "content": "What is 2 + 2?" }
      ],
      "max_tokens": 10
    },
    {
      "name": "write-function",
      "category": "coding",
      "messages": [
        { "role": "system", "content": "You are a coding assistant." },
        { "role": "user", "content": "Write a Python function to reverse a string." }
      ],
      "max_tokens": 50
    },
    {
      "name": "needle-16k",
      "category": "long-context",
      "context": {
        "filler": "The project codename for the secret mission is Bluefin. This is a long filler text that will be repeated to build up a large context window for benchmarking. ",
        "target_tokens": 16000,
        "position": "middle"
      },
      "messages": [
        { "role": "user", "content": "What is the project codename mentioned in this text?" }
      ],
      "max_tokens": 20
    }
  ]
}
```

## Next Steps

The [Lemonade Server API documentation](../api/README.md) provides more information about the endpoints that the CLI interacts with. For details on model formats and recipes, see the [custom model guide](./configuration/custom-models.md).
