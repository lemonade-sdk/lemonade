# `lemonade-server` CLI

The `lemonade-server` command-line interface (CLI) provides a set of utility commands for managing the server. When you install Lemonade Server using the GUI installer, `lemonade-server` is added to your PATH so that it can be invoked from any terminal.

> Note: if you installed from source or PyPI, you should call `lemonade-server-dev` in your activated Python environment, instead of using `lemonade-server`.

`lemonade-server` provides these utilities:

| Option/Command      | Description                         |
|---------------------|-------------------------------------|
| `-v`, `--version`   | Print the `lemonade-sdk` package version used to install Lemonade Server. |
| `serve`             | Start the server process in the current terminal. Use the `--port PORT_NUMBER` option to set the port number. |
| `status`            | Check if server is running. If it is, print the port number. |
| `stop`              | Stop any running Lemonade Server process. |
| `pull MODEL_NAME`   | Install an LLM named `MODEL_NAME`. See the [server models guide](../server_models.md) for more information. |

The [Lemonade Server integration guide](../server_integration.md) provides more information about how these commands can be used to integrate Lemonade Server into an application.
