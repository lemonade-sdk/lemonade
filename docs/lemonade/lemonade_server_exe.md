# Lemonade Server Installer

The `lemonade` server is available as a standalone tool with a one-click Windows installer `.exe`. Check out the [server spec](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/server_spec.md) to learn more about the functionality.

## GUI Installation and Usage

> *Note:* you may need to give your browser or OS permission to download or install the .exe.

1. Navigate to the [latest release](https://github.com/onnx/turnkeyml/releases/latest).
1. Scroll to the bottom and click `Lemonade_Server_Installer.exe` to download.
1. Double-click the `Lemonade_Server_Installer.exe` and follow the instructions.

Now that you have the server installed, you can double click the desktop shortcut to run the server process. From there, you can connect it to applications that are compatible with the OpenAI completions API.

## Silent Installation and Command Line Usage

Silent installation and command line usage are useful if you want to fully integrate `lemonade` server into your own application. This guide provides fully automated steps for downloading, installing, and running `lemonade` server so that your users don't have to install `lemonade` separately.

Definitions:
- "Silent installation" refers to an automatic command for installing `lemonade` server without running any GUI or prompting the user for any questions. It does assume that the end-user fully accepts the license terms, so be sure that your own application makes this clear to the user.
- Command line usage allows the server process to be launched programmatically, so that your application can manage starting and stopping the server process on your user's behalf.

### Download

Follow these instructions to download a copy of `Lemonade_Server_Installer.exe`.

#### cURL Download

In a `bash` terminal, such as `git bash`:

Download the latest version:

```bash
curl -L -o ".\Lemonade_Server_Installer.exe" https://github.com/onnx/turnkeyml/releases/latest/download/Lemonade_Server_Installer.exe
```

Download a specific version:

```bash
curl -L -o ".\Lemonade_Server_Installer.exe" https://github.com/onnx/turnkeyml/releases/download/v6.0.0/Lemonade_Server_Installer.exe
```

#### PowerShell Download

In a powershell terminal:

Download the latest version:

```powershell
Invoke-WebRequest -Uri "https://github.com/onnx/turnkeyml/releases/latest/download/Lemonade_Server_Installer.exe" -OutFile "Lemonade_Server_Installer.exe"
```

Download a specific version:

```powershell
Invoke-WebRequest -Uri "https://github.com/onnx/turnkeyml/releases/download/v6.0.0/Lemonade_Server_Installer.exe" -OutFile "Lemonade_Server_Installer.exe"
```

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
Lemonade_Server_Installer.exe /S /Extras=hybrid /D=C:\a\new\path`
```

### Command Line Invocation

Command line invocation starts the `lemonade` server process so that your application can connect to it via REST API endpoints. 

#### Foreground Process

These steps will open lemonade server in a terminal window that is visible to users. The user can exit the server by closing the window.

In a `cmd.exe` terminal:

```bash
conda run --no-capture-output -p INSTALL_DIR\lemonade_server\lemon_env lemonade serve
```

Where `INSTALL_DIR` is the installation path of `lemonade_server`. 

For example, if you used the default installation directory and your username is USERNAME: 

```bash
C:\Windows\System32\cmd.exe /C conda run --no-capture-output -p C:\Users\USERNAME\AppData\Local\lemonade_server\lemon_env lemonade serve
```

#### Background Process

This command will open lemonade server without opening a window. Your application needs to manage terminating the process and any child processes it creates.

In a powershell terminal:

```powershell
$serverProcess = Start-Process -FilePath "C:\Windows\System32\cmd.exe" -ArgumentList "/C conda run --no-capture-output -p INSTALL_DIR\lemonade_server\lemon_env lemonade serve" -RedirectStandardOutput lemonade_out.txt -RedirectStandardError lemonade_err.txt -PassThru -NoNewWindow
```

Where `INSTALL_DIR` is the installation path of `lemonade_server`.