# Arch Installation

## Step 1: Install lemonade-server

```
pacman -S lemonade-server
```

For package details, see [lemonade-server](https://archlinux.org/packages/extra/x86_64/lemonade-server).

## Optional: container runtime

The `rocmfpx:rocmfpx`, `llamacpp:nathanw`, `ds4:rocm` and `halogen:rocm` backends run inside OCI
container images, so they need Podman (preferred) or Docker on the host. The package installs
`lemonade-podman.socket`, through which `lemond.service` reaches Podman. Install Podman and enable
the socket:

```
sudo pacman -S podman
sudo systemctl enable --now lemonade-podman.socket
```

See [Container Backends](../configuration/container-backends.md) for Docker and other setups.

## Step 2: Choose your frontend

=== "Web UI"

    Always available at [http://localhost:13305](http://localhost:13305).

    The web app is automatically available once lemonade-server is running. Just open your browser and navigate to the URL above.

=== "Lemonade Desktop package"

    ```
    pacman -S lemonade-desktop
    ```

    For package details, see [lemonade-desktop](https://archlinux.org/packages/extra/x86_64/lemonade-desktop).
