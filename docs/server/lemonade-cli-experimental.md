# `lemonade` CLI (EXPERIMENTAL)

The `lemonade` command-line interface (CLI) provides an HTTP client for interacting with Lemonade Server. It allows you to manage models, recipes, and backends through a simple command-line interface.

**Contents:**

- [Commands](#commands)
- [Global Options](#global-options)
- [Options for list](#options-for-list)
- [Options for pull](#options-for-pull)
- [Options for import](#options-for-import)
- [Options for load](#options-for-load)
- [Options for run](#options-for-run)
- [Options for export](#options-for-export)
- [Options for recipes](#options-for-recipes)
- [Options for launch](#options-for-launch)
- [Options for scan](#options-for-scan)

## Commands

`lemonade` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `--help`            | Display help information. |
| `--help-all`        | Display help information for all subcommands. |
| `--version`         | Print the `lemonade` CLI version. |
| `status`            | Check if server can be reached. If it is, prints server information. |
| `list`              | List all available models. |
| `pull MODEL_NAME`   | Download and install a model. See command options [below](#options-for-pull). |
| `import JSON_FILE`  | Import a model from a JSON configuration file. See command options [below](#options-for-import). |
| `delete MODEL_NAME` | Delete a model and its files from local storage. |
| `load MODEL_NAME`   | Load a model for inference. See command options [below](#options-for-load). |
| `run MODEL_NAME`    | Load a model for inference and open the web app in the browser. See command options [below](#options-for-run). |
| `unload [MODEL_NAME]` | Unload a model. If no model name is provided, unload all loaded models. |
| `recipes`           | List available recipes and backends. Use `--install` or `--uninstall` to manage backends. |
| `export MODEL_NAME` | Export model information to JSON format. See command options [below](#options-for-export). |
| `launch AGENT`      | Launch an agent with a model. See command options [below](#options-for-launch). |
| `scan`              | Scan for network beacons on the local network. See command options [below](#options-for-scan). |

## Global Options

The following options are available for all commands:

| Option | Description | Default |
|--------|-------------|---------|
| `--host HOST` | Server host address | `127.0.0.1` |
| `--port PORT` | Server port number | `8000` |
| `--api-key KEY` | API key for authentication | None |

These options can also be set via environment variables:
- `LEMONADE_HOST` for `--host`
- `LEMONADE_PORT` for `--port`
- `LEMONADE_API_KEY` for `--api-key`

**Examples:**

On Linux/macOS:
```bash
export LEMONADE_HOST=192.168.1.100
export LEMONADE_PORT=8000
export LEMONADE_API_KEY=your-api-key-here
lemonade list
```

On Windows (Command Prompt):
```cmd
set LEMONADE_HOST=192.168.1.100
set LEMONADE_PORT=8000
set LEMONADE_API_KEY=your-api-key-here
lemonade list
```

On Windows (PowerShell):
```powershell
$env:LEMONADE_HOST="192.168.1.100"
$env:LEMONADE_PORT="8000"
$env:LEMONADE_API_KEY="your-api-key-here"
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
lemonade recipes --install llamacpp:vulkan

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

The `pull` command downloads and installs models. For models already in the [Lemonade Server registry](https://lemonade-server.ai/models.html), only the model name is required. To register and install custom models from Hugging Face, use the registration options below:

```bash
lemonade pull MODEL_NAME [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `MODEL_NAME` | Model name to pull (e.g., `Qwen3-0.6B-GGUF` or `user.MyModel`) | Yes |
| `--checkpoint TYPE CHECKPOINT` | Hugging Face checkpoint in the format `org/model:variant`. The `TYPE` specifies the component type. Can be specified multiple times. Valid types: `main`, `mmproj`, `vae`, `text_encoder`. For GGUF models, the variant (after the colon) is required. Examples: `unsloth/Qwen3-8B-GGUF:Q4_0`, `amd/Qwen3-4B-awq-quant-onnx-hybrid` | For custom models |
| `--recipe RECIPE` | Inference recipe to use. Options: `llamacpp`, `flm`, `ryzenai-llm` | For custom models |
| `--label LABEL` | Add a label to the model. Can be specified multiple times. Valid labels: `coding`, `embeddings`, `hot`, `reasoning`, `reranking`, `tool-calling`, `vision` | No |

**Notes:**
- Custom model names must use the `user.` namespace prefix (e.g., `user.MyModel`)
- GGUF models require a variant specified in the checkpoint after the colon
- To import a model from a JSON configuration file, use the [`import`](#options-for-import) command instead

**Examples:**

```bash
# Pull a registered model from the Lemonade Server registry
lemonade pull Qwen3-0.6B-GGUF

# Register and pull a custom GGUF model with main checkpoint
lemonade pull user.Phi-4-Mini-GGUF \
  --checkpoint main unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M \
  --recipe llamacpp

# Register and pull a model with multiple checkpoints (e.g., main model + mmproj)
lemonade pull user.Gemma-3-4b \
  --checkpoint main ggml-org/gemma-3-4b-it-GGUF:Q4_K_M \
  --checkpoint mmproj mmproj-model-f16.gguf \
  --recipe llamacpp

# Register and pull a model with multiple labels
lemonade pull user.MyCodingModel \
  --checkpoint main org/model:Q4_0 \
  --recipe llamacpp \
  --label coding \
  --label tool-calling
```

## Options for import

The `import` command imports a model from a JSON configuration file. This is useful for importing models with complex configurations that would be cumbersome to specify via command-line options:

```bash
lemonade import JSON_FILE
```

| Option | Description | Required |
|--------|-------------|----------|
| `JSON_FILE` | Path to a JSON configuration file | Yes |

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
```

`model-with-multiple-checkpoints.json`:
```json
{
  "model_name": "MyMultimodalModel",
  "checkpoints": {
    "main": "ggml-org/gemma-3-4b-it-GGUF:Q4_K_M",
    "mmproj": "mmproj-model-f16.gguf"
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
lemonade run Qwen3-0.6B-GGUF --host 192.168.1.100 --port 8000
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

## Options for recipes

The `recipes` command lists available recipes and their backends. It also supports installing and uninstalling backends:

```bash
lemonade recipes [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `--install SPEC` | Install a backend. Format: `recipe:backend` (e.g., `llamacpp:vulkan`) | No |
| `--uninstall SPEC` | Uninstall a backend. Format: `recipe:backend` (e.g., `llamacpp:cpu`) | No |

**Notes:**
- Available backends depend on your system and the recipe
- Use `lemonade recipes` without options to list all available recipes and backends

**Examples:**

```bash
# List all available recipes and backends
lemonade recipes

# Install Vulkan backend for llamacpp
lemonade recipes --install llamacpp:vulkan

# Uninstall CPU backend for llamacpp
lemonade recipes --uninstall llamacpp:cpu

# Install FLM backend
lemonade recipes --install flm:npu
```

## Options for launch

The `launch` command launches an agent with a model loaded. It requires an agent name and a model name, and supports recipe-specific options:

```bash
lemonade launch AGENT --model MODEL_NAME [options]
```

| Option/Argument | Description | Required |
|-----------------|-------------|----------|
| `AGENT` | Agent name to launch. Supported agents: `claude`, `codex` | Yes |
| `--model MODEL_NAME` | Model name to load before launching the agent | Yes |
| `--ctx-size SIZE` | Context size for the model | `4096` |
| `--llamacpp BACKEND` | LlamaCpp backend to use | Auto-detected |
| `--llamacpp-args ARGS` | Custom arguments to pass to llama-server (must not conflict with managed args) | `""` |

**Notes:**
- The model is loaded before launching the agent
- Supported agents: `claude`, `codex`

**Examples:**

```bash
# Launch an agent with default model settings
lemonade launch claude --model Qwen3-0.6B-GGUF

# Launch an agent with custom context size
lemonade launch claude --model Qwen3-0.6B-GGUF --ctx-size 8192

# Launch an agent with a specific llama.cpp backend
lemonade launch codex --model Qwen3-0.6B-GGUF --llamacpp vulkan

# Launch an agent with custom llama.cpp arguments
lemonade launch claude --model Qwen3-0.6B-GGUF --ctx-size 4096 --llamacpp-args "--flash-attn on --no-mmap"
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
- The scan listens on UDP port 8000 for beacon broadcasts
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

The [Lemonade Server API documentation](../server_spec.md) provides more information about the endpoints that the CLI interacts with. For details on model formats and recipes, see the [custom model guide](./custom-models.md).
