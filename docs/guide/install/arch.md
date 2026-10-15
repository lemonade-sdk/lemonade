# Arch Installation

## Step 1: Install lemonade-server

```
pacman -S lemonade-server
```

For package details, see [lemonade-server](https://archlinux.org/packages/extra/x86_64/lemonade-server).

## Step 2: Start the service

The package installs a systemd service, but it is not started automatically. Enable and start it:

```bash
sudo systemctl enable --now lemond
```

Check that it's running:

```bash
sudo systemctl --no-pager status lemond
```

Without this step, commands such as `lemonade pull` fail with "Could not connect to Lemonade server".

## Step 3: Choose your frontend

=== "Web UI"

    Always available at [http://localhost:13305](http://localhost:13305).

    The web app is automatically available once lemonade-server is running. Just open your browser and navigate to the URL above.

=== "Lemonade Desktop package"

    ```
    pacman -S lemonade-desktop
    ```

    For package details, see [lemonade-desktop](https://archlinux.org/packages/extra/x86_64/lemonade-desktop).
