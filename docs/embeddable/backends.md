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

### Installing Backends at Install-Time or Runtime

You can install backends either during your app's installer or first-run flow, or later while the app is running. In both cases, start by calling [`GET /v1/system-info`](../server/server_spec.md#get-apiv1system-info) on the target machine. The response tells you which backends are supported on that specific system.

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

See the [Server Spec](../server/server_spec.md) for endpoint details.

## Bring Your Own Backends

You can provide `lemond` the path to your own backend binaries with the following settings. This will cause `lemond` to use your custom backend binaries instead of downloading its own. This is useful if you have a highly customized backend binary you want to use, or if you want to share backend binaries between `lemond` and other software in your application.

For example, to use your own Vulkan `llama-server` in place of Lemonade's:

=== "Windows (cmd.exe)"

    ```cmd
    REM Start lemond to update configuration
    lemond.exe ./

    REM Set the llama-server vulkan binary path
    lemonade.exe config set llamacpp.vulkan_bin C:\path\to\bins
    ```

=== "Linux (bash)"

    ```bash
    # Start lemond to update configuration
    ./lemond ./

    # Set the llama-server vulkan binary path
    ./lemonade config set llamacpp.vulkan_bin /path/to/bins
    ```

See the `*_bin` settings in the [Configuration Guide](../server/configuration.md) for the full set of customization options.
