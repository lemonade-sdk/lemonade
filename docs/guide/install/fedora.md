## Fedora Installation

Lemonade is built for Fedora 43 and 44.

=== "Fedora 43"

    Get the RPM from the [latest release](https://github.com/lemonade-sdk/lemonade/releases):
    `lemonade-server-<version>-fc43.x86_64.rpm`

    ```bash
    sudo dnf install ./lemonade-server-*-fc43.x86_64.rpm
    ```

=== "Fedora 44"

    Get the RPM from the [latest release](https://github.com/lemonade-sdk/lemonade/releases):
    `lemonade-server-<version>-fc44.x86_64.rpm`

    ```bash
    sudo dnf install ./lemonade-server-*-fc44.x86_64.rpm
    ```

Enable and start the service:

```bash
sudo systemctl enable --now lemond
```

Check that it's running:

```bash
sudo systemctl --no-pager status lemond
```

Once the service is running, open [http://localhost:13305](http://localhost:13305) in your browser.

## Optional: container runtime

The `rocmfpx` and `ds4` backends run inside OCI container images, so they need
podman (preferred) or docker on the host. Nothing else does. See
[Container Backends](../configuration/container-backends.md).

```bash
sudo dnf install podman
```
