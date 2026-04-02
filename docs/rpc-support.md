# llama.cpp RPC Client/Server Support

## Context

llama.cpp supports distributed inference via RPC — an `rpc-server` process offloads tensor computation on a remote machine's GPU, and the main `llama-server` connects as an RPC client via `--rpc host1:port,host2:port`. This lets users split model inference across multiple machines.

Lemonade now integrates RPC with two features:
1. **`--rpc` per-model recipe option** — pass RPC server addresses when loading a llamacpp model
2. **`lemonade rpc-server` CLI subcommand** — start the `rpc-server` binary that ships in the llama.cpp release archive

## Changes

### 1. Add `--rpc` recipe option (RPC client side)

**`src/cpp/server/recipe_options.cpp`:**
- Added `{"rpc", ""}` to `DEFAULTS`
- Added `{"rpc", "--rpc"}` to `OPTION_TO_CLI_FLAG`
- Added `"rpc"` to the llamacpp key list in `get_keys_for_recipe()`
- Added `{"--rpc", {{"option_name", "rpc"}, {"type_name", "SERVERS"}, {"envname", "LEMONADE_RPC"}, {"help", "RPC server addresses for distributed inference (host:port,host:port)"}}}` to `CLI_OPTIONS`

**`src/cpp/server/backends/llamacpp_server.cpp`:**
- In `LlamaCppServer::load()`, reads the rpc option: `std::string rpc = options.get_option("rpc");`
- Before the custom args validation block, adds the `--rpc` arg if non-empty:
  ```cpp
  if (!rpc.empty()) {
      push_arg(args, reserved_flags, "--rpc", rpc);
  }
  ```

### 2. Add `lemonade rpc-server` subcommand (RPC server side)

**`src/cpp/cli/main.cpp`:**

Added to `CliConfig`:
```cpp
std::string rpc_host = "0.0.0.0";
int rpc_port = 50052;
std::string rpc_backend;
int rpc_mem = 0;
```

Registered subcommand (in "Server" group):
```cpp
CLI::App* rpc_server_cmd = app.add_subcommand("rpc-server",
    "Start a llama.cpp RPC server for distributed inference")->group("Server");
rpc_server_cmd->add_option("--rpc-host", config.rpc_host, "Host to bind to")->default_val("0.0.0.0");
rpc_server_cmd->add_option("--rpc-port", config.rpc_port, "RPC server port")->default_val(50052);
rpc_server_cmd->add_option("--backend", config.rpc_backend,
    "llamacpp backend (vulkan/rocm/metal/cpu)")->type_name("BACKEND");
rpc_server_cmd->add_option("--mem", config.rpc_mem, "Memory to allocate in MB")->type_name("MB");
```

> Note: `--rpc-host` and `--rpc-port` are used instead of `--host`/`--port` to avoid
> conflicting with the global `--host`/`--port` options (which target the lemonade server).

Added handler `handle_rpc_server_command`:
1. If `--backend` not specified, defaults to `"vulkan"` on Linux/Windows, `"metal"` on macOS
2. Calls `client.install_backend("llamacpp", backend)` to ensure the backend is installed
3. Finds the `rpc-server` binary by searching the llamacpp install directory
   (`get_downloaded_bin_dir() / "llamacpp" / backend`) using `std::filesystem::recursive_directory_iterator`
4. Builds args: `--host <host> --port <port>` and optionally `--mem <mem>`
5. Starts via `ProcessManager::start_process(exe, args, "", true, false, {})` with inherited output
6. Waits for exit via `ProcessManager::wait_for_exit(handle, -1)`

## Files Modified

| File | Change |
|------|--------|
| `src/cpp/server/recipe_options.cpp` | Add `rpc` option to defaults, CLI flags, keys, and CLI_OPTIONS |
| `src/cpp/server/backends/llamacpp_server.cpp` | Pass `--rpc` arg to llama-server subprocess when option is set |
| `src/cpp/cli/main.cpp` | Add `rpc-server` subcommand with handler |

## Usage

### Distributed inference (RPC client)

On the machine running lemonade, pass the RPC server addresses when loading a model:

```bash
# Via CLI
lemonade load my-model --rpc 192.168.1.100:50052

# Multiple RPC servers
lemonade load my-model --rpc 192.168.1.100:50052,192.168.1.101:50052

# Via environment variable
export LEMONADE_RPC=192.168.1.100:50052
lemonade load my-model
```

### Starting an RPC server

On the remote machine(s), start the RPC server:

```bash
# Default: binds to 0.0.0.0:50052 with vulkan backend
lemonade rpc-server

# Custom port and backend
lemonade rpc-server --rpc-port 50053 --backend rocm

# Limit memory usage
lemonade rpc-server --mem 8192
```

## Verification

1. **Build**: `cmake --build --preset default`
2. **Test `--rpc` option**: `lemonade load <model> --rpc 192.168.1.100:50052` — check debug logs show `--rpc` in llama-server args
3. **Test `rpc-server`**: `lemonade rpc-server --rpc-port 50053` — should install backend, find binary, start rpc-server
4. **Regression tests**: `python test/server_cli.py` and `python test/server_endpoints.py`
