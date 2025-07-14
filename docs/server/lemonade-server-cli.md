# `lemonade-server` CLI

The `lemonade-server` command-line interface (CLI) provides a set of utility commands for managing the server. When you install Lemonade Server using the GUI installer, `lemonade-server` is added to your PATH so that it can be invoked from any terminal.

> Note: if you installed from source or PyPI, you should call `lemonade-server-dev` in your activated Python environment, instead of using `lemonade-server`.

`lemonade-server` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `-v`, `--version`   | Print the `lemonade-sdk` package version used to install Lemonade Server. |
| `serve`             | Start the server process in the current terminal. See command options [below](#command-line-options-for-serve). |
| `status`            | Check if server is running. If it is, print the port number. |
| `stop`              | Stop any running Lemonade Server process. |
| `pull MODEL_NAME`   | Install an LLM named `MODEL_NAME`. See the [server models guide](./server_models.md) for more information. |
| `run MODEL_NAME`    | Start the server (if not already running) and chat with the specified model. |
| `list`              | List all models. |


Example:

```bash
lemonade-server serve --port 8080 --log-level debug --truncate-inputs
```

## Command Line Options for `serve`

When using the `serve` command, you can configure the server with these additional options:

| Option                         | Description                         | Default |
|--------------------------------|-------------------------------------|---------|
| `--port [port]`                | Specify the port number to run the server on | 8000 |
| `--log-level [level]`          | Set the logging level               | info |

The [Lemonade Server integration guide](./server_integration.md) provides more information about how these commands can be used to integrate Lemonade Server into an application.

<!--Copyright (c) 2025 AMD-->