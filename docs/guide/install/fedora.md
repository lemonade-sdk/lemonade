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


### Linux FLM/NPU beta notes

Lemonade's Linux FLM/NPU path is experimental and may require runtime configuration beyond installing the RPM. If you install FastFlowLM or XRT separately, make sure the lemond systemd service can find the required libraries and model configuration; an interactive shell's environment is not automatically inherited by systemd.

For a beta setup, validate the runtime from the same service context used by Lemonade and check the service logs if the backend is reported as unavailable. Depending on the installation, the service may need environment entries such as:

```ini
[Service]
Environment=LEMONADE_FLM_LINUX_BETA=1
Environment=LD_LIBRARY_PATH=/path/to/fastflowlm/lib:/path/to/xrt/lib
Environment=FLM_CONFIG_PATH=/path/to/model_list.json
LimitMEMLOCK=infinity
```

Replace the paths with the locations from your installation, then reload and restart the service:

```bash
sudo systemctl daemon-reload
sudo systemctl restart lemond
sudo systemctl --no-pager status lemond
```

This path is still beta. If you are not intentionally testing FLM/NPU support, use the standard Fedora installation above and report backend-specific failures with the Lemonade version, Fedora version, and relevant service log excerpts.
