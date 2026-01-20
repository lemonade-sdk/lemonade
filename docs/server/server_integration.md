# Integrating with Lemonade Server

This guide provides instructions on how to integrate Lemonade Server into your application.

There are two main ways in which Lemonade Sever might integrate into apps:

* User-Managed Server: User is responsible for installing and managing Lemonade Server.
* App-Managed Server: App is responsible for installing and managing Lemonade Server on behalf of the user.

The first part of this guide contains instructions that are common for both integration approaches. The second part provides advanced instructions only needed for app-managed server integrations.

## General Instructions

### Identifying Existing Installation

To identify if Lemonade Server is installed on a system, you can use the [`lemonade-server` CLI command](./lemonade-server-cli.md), which is added to path when using our installer. This is a reliable method to:

- Verify if the server is installed.
- Check which version is currently available is running the command below.

```
lemonade-server --version
```

>Note: The `lemonade-server` CLI command is added to PATH when using the Windows Installer (lemonade-server-minimal.msi) and Debian Installer (lemonade-server-minimal_<VERSION>_amd64.deb).

### Checking Server Status

To identify whether or not the server is running anywhere on the system you may use the `status` command of `lemonade-server`.

```
lemonade-server status
```

This command will return either `Server is not running` or `Server is running on port <PORT>`.

### Identifying Compatible Devices

AMD Ryzen™ AI `Hybrid` and `NPU` models are available on Windows 11 on all AMD Ryzen™ AI 300 Series, 400 Series, and Z2 Series Processors. To programmatically identify supported devices, we recommend using a regular expression that checks if the CPU name converted to lowercase contains "ryzen ai" and either a 3-digit number starting with 3 or 4, or "z2" as shown below.

```
ryzen ai.*((\b[34]\d{2}\b)|(\bz2\b))
```

Explanation:

- `ryzen ai`: Matches the literal phrase "Ryzen AI".
- `.*`: Allows any characters (including spaces) to appear after "Ryzen AI".
- `((\b[34]\d{2}\b)|(\bz2\b))`: Matches either a three-digit number starting with 3 or 4 (for 300/400 series), or "z2" (for Z2 series like Z2 Extreme), ensuring it's a standalone word.

There are several ways to check the CPU name on a Windows computer. A reliable way of doing so is through cmd's `reg query` command as shown below.

```
reg query "HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ProcessorNameString
```

Once you capture the CPU name, make sure to convert it to lowercase before using the regular expression.

### Downloading Server Installer

The recommended way of directing users to the installer is pointing users to https://lemonade-server.ai/install_options.html

### Installing Additional Models

Lemonade Server installations always come with at least one LLM installed. If you want to install additional models on behalf of your users, the following tools are available:

- Discovering which LLMs are available:
  - [A human-readable list of supported models](./server_models.md).
  - [A JSON file with the list of supported models](https://github.com/lemonade-sdk/lemonade/tree/main/src/lemonade_server/server_models.json) is included in every Lemonade Server installation.
- Installing LLMs:
  - [The `pull` endpoint in the server](./server_spec.md#get-apiv1pull).
  - `lemonade-server pull MODEL` on the command line interface.

## Stand-Alone Server Integration

Some apps might prefer to be responsible for installing and managing Lemonade Server on behalf of the user. This part of the guide includes steps for installing and running Lemonade Server so that your users don't have to install Lemonade Server separately.

Definitions:

- Command line usage allows the server process to be launched programmatically, so that your application can manage starting and stopping the server process on your user's behalf.
- "Silent installation" refers to an automatic command for installing Lemonade Server without running any GUI or prompting the user for any questions. It does assume that the end-user fully accepts the license terms, so be sure that your own application makes this clear to the user.

### Command Line Invocation

This command line invocation starts the Lemonade Server process so that your application can connect to it via REST API endpoints. To start the server, simply run the command below.

```bash
lemonade-server serve
```

By default, the server runs on port 8000. Optionally, you can specify a custom port using the --port argument:

```bash
lemonade-server serve --port 8123
```

You can also prevent the server from showing a system tray icon by using the `--no-tray` flag (Windows only):

```bash
lemonade-server serve --no-tray
```

You can also run the server as a background process using a subprocess or any preferred method.

To stop the server, you may use the `lemonade-server stop` command, or simply terminate the process you created by keeping track of its PID. Please do not run the `lemonade-server stop` command if your application has not started the server, as the server may be used by other applications.

## Windows Installation

**Available Installers:**
- `lemonade-server-minimal.msi` - Server only (~3 MB)
- `lemonade.msi` - Full installer with Electron desktop app (~105 MB)

**GUI Installation:**

Double-click the MSI file, or run:

```bash
msiexec /i lemonade.msi
```

**MSI Properties:**

Properties can be passed on the command line to customize the installation:

- `INSTALLDIR` - Custom installation directory (default: `%LOCALAPPDATA%\lemonade_server`)
- `ADDDESKTOPSHORTCUT` - Create desktop shortcut (0=no, 1=yes, default: 1)
- `ALLUSERS` - Install for all users (1=yes, requires elevation; default: per-user)

**Examples:**

```bash
# Custom install directory
msiexec /i lemonade.msi INSTALLDIR="C:\Custom\Path"

# Without desktop shortcut
msiexec /i lemonade.msi ADDDESKTOPSHORTCUT=0

# Combined parameters
msiexec /i lemonade.msi INSTALLDIR="C:\Custom\Path" ADDDESKTOPSHORTCUT=0
```

### Silent Installation

Add `/qn` to run without a GUI, automatically accepting all prompts:

```bash
msiexec /i lemonade.msi /qn
```

This can be combined with any MSI properties:

```bash
msiexec /i lemonade.msi /qn INSTALLDIR="C:\Custom\Path" ADDDESKTOPSHORTCUT=0
```

### All Users Installation

To install for all users (Program Files + system PATH), you **must** run from an Administrator command prompt.

1. Open Command Prompt as Administrator (right-click → "Run as administrator")
2. Run the install command:

```bash
msiexec /i lemonade.msi ALLUSERS=1 INSTALLDIR="C:\Program Files (x86)\Lemonade Server"
```

For silent all-users installation, add `/qn`:

```bash
msiexec /i lemonade.msi /qn ALLUSERS=1 INSTALLDIR="C:\Program Files (x86)\Lemonade Server"
```

**Troubleshooting:**
- If installation fails silently, check that you're running as Administrator
- Add `/L*V install.log` to generate a debug log file


<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->