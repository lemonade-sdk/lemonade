# Embeddable Lemonade: Backends

This guide discusses how to set up and manage backends for `lemond`. Backends are the software that implements inference, such as `llama.cpp`, `whisper.cpp`, `FastFlowLM`, etc. `lemond` can install backends on your behalf, or it can utilize backends that are already part of your app. You can also download backends at packaging time, install time, or runtime.

Contents:

- [Setting Up Lemonade's Backends](#setting-up-lemonades-backends)
  - [Customizing Backend Versions](#customizing-backend-versions)
  - [Bundling Backends at Packaging Time](#bundling-backends-at-packaging-time)
  - [Installing Backends at Install-Time or Runtime](#installing-backends-at-install-time-or-runtime)
- [Bring Your Own Backends](#bring-your-own-backends)

## Setting Up Lemonade's Backends

### Customizing Backend Versions

Each version of `lemond` ships with recommended version numbers for each support backend, which can be found in `resources/backend_versions.json`. For example, `lemond v10.0.1` recommends `ggml-org/llama.cpp` version `b8460`, `FastFlowLM v0.9.36`, etc.

These backend versions have been validated against that specific release of `lemond` to ensure compatibility, and represent a good starting point for you app. However, you can also customize `backend_versions.json` to your requirements. If you change any backend version, simply restart `lemond` and run any install, load, or inference request against that backend to trigger the new backend version to install.

### Bundling Backends at Packaging Time

Follow these instructions if you want backends to be bundled into your app's installer:

1. Start `lemond ./` on the system where you are packaging your app.
2. Run `lemonade backends` to see the full set of supported backends.
3. `lemonade backends install BACKEND:DEVICE` for each backend.


=== "Windows (cmd.exe)"

    ```cmd
    REM Start lemond to download backends to ./bin/
    lemond.exe ./

    REM Download llama.cpp with the Vulkan backend to ./bin/llamacpp/vulkan
    lemonade.exe backends install llamacpp:vulkan
    ```

=== "Linux (bash)"

    ```bash
    # Start lemond to download backends to ./bin/
    ./lemond ./

    # Download llama.cpp with the Vulkan backend to ./bin/llamacpp/vulkan
    ./lemonade backends install llamacpp:vulkan --force
    ```

> Note: by default, `lemond backends install` will only install backends that are compatible with your current system. The `--force` option ignores these compatibility checks, which enables you to package on a VM and then deploy to a specific system.

#### Limitations
At the time of this writing:
-  `flm` is not available for packaging-time bundling *on Linux*.
- `llamacpp:rocm` is not available for packaging-time bundling on any OS.
- `vllm:rocm` is not available for packaging-time bundling on any OS — the install flow constructs a per-GPU-target release tag at runtime, so the host doing the packaging would need to share its `gfx_target` with the deployment machine.

### Installing Backends at Install-Time or Runtime

You can install backends either during your app's installer or first-run flow, or later while the app is running. In both cases, start by calling [`GET /v1/system-info`](../api/lemonade.md#get-v1system-info) on the target machine. The response tells you which backends are supported on that specific system.

This is useful when the correct backend depends on the user's hardware. For example, you can prefer `llamacpp:rocm` when ROCm is supported, and fall back to `llamacpp:vulkan` otherwise.

Example flow:

1. Launch `lemond`.
2. Call `/v1/system-info`.
3. Check `recipes.llamacpp.backends.rocm.devices` or `recipes.llamacpp.backends.rocm.state`.
4. If ROCm is supported, call `POST /v1/install` with `{"recipe":"llamacpp","backend":"rocm"}`.
5. Otherwise, call `POST /v1/install` with `{"recipe":"llamacpp","backend":"vulkan"}`.

For example:

=== "Windows (cmd.exe)"

    ```cmd
    curl http://localhost:8000/v1/system-info
    ```

=== "Linux (bash)"

    ```bash
    curl http://localhost:8000/v1/system-info
    ```

If the response shows ROCm support:

```json
{
  "recipes": {
    "llamacpp": {
      "backends": {
        "rocm": {
          "devices": ["amd_igpu"],
          "state": "installable"
        }
      }
    }
  }
}
```

Install ROCm:

=== "Windows (cmd.exe)"

    ```cmd
    curl -X POST http://localhost:8000/v1/install ^
      -H "Content-Type: application/json" ^
      -d "{\"recipe\": \"llamacpp\", \"backend\": \"rocm\", \"stream\": false}"
    ```

=== "Linux (bash)"

    ```bash
    curl -X POST http://localhost:8000/v1/install \
      -H "Content-Type: application/json" \
      -d '{
        "recipe": "llamacpp",
        "backend": "rocm",
        "stream": false
      }'
    ```

Otherwise, install Vulkan:

=== "Windows (cmd.exe)"

    ```cmd
    curl -X POST http://localhost:8000/v1/install ^
      -H "Content-Type: application/json" ^
      -d "{\"recipe\": \"llamacpp\", \"backend\": \"vulkan\", \"stream\": false}"
    ```

=== "Linux (bash)"

    ```bash
    curl -X POST http://localhost:8000/v1/install \
      -H "Content-Type: application/json" \
      -d '{
        "recipe": "llamacpp",
        "backend": "vulkan",
        "stream": false
      }'
    ```

See the [Endpoints Spec](../api/README.md) for endpoint details.

## Bring Your Own Backends

You can provide `lemond` the path to your own backend binaries with the following settings. This will cause `lemond` to use your custom backend binaries instead of downloading its own. This is useful if you have a highly customized backend binary you want to use, or if you want to share backend binaries between `lemond` and other software in your application.

For example, to use your own Vulkan `llama-server` in place of Lemonade's:

=== "Windows (cmd.exe)"

    ```cmd
    REM Start lemond to update configuration
    lemond.exe ./

    REM Set the llama-server vulkan binary path
    lemonade.exe config set "llamacpp.vulkan_bin=C:\path\to\llama-server.exe"
    ```

=== "Linux (bash)"

    ```bash
    # Start lemond to update configuration
    ./lemond ./

    # Set the llama-server vulkan binary path
    ./lemonade config set llamacpp.vulkan_bin=/path/to/llama-server
    ```

See the `*_bin` settings in the [Configuration Guide](../guide/configuration/README.md) for the full set of customization options.

### IFM K2-Horizon with a custom llama.cpp build

K2-Horizon can use the existing `llamacpp` recipe with a build of IFM's
[`model/K2Horizon` branch](https://github.com/MBZUAI-IFM/llama.cpp/tree/model/K2Horizon).
No Lemonade source changes or managed-backend version updates are required.

The following is a complete macOS source-build example. From the Lemonade
source tree, check the dependencies and build the server and CLI:
`setup.sh` recreates `build/`, so move any local artifacts there first.

```bash
./setup.sh
cmake --preset default -DBUILD_WEB_APP=OFF -DBUILD_TESTING=OFF
cmake --build --preset default --target lemond lemonade -j
```

If `setup.sh` reaches the pre-commit installation step and exits because the
repository has `core.hooksPath` configured, that does not prevent the runtime
build after all dependency checks have passed; continue with the CMake commands.
This minimal build intentionally omits the desktop and web apps. The message
"This build of Lemonade has been built without a desktop app or a web app" at
the server root is expected and does not affect the REST API.

To include the browser UI instead, configure with `BUILD_WEB_APP=ON`, build the
`web-app` target, and restart `lemond`:

```bash
cmake --preset default -DBUILD_WEB_APP=ON -DBUILD_TESTING=OFF
cmake --build --preset default --target lemond lemonade web-app -j
```

The UI is then available at the server root. A native desktop app has additional
prerequisites; see the [application development guide](../dev/app.md).

Build `llama-server` from the Lemonade source tree using the branch's
[build instructions](https://github.com/MBZUAI-IFM/llama.cpp/blob/model/K2Horizon/docs/build.md)
for your accelerator. For example, on macOS with Metal:

```bash
git clone --branch model/K2Horizon --single-branch https://github.com/MBZUAI-IFM/llama.cpp.git llama.cpp-ifm
cmake -S llama.cpp-ifm -B llama.cpp-ifm/build -DGGML_METAL=ON -DCMAKE_BUILD_TYPE=Release
cmake --build llama.cpp-ifm/build --config Release --target llama-server -j
```

Start Lemonade on an explicit port in one terminal:

```bash
./build/lemond --port 13305
```

In a second terminal, select the matching backend and the **full executable
path** (not its directory). Keep the build's shared libraries alongside the
executable. Supplying `--port` makes the source-built CLI use the same server
without relying on discovery.

```bash
./build/lemonade --port 13305 config set \
  llamacpp.backend=metal \
  "llamacpp.metal_bin=$(pwd)/llama.cpp-ifm/build/bin/llama-server"
```

For a Vulkan build on Windows or Linux, use `llamacpp.backend=vulkan` and
`llamacpp.vulkan_bin` instead; Windows builds normally place `llama-server.exe`
under `build/bin/Release`. The override applies to all models using that backend.
Unload any already-loaded model before switching binaries.

Register and download all non-Uno models supported by the IFM llama.cpp branch
through the existing [custom-model CLI](../guide/configuration/custom-models.md):

```bash
./build/lemonade --port 13305 pull user.K2-Horizon-0.9B \
  --recipe llamacpp \
  --source huggingface \
  --checkpoint main IFM/K2-Horizon-0.9B-GGUF:K2-Horizon-1B-BF16.gguf

./build/lemonade --port 13305 pull user.K2-Horizon-3.7B \
  --recipe llamacpp \
  --source huggingface \
  --checkpoint main IFM/K2-Horizon-3.7B-GGUF:K2-Horizon-4B-BF16.gguf

./build/lemonade --port 13305 pull user.K2-Horizon-7B \
  --recipe llamacpp \
  --source huggingface \
  --checkpoint main IFM/K2-Horizon-7B-GGUF:K2-Horizon-7B-BF16.gguf

./build/lemonade --port 13305 pull user.K2-Horizon-32B \
  --recipe llamacpp \
  --source huggingface \
  --checkpoint main IFM/K2-Horizon-32B-GGUF:K2-Horizon-32B-BF16.gguf

./build/lemonade --port 13305 pull user.K2-Horizon-MoVA-36B-A4B \
  --recipe llamacpp \
  --source huggingface \
  --checkpoint main IFM/K2-Horizon-MoVA-36B-A4B-GGUF:K2-Horizon-36B-BF16.gguf
```

These BF16 weights require approximately 175 GB of disk space in total. The
maximum context windows come from the corresponding model cards and are also
stored in the GGUF metadata:

| Name | BF16 weight size | Maximum context |
|---|---:|---:|
| `user.K2-Horizon-0.9B` | 2.16 GB | 131,072 tokens (128K) |
| `user.K2-Horizon-3.7B` | 10.13 GB | 524,288 tokens (512K) |
| `user.K2-Horizon-7B` | 18.01 GB | 524,288 tokens (512K) |
| `user.K2-Horizon-32B` | 69.57 GB | 524,288 tokens (512K) |
| `user.K2-Horizon-MoVA-36B-A4B` | 74.92 GB | 524,288 tokens (512K) |

The 375B model is intentionally not implemented in the IFM llama.cpp branch
and cannot be used through this integration.

The custom-model registration does not need a separate context-length option.
Use `ctx_size` when loading to select the runtime context window. For example,
load the 0.9B model at its full model-card limit:

```bash
curl http://localhost:13305/v1/load -H "Content-Type: application/json" \
  -d '{"model_name":"user.K2-Horizon-0.9B","ctx_size":131072}'
curl http://localhost:13305/v1/chat/completions -H "Content-Type: application/json" \
  -d '{"model":"user.K2-Horizon-0.9B","messages":[{"role":"user","content":"What is 2 + 2?"}],"reasoning_effort":"high","max_tokens":1024}'
```

For any other model in the table, use its model name and `"ctx_size":524288`.
These are model-supported maxima, not hardware recommendations. KV cache and
compute buffers require substantial additional memory; choose a smaller
`ctx_size` if the full window does not fit on the target system.

These are custom models, not a claim of support in Lemonade's managed binaries.

To return to Lemonade's managed Metal binary, unload the model and run
`./build/lemonade --port 13305 config set llamacpp.metal_bin=builtin` (use the
corresponding `*_bin` key for another backend).
