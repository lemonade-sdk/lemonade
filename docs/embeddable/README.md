# Embeddable Lemonade Guide

Embeddable Lemonade is a portable build of the `lemond` service that you can bundle into your app.

## Who is this for?

Use Embeddable Lemonade instead of a global Lemonade Service when you want a cohesive end-to-end experience for users of your app.
- Users only see your installer, icons, etc.
- Prevent users and other apps from directly interacting with `lemond`.
- Keep your models private from the rest of the system.
- Customize `lemond` to your exact specifications, including backend versions, available models, and much more.

## What's in the release artifact?

Embeddable Lemonade is an zip/tarball artifact shipped in Lemonade releases. It has the following contents:

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

In these examples, we start `lemond ./` to place these files in the same directory as `lemond` itself, but you can choose any path within your application's layout. Next, we use the `lemonade` CLI's `config set` command to programmatically customize the contents of `config.json` (you can also manually edit `config.json` if you prefer). You can also use `lemonade recipes --install` to pre-download backends to be bundled in your app.

Then, we give examples of how you can exit `server_models.json` and `backend_versions.json` to fully customize the experience for your users.

Next, you can delete the `lemonade` CLI and `defaults.json` files to minimize the footprint of your app.

Finally, you can place the fully-configured Embeddable Lemonade folder into your app's installer.

### Customized Layout

TODO

### Bundling Backends

`lemond` can download backends such as `llama-server` on your behalf, or it can utilize backends that are already part of your app. You can also download backends at packaging time, install time, or runtime.

#### Using Pre-Existing Backends

You can provide `lemond` the path to pre-existing backends with the following settings. This will cause `lemond` to use your custom backend binaries instead of downloading its own.

```
# Start lemond to update configuration
lemond ./

# Set the llama-server vulkan binary path
lemonade config set llamacpp.vulkan_bin /path/to/bins

# Works for llamacpp.rocm_bin and llamacpp.cpu_bin as well

# Set the sdcpp binary path
lemonade config set
