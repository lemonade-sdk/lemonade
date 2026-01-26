# `lemonade-server` CLI

The `lemonade-server` command-line interface (CLI) provides a set of utility commands for managing the server. When you install, `lemonade-server` is added to your PATH so that it can be invoked from any terminal.

**Contents:**

- [Commands](#commands)
- [Options for serve and run](#options-for-serve-and-run)
  - [Environment Variables](#environment-variables) | [Custom Backend Binaries](#custom-backend-binaries) | [API Key and Security](#api-key-and-security)
- [Options for pull](#options-for-pull)
- [Lemonade Desktop App](#lemonade-desktop-app) | [Remote Server Connection](#remote-server-connection)

## Commands

`lemonade-server` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `-v`, `--version`   | Print the `lemonade-sdk` package version used to install Lemonade Server. |
| `serve`             | Start the server process in the current terminal. See command options [below](#options-for-serve-and-run). |
| `status`            | Check if server is running. If it is, print the port number. |
| `stop`              | Stop any running Lemonade Server process. |
| `pull MODEL_NAME`   | Install an LLM named `MODEL_NAME`. See [pull command options](#options-for-pull) for registering custom models. |
| `run MODEL_NAME`    | Start the server (if not already running) and chat with the specified model. Supports the same options as `serve`. |
| `list`              | List all models. |
| `delete MODEL_NAME` | Delete a model and its files from local storage. |


Examples:

```bash
# Start server with custom settings
lemonade-server serve --port 8080 --log-level debug --llamacpp vulkan

# Run a specific model with custom server settings
lemonade-server run Qwen3-0.6B-GGUF --port 8080 --log-level debug --llamacpp rocm
```

## Options for serve and run

When using the `serve` command, you can configure the server with these additional options. The `run` command supports the same options but also requires a `MODEL_NAME` parameter:

```bash
lemonade-server serve [options]
lemonade-server run MODEL_NAME [options]
```

| Option                         | Description                         | Default |
|--------------------------------|-------------------------------------|---------|
| `--port [port]`                | Specify the port number to run the server on | 8000 |
| `--host [host]`                | Specify the host address for where to listen connections | `localhost` |
| `--log-level [level]`          | Set the logging level               | info |
| `--no-tray`                    | Start server without the tray app (headless mode) | False |
| `--llamacpp [vulkan\|rocm\cpu]`    | Default LlamaCpp backend to use when loading models. Can be overridden per-model via the `/api/v1/load` endpoint. | vulkan |
| `--ctx-size [size]`            | Default context size for models. For llamacpp recipes, this sets the `--ctx-size` parameter for the llama server. For other recipes, prompts exceeding this size will be truncated. Can be overridden per-model via the `/api/v1/load` endpoint. | 4096 |
| `--llamacpp-args [args]`       | Default custom arguments to pass to llama-server. Must not conflict with arguments managed by Lemonade (e.g., `-m`, `--port`, `--ctx-size`, `-ngl`). Can be overridden per-model via the `/api/v1/load` endpoint. Example: `--llamacpp-args "--flash-attn on --no-mmap"` | "" |
| `--extra-models-dir [path]`    | Experimental feature. Secondary directory to scan for LLM GGUF model files. Audio, embedding, reranking, and non-GGUF files are not supported, yet. | None |
| `--max-loaded-models [LLMS] [EMBEDDINGS] [RERANKINGS] [AUDIO]` | Maximum number of models to keep loaded simultaneously. Accepts 1, 3, or 4 values for LLM, embedding, reranking, and audio models respectively. Unspecified values default to 1. Example: `--max-loaded-models 3 2 1 1` loads up to 3 LLMs, 2 embedding models, 1 reranking model, and 1 audio model. | `1 1 1 1` |
| `--save-options` | Only available for the run command. Saves the context size, LlamaCpp backend and custom llama-server arguments as default for running this model. Unspecified values will be saved using their default value. | False |

### Environment Variables

These settings can also be provided via environment variables that Lemonade Server recognizes regardless of launch method:

| Environment Variable               | Description                                                                                                                                             |
|------------------------------------|---------------------------------------------------------------------------------------------------------------------------------------------------------|
| `LEMONADE_HOST`                    | Host address for where to listen for connections                                                                                                        |
| `LEMONADE_PORT`                    | Port number to run the server on                                                                                                                        |
| `LEMONADE_LOG_LEVEL`               | Logging level                                                                                                                                           |
| `LEMONADE_LLAMACPP`                | Default LlamaCpp backend (`vulkan`, `rocm`, or `cpu`)                                                                                                   |
| `LEMONADE_CTX_SIZE`                | Default context size for models                                                                                                                         |
| `LEMONADE_LLAMACPP_ARGS`           | Custom arguments to pass to llama-server                                                                                                                |
| `LEMONADE_EXTRA_MODELS_DIR`        | Secondary directory to scan for GGUF model files                                                                                                        |
| `LEMONADE_DISABLE_MODEL_FILTERING` | Set to `1` to disable hardware-based model filtering (e.g., RAM amount, NPU availability) and show all models regardless of system capabilities         |
| `LEMONADE_ENABLE_DGPU_GTT`         | Set to `1` to include GTT for hardware-based model filtering |

#### Custom Backend Binaries

You can provide your own `llama-server`, `whisper-server`, or `ryzenai-server` binary by setting the full path via the following environment variables:

| Environment Variable | Description |
|---------------------|-------------|
| `LEMONADE_LLAMACPP_ROCM_BIN` | Path to custom `llama-server` binary for ROCm backend |
| `LEMONADE_LLAMACPP_VULKAN_BIN` | Path to custom `llama-server` binary for Vulkan backend |
| `LEMONADE_LLAMACPP_CPU_BIN` | Path to custom `llama-server` binary for CPU backend |
| `LEMONADE_WHISPERCPP_BIN` | Path to custom `whisper-server` binary |
| `LEMONADE_RYZENAI_SERVER_BIN` | Path to custom `ryzenai-server` binary for NPU/Hybrid models |

**Note:** These environment variables do not override the `--llamacpp` option. They allow you to specify an alternative binary for specific backends while still using the standard backend selection mechanism.

**Examples:**

On Windows:

```cmd
set LEMONADE_LLAMACPP_VULKAN_BIN=C:\path\to\my\llama-server.exe
lemonade-server serve
```

On Linux:

```bash
export LEMONADE_LLAMACPP_VULKAN_BIN=/path/to/my/llama-server
lemonade-server serve
```

#### API Key and Security

If you expose your server over a network you can use the `LEMONADE_API_KEY` environment variable to set an API key (use a random long string) that will be required to execute any request. The API key will be expected as HTTP Bearer authentication, which is compatible with the OpenAI API.

**IMPORTANT**: If you need to access `lemonade-server` over the internet, do not expose it directly! You will also need to setup an HTTPS reverse proxy (such as nginx) and expose that instead, otherwise all communication will be in plaintext!

## Options for pull

The `pull` command downloads and installs models. For models already in the [Lemonade Server registry](./server_models.md), only the model name is required. To register and install custom models from Hugging Face, use the registration options below:

```bash
lemonade-server pull <model_name> [options]
```

| Option | Description | Required |
|--------|-------------|----------|
| `--checkpoint CHECKPOINT` | Hugging Face checkpoint in the format `org/model:variant`. For GGUF models, the variant (after the colon) is required. Examples: `unsloth/Qwen3-8B-GGUF:Q4_0`, `amd/Qwen3-4B-awq-quant-onnx-hybrid` | For custom models |
| `--recipe RECIPE` | Inference recipe to use. Options: `llamacpp`, `flm`, `oga-cpu`, `oga-hybrid`, `oga-npu` | For custom models |
| `--reasoning` | Mark the model as a reasoning model (e.g., DeepSeek-R1). Adds the 'reasoning' label to model metadata. | No |
| `--vision` | Mark the model as a vision/multimodal model. Adds the 'vision' label to model metadata. | No |
| `--embedding` | Mark the model as an embedding model. Adds the 'embeddings' label to model metadata. For use with the `/api/v1/embeddings` endpoint. | No |
| `--reranking` | Mark the model as a reranking model. Adds the 'reranking' label to model metadata. For use with the `/api/v1/reranking` endpoint. | No |
| `--mmproj FILENAME` | Multimodal projector file for GGUF vision models. Example: `mmproj-model-f16.gguf` | For vision models |

**Notes:**
- Custom model names must use the `user.` namespace prefix (e.g., `user.MyModel`)
- GGUF models require a variant specified in the checkpoint after the colon
- Use `lemonade-server pull --help` to see examples and detailed information

**Examples:**

```bash
# Install a registered model from the Lemonade Server registry
lemonade-server pull Qwen3-0.6B-GGUF

# Register and install a custom GGUF model
lemonade-server pull user.Phi-4-Mini-GGUF \
  --checkpoint unsloth/Phi-4-mini-instruct-GGUF:Q4_K_M \
  --recipe llamacpp

# Register and install a vision model with multimodal projector
lemonade-server pull user.Gemma-3-4b \
  --checkpoint ggml-org/gemma-3-4b-it-GGUF:Q4_K_M \
  --recipe llamacpp \
  --vision \
  --mmproj mmproj-model-f16.gguf

# Register and install an embedding model
lemonade-server pull user.nomic-embed \
  --checkpoint nomic-ai/nomic-embed-text-v1-GGUF:Q4_K_S \
  --recipe llamacpp \
  --embedding
```

For more information about model formats and recipes, see the [API documentation](../lemonade_api.md) and the [server models guide](./server_models.md).

## Lemonade Desktop App

The Lemonade Desktop App provides a graphical interface for chatting with models and managing the server. When installed via the full installer, `lemonade-app` is added to your PATH for easy command-line access.

### Launching the App

```bash
# Launch the app (connects to local server automatically)
lemonade-app

# Launch with a specific server URL
lemonade-app --base-url http://192.168.0.100:8000
```

By default, the app connects to a server running on `localhost` and automatically discovers the port. To connect to a remote server, use the `--base-url` option.

### Remote Server Connection

To connect the app to a server running on a different machine:

1. **Start the server with network access** on the host machine:
   ```bash
   lemonade-server serve --host 0.0.0.0 --port 8000
   ```
   > **Note:** Using `--host 0.0.0.0` allows connections from other machines on the network. Only do this on trusted networks. You can use `LEMONADE_API_KEY` (see above) to manage access on your network.

2. **Launch the app** on the client machine with the `--base-url` flag:
   ```bash
   lemonade-app --base-url http://192.168.0.100:8000
   ```
   Replace `192.168.0.100` with the IP address of the machine running the server.

Alternatively, you can set the `LEMONADE_APP_BASE_URL` environment variable.

| Option / Environment Variable | Description |
|-------------------------------|-------------|
| `--base-url URL` | Connect the app to a server at the specified URL (e.g., `http://192.168.0.100:8000`) |
| `LEMONADE_APP_BASE_URL` | Environment variable alternative to `--base-url`. The command-line flag takes precedence if both are set. |

When no `--base-url` is provided, the app automatically discovers and connects to a local server.

## Next Steps

The [Lemonade Server integration guide](./server_integration.md) provides more information about how these commands can be used to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->
