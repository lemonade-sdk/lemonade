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


## Linux FLM/NPU beta notes

The Linux FLM/NPU backend is currently beta. Before starting `lemond`, make sure the runtime libraries and model configuration are visible to the service.

For an interactive test, export the variables in the shell that launches the server:

```bash
export LEMONADE_FLM_LINUX_BETA=1
export LD_LIBRARY_PATH=/path/to/fastflowlm/lib:/path/to/xrt/lib
export FLM_CONFIG_PATH=/path/to/model_list.json
lemond
```

For a systemd installation, put the same settings in an override instead of relying on a shell profile:

```ini
# sudo systemctl edit lemond
[Service]
Environment=LEMONADE_FLM_LINUX_BETA=1
Environment=LD_LIBRARY_PATH=/path/to/fastflowlm/lib:/path/to/xrt/lib
Environment=FLM_CONFIG_PATH=/path/to/model_list.json
LimitMEMLOCK=infinity
```

Apply the override and inspect the service log:

```bash
sudo systemctl daemon-reload
sudo systemctl restart lemond
sudo journalctl -u lemond -n 100 --no-pager
```

The beta path and its environment names may change as support matures. If startup fails, verify the library paths, model-list path, device/runtime versions, and the `journalctl` output when reporting the issue.
