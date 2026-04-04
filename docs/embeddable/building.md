# Embeddable Lemonade: Building from Source

This guide shows how to build the embeddable `lemond` and `lemonade` binaries from source.

For general prerequisites, toolchain setup, and broader development workflows, see [Lemonade Development](../dev-getting-started.md).

Contents:

- [Default Embeddable Build](#default-embeddable-build)
- [Include the Web App](#include-the-web-app)
- [Expected Outputs](#expected-outputs)

## Default Embeddable Build

The [release workflow](../../.github/workflows/cpp_server_build_test_release.yml) builds the embeddable archives with the web app disabled, producing only the server, CLI, and required resource files.

=== "Windows (cmd.exe)"

    ```cmd
    cmake --preset windows -DBUILD_WEB_APP=OFF
    cmake --build --preset windows --target lemond lemonade
    ```

=== "Linux (bash)"

    ```bash
    sudo apt-get update
    sudo apt-get install -y cmake ninja-build g++ pkg-config libssl-dev libdrm-dev
    cmake --preset default -DBUILD_WEB_APP=OFF
    cmake --build --preset default --target lemond lemonade
    ```

## Include the Web App

If you want the embeddable build to include the browser UI assets under `resources/web-app`, enable `BUILD_WEB_APP`.

=== "Windows (cmd.exe)"

    ```cmd
    cmake --preset windows -DBUILD_WEB_APP=ON
    cmake --build --preset windows --target lemond lemonade web-app
    ```

=== "Linux (bash)"

    ```bash
    cmake --preset default -DBUILD_WEB_APP=ON
    cmake --build --preset default --target lemond lemonade web-app
    ```

## Expected Outputs

Default embeddable builds produce:

- `build/lemond` or `build/Release/lemond.exe`
- `build/lemonade` or `build/Release/lemonade.exe`
- `build/resources/` or `build/Release/resources/`
  - `server_models.json`
  - `backend_versions.json`
  - `defaults.json`

If `BUILD_WEB_APP=ON`, the build also includes `resources/web-app/`.
