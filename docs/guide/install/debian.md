## Debian Installation

Lemonade is built for Debian 13 (Trixie).

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
