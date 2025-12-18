# `lemonade-server` CLI

The `lemonade-server` command-line interface (CLI) provides a set of utility commands for managing the server. When you install Lemonade Server using the GUI installer, `lemonade-server` is added to your PATH so that it can be invoked from any terminal.

`lemonade-server` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `-v`, `--version`   | Print the `lemonade-sdk` package version used to install Lemonade Server. |
| `serve`             | Start the server process in the current terminal. See command options [below](#command-line-options-for-serve-and-run). |
| `status`            | Check if server is running. If it is, print the port number. |
| `stop`              | Stop any running Lemonade Server process. |
| `pull MODEL_NAME`   | Install an LLM named `MODEL_NAME`. See [pull command options](#pull-command-options) for registering custom models. |
| `run MODEL_NAME`    | Start the server (if not already running) and chat with the specified model. Supports the same options as `serve`. |
| `list`              | List all models. |
| `delete MODEL_NAME` | Delete a model and its files from local storage. |


Examples:

```bash
# Start server with custom settings
lemonade-server serve --port 8080 --log-level debug --llamacpp vulkan

# Run a specific model with custom server settings
lemonade-server run llama-3.2-3b-instruct --port 8080 --log-level debug --llamacpp rocm
```

## Command Line Options for `serve` and `run`

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

These settings can also be provided via environment variables that Lemonade Server recognizes regardless of launch method: `LEMONADE_HOST`, `LEMONADE_PORT`, `LEMONADE_LOG_LEVEL`, `LEMONADE_LLAMACPP`, `LEMONADE_CTX_SIZE`, `LEMONADE_LLAMACPP_ARGS`, and `LEMONADE_EXTRA_MODELS_DIR`.

Additionally, you can provide your own `llama-server` binary by giving the full path to it via the following environment variables: `LEMONADE_LLAMACPP_ROCM_BIN`, `LEMONADE_LLAMACPP_VULKAN_BIN`, `LEMONADE_LLAMACPP_CPU_BIN`. Note that this does not override the `--llamacpp` option, rather it allows to provide an alternative binary for specific backends.

The same can also be done for the `whisper-server` binary. The environment variable to set in this case is `LEMONADE_WHISPERCPP_BIN`.

## `pull` Command Options

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

## Next Steps

The [Lemonade Server integration guide](./server_integration.md) provides more information about how these commands can be used to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->