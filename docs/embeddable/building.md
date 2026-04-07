# Embeddable Lemonade: Building from Source

This guide shows how to build the embeddable `lemond` and `lemonade` binaries from source.

For general prerequisites, toolchain setup, and broader development workflows, see [Lemonade Development](../dev-getting-started.md).

Contents:

- [Default Embeddable Build](#default-embeddable-build)
- [Include the Web App](#include-the-web-app)
- [Expected Outputs](#expected-outputs)

## Default Embeddable Build

The `embeddable` CMake target builds the server, CLI, and required resource files, then packages them into a single archive. The [release workflow](../../.github/workflows/cpp_server_build_test_release.yml) uses this target to produce the embeddable archives.

=== "Windows (cmd.exe)"

    ```cmd
    cmake --preset windows -DBUILD_WEB_APP=OFF
    cmake --build --preset windows --target embeddable
    ```

    This produces `build\lemonade-embeddable-{VERSION}-windows-x64.zip`.

=== "Linux (bash)"

    ```bash
    sudo apt-get update
    sudo apt-get install -y cmake ninja-build g++ pkg-config libssl-dev libdrm-dev
    cmake --preset default -DBUILD_WEB_APP=OFF
    cmake --build --preset default --target embeddable
    ```

    This produces `build/lemonade-embeddable-{VERSION}-ubuntu-x64.tar.gz`.

## Include the Web App

If you want the embeddable build to include the browser UI assets under `resources/web-app`, enable `BUILD_WEB_APP` and build the `web-app` target before `embeddable`:

=== "Windows (cmd.exe)"

    ```cmd
    cmake --preset windows -DBUILD_WEB_APP=ON
    cmake --build --preset windows --target web-app embeddable
    ```

=== "Linux (bash)"

    ```bash
    cmake --preset default -DBUILD_WEB_APP=ON
    cmake --build --preset default --target web-app embeddable
    ```

## Expected Outputs

The `embeddable` target produces a single archive in `build/`:

| Platform | Archive |
|----------|---------|
| Linux    | `lemonade-embeddable-{VERSION}-ubuntu-x64.tar.gz` |
| Windows  | `lemonade-embeddable-{VERSION}-windows-x64.zip` |

Each archive contains:

- `lemond` (or `lemond.exe`) — the server binary
- `lemonade` (or `lemonade.exe`) — the CLI binary
- `LICENSE`
- `resources/server_models.json`
- `resources/backend_versions.json`
- `resources/defaults.json`
