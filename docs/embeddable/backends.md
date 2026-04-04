# Embeddable Lemonade: Backends

This guide discusses how to set up and manage backends for `lemond`. Backends are the software that implements inference, such as `llama.cpp`, `whisper.cpp`, `FastFlowLM`, etc. `lemond` can install backends on your behalf, or it can utilize backends that are already part of your app. You can also download backends at packaging time, install time, or runtime.

## Setting Up Lemonade's Backends

### Customizing Backend Versions

Each version of `lemond` ships with recommended version numbers for each support backend, which can be found in `resources/backend_versions.json`. For example, `lemond v10.0.1` recommends `ggml-org/llama.cpp` version `b8460`, `FastFlowLM v0.9.36`, etc.

These backend versions have been validated against that specific release of `lemond` to ensure compatibility, and represent a good starting point for you app. However, you can also customize `backend_versions.json` to your requirements. If you change any backend version, simply restart `lemond` and run any install, load, or inference request against that backend to trigger the new backend version to install.

### Bundling Backends at Packaging Time

Follow these instructions if you want backends to be bundled into your app's installer:

1. Start `lemond ./` on the system where you are packaging your app.
2. Run `lemonade recipes` to see the full set of supported backends.
3. `lemonade recipes --install BACKEND` for each backend.


```
# Start lemond to download backends to ./bin/
lemond ./

# Download llama.cpp with the Vulkan backend to ./bin/llamacpp/vulkan
lemonade recipes --install llamacpp:vulkan
```

## Bring Your Own Backends

You can provide `lemond` the path to your own backend binaries with the following settings. This will cause `lemond` to use your custom backend binaries instead of downloading its own. This is useful if you have a highly customized backend binary you want to use, or if you want to share backend binaries between `lemond` and other software in your application.

For example, to use your own Vulkan `llama-server` in place of Lemonade's:

```
# Start lemond to update configuration
lemond ./

# Set the llama-server vulkan binary path
lemonade config set llamacpp.vulkan_bin /path/to/bins
```

See the `*_bin` settings in the [Configuration Guide](../server/configuration.md) for the full set of customization options.
