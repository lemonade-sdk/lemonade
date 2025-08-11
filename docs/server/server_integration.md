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

>Note: The `lemonade-server` CLI command is added to PATH when using the Windows Installer (Lemonade_Server_Installer.exe). For Linux users or Python development environments, the command `lemonade-server-dev` is available when installing via pip.

### Checking Server Status

To identify whether or not the server is running anywhere on the system you may use the `status` command of `lemonade-server`.

```
lemonade-server status
```

This command will return either `Server is not running` or `Server is running on port <PORT>`.

### Identifying Compatible Devices

AMD Ryzen™ AI `Hybrid` models are available on Windows 11 on all AMD Ryzen™ AI 300 Series Processors. To programmatically identify supported devices, we recommend using a regular expression that checks if the CPU name converted to lowercase contains "ryzen ai" and a 3-digit number starting with 3 as shown below.

```
ryzen ai.*\b3\d{2}\b
```

Explanation:

- `ryzen ai`: Matches the literal phrase "Ryzen AI".
- `.*`: Allows any characters (including spaces) to appear after "Ryzen AI".
- `\b3\d{2}\b`: Matches a three-digit number starting with 3, ensuring it's a standalone number.

There are several ways to check the CPU name on a Windows computer. A reliable way of doing so is through cmd's `reg query` command as shown below.

```
reg query "HKEY_LOCAL_MACHINE\HARDWARE\DESCRIPTION\System\CentralProcessor\0" /v ProcessorNameString
```

Once you capture the CPU name, make sure to convert it to lowercase before using the regular expression.

### Downloading Server Installer

The recommended way of directing users to the server installer is pointing users to our releases page at [`https://github.com/lemonade-sdk/lemonade/releases`](https://github.com/lemonade-sdk/lemonade/releases). Alternatively, you may also provide the direct path to the installer itself or download the installer programmatically as shown below:


Latest version:

```bash
https://github.com/lemonade-sdk/lemonade/releases/latest/download/Lemonade_Server_Installer.exe
```

Specific version:

```bash
https://github.com/lemonade-sdk/lemonade/releases/download/v6.0.0/Lemonade_Server_Installer.exe
```

Please note that the Server Installer is only available on Windows. Apps that integrate with our server on a Linux machine must install Lemonade from source as described [here](https://lemonade-server.ai/install_options.html).

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

You can also prevent the server from showing a system tray icon by using the `--no-tray` flag:

```bash
lemonade-server serve --no-tray
```

Regardless of how Lemonade Server is installed or launched, it can read its host,
port, log level, Llama.cpp backend, and context size from environment variables.
Set `LEMONADE_HOST`, `LEMONADE_PORT`, `LEMONADE_LOG_LEVEL`, `LEMONADE_LLAMACPP`,
or `LEMONADE_CTX_SIZE` before launching `lemonade-server` by any method to
override the default settings without editing the startup script.

You can also run the server as a background process using a subprocess or any preferred method.

To stop the server, you may use the `lemonade-server stop` command, or simply terminate the process you created by keeping track of its PID. Please do not run the `lemonade-server stop` command if your application has not started the server, as the server may be used by other applications.

### Silent Installation

Silent installation runs `Lemonade_Server_Installer.exe` without a GUI and automatically accepts all prompts.

In a `cmd.exe` terminal:

Install *with* Ryzen AI hybrid support: 

```bash
Lemonade_Server_Installer.exe /S /Extras=hybrid
```

Install *without* Ryzen AI hybrid support:

```bash
Lemonade_Server_Installer.exe /S
```

The install directory can also be changed from the default by using `/D` as the last argument. 

For example: 

```bash
Lemonade_Server_Installer.exe /S /Extras=hybrid /D=C:\a\new\path
```

Only `Qwen2.5-0.5B-Instruct-CPU` is installed by default in silent mode. If you wish to select additional models to download in silent mode, you may use the `/Models` argument.

```bash
Lemonade_Server_Installer.exe /S /Extras=hybrid /Models="Qwen2.5-0.5B-Instruct-CPU Llama-3.2-1B-Instruct-Hybrid"
```

The available modes are documented [here](./server_models.md).

Finally, if you don't want to create a desktop shortcut during installation, use the `/NoDesktopShortcut` parameter:

```bash
Lemonade_Server_Installer.exe /S /NoDesktopShortcut
```

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->