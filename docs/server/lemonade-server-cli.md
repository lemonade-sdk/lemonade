# `lemonade-server` CLI

The `lemonade-server` command-line interface (CLI) provides a set of utility commands for managing the server. When you install Lemonade Server using the GUI installer, `lemonade-server` is added to your PATH so that it can be invoked from any terminal.

> Note: if you installed from source or PyPI, you should call `lemonade-server-dev` in your activated Python environment, instead of using `lemonade-server`.

`lemonade-server` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `-v`, `--version`   | Print the `lemonade-sdk` package version used to install Lemonade Server. |
| `serve`             | Start the server process in the current terminal. See command options [below](#command-line-options-for-serve-and-run). |
| `status`            | Check if server is running. If it is, print the port number. |
| `stop`              | Stop any running Lemonade Server process. |
| `pull MODEL_NAME`   | Install an LLM named `MODEL_NAME`. See the [server models guide](./server_models.md) for more information. |
| `run MODEL_NAME`    | Start the server (if not already running) and chat with the specified model. Supports the same options as `serve`. |
| `list`              | List all models. |


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
| `--llamacpp [vulkan\|rocm]`    | Specify the LlamaCpp backend to use | vulkan |
| `--ctx-size [size]`            | Set the context size for the model. For llamacpp recipes, this sets the `--ctx-size` parameter for the llama server. For other recipes, prompts exceeding this size will be truncated. | 4096 |

These settings can also be provided via environment variables that Lemonade Server recognizes regardless of launch method: `LEMONADE_HOST`, `LEMONADE_PORT`, `LEMONADE_LOG_LEVEL`, `LEMONADE_LLAMACPP`, and `LEMONADE_CTX_SIZE`.

The [Lemonade Server integration guide](./server_integration.md) provides more information about how these commands can be used to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->