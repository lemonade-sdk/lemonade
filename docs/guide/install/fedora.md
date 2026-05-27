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
