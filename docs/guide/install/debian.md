## Debian Installation

Lemonade is built for Debian 13 (Trixie).

=== "Prerequisites"

    Lemonade depends on **`libcpp-httplib0.41`** which is not available in Debian 13's
    default repositories (it ships `libcpp-httplib0.18` only). You must enable the
    [Debian Backports](https://backports.debian.org/) repository first:

    ```bash
    # Add the backports repository
    echo "deb http://deb.debian.org/debian trixie-backports main" | sudo tee /etc/apt/sources.list.d/backports.list
    sudo apt update
    ```

=== "Installation"

    Get the `.deb` from the [latest release](https://github.com/lemonade-sdk/lemonade/releases):
    `lemonade-server_<version>-debian13_amd64.deb`

    ```bash
    sudo apt install ./lemonade-server_*-debian13_amd64.deb
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
