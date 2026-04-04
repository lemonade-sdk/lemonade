# Embeddable Lemonade Guide

Embeddable Lemonade is a portable build of the `lemond` service that you can bundle into your app.

## Who is this for?

Use Embeddable Lemonade instead of a global Lemonade Service when you want a cohesive end-to-end experience for users of your app.
- Users only see your installer, icons, etc.
- Prevent users and other apps from directly interacting with `lemond`.
- Keep your models private from the rest of the system.
- Customize `lemond` to your exact specifications, including backend versions, available models, and much more.

## What's in the release artifact?

Embeddable Lemonade is an zip/tarball artifact shipped in Lemonade releases.

- Windows: `lemonade-embeddable-10.1.0-windows-x64.zip`
- Ubuntu: `lemonade-embeddable-10.1.0-ubuntu-x64.tar.gz`

> Note: see TODO for instructions for building your own embeddable Lemonade from source, including for other Linux distros.

Each arcive has the following contents:

- `lemond.exe` / `lemond` executable: your own private Lemoande instance.
- `lemonade.exe` / `lemonade` CLI: useful for configuring and testing `lemond` before you ship. Feel free to exclude this from your shipped app.
- `resources/`
    - `server_models.json`: customizable list of models that `lemond` will show on the `models` endpoint.
    - `backend_versions.json`: customizable list that determines which versions of llama.cpp, FastFlowLM, etc. will be used as backends for `lemond`.
    - `defaults.json`: default values for `lemond`'s `config.json` file. Safe to delete after `config.json` has been initialized.

Keep reading to learn about the many customization options.

## Customization

While you can ship Embeddable Lemonade as-is, there many opportunities to customize it before packaging it into your app.

### How it Works

Many of the customization options rely of `lemond`'s `config.json` file, a persistent store of settings. Learn more about the individual settings in the [configuration guide](./server/configuration.md).

`config.json` is automatically generated based on the values in `resources/defaults.json` the first time `lemond` starts. The positional arg `lemond DIR` determines where `config.json` and other runtime files (e.g., backend binaries) will be located.

In the examples in this guide, we start `lemond ./` to place these files in the same directory as `lemond` itself. Then:

1. We use the `lemonade` CLI's `config set` command to programmatically customize the contents of `config.json` (you can also manually edit `config.json` if you prefer).
2. Use `lemonade recipes --install` to pre-download backends to be bundled in your app.
3. Edit `server_models.json` and `backend_versions.json` to fully customize the experience for your users.
4. You can delete the `lemonade` CLI and `defaults.json` files to minimize the footprint of your app.

Finally, you can place the fully-configured Embeddable Lemonade folder into your app's installer.

### Deployment-Ready Layout

Once you've finished customization, you'll have a portable Lemonade folder ready for deployment with a layout like this:

```
lemond.exe                      # App runs lemond as a subprocess
lemonade.exe                    # Optional: CLI management for lemond
config.json                     # Persistant customized settings for lemond
recipe_options.json             # Per-model customization (e.g., llama args)

resources/
    |- server_models.json       # Customized lemond models list
    |- backend_versions.json    # Customized version numbers for llamacpp, etc.

bin/                            # Pre-downloaded backends bundled into app
    |- llamacpp/                # GPU LLMs, embedding, and reranking
        |- rocm/
            |- llama-server.exe
        |- vulkan/
            |- llama-server.exe
    |- ryzenai-server/          # NPU LLMs
    |- flm/                     # NPU LLMs, embedding, and ASR
    |- sdpp/                    # GPU image generation
    |- whispercpp/              # NPU and GPU ASR

models/                         # Pre-downloaded models bundled into app
    |- hub/                     # Hugging Face standard layout for models
        |- models--unsloth--Qwen3-0.6B-GGUF/
    |- gguf/                    # Additional GGUF files
        |- Qwen3.5-4B-UD-Q4_K_XL.gguf
```

## Next Steps

Reference detailed guides for each of the following subjects:

- [Runtime](./runtime.md): Using `lemond` as a subprocess runtime.
- TODO Configuration: server settings, model directories, backend settings, etc.
- [Backends](./backends.md): deploy backends at packaging time, install time, or runtime.
- TODO Models: bundling, organization, sharing, per-model settings.
- TODO API Key: Ensure only your app can access `lemond`.
