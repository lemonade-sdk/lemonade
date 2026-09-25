# Ubuntu Installation

Lemonade is tested on Ubuntu 24.04 LTS but should also work on other versions.

To build from source, see the [development](../../dev/README.md) guide.

## Step 1: Install lemonade-server

=== "Stable PPA"

    ```
    sudo add-apt-repository ppa:lemonade-team/stable
    sudo apt install lemonade-server
    ```

=== "Snap"

    ```
    sudo snap install lemonade-server
    ```

## Optional: container runtime

The `rocmfpx:rocmfpx`, `llamacpp:nathanw`, `ds4:rocm` and `halogen:rocm` backends run inside OCI
container images, so they need Podman (preferred) or Docker on the host.

=== "Stable PPA"

    The package installs and enables `lemonade-podman.socket`, through which `lemond.service`
    reaches Podman, so installing Podman is the only step:

    ```
    sudo apt install podman
    ```

=== "Snap"

    ```
    sudo apt install podman
    sudo systemctl enable --now podman.socket
    sudo snap connect lemonade-server:podman :podman
    ```

See [Container Backends](../configuration/container-backends.md) for Docker and other setups.

## Step 2: Choose your frontend

=== "Web UI"

    Always available at [http://localhost:13305](http://localhost:13305).

    The web app is automatically available once lemonade-server is running. Just open your browser and navigate to the URL above.

=== "Lemonade Desktop package"

    Launches the web ui in chromium.

    ```
    sudo apt install lemonade-desktop
    ```

=== "Snap"

    Fully sandboxed desktop app.

    ```
    sudo snap install lemonade
    ```
