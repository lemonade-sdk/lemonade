# `wut` Terminal Assistant

## Overview

The [`wut` terminal assistant](https://github.com/shobrook/wut) uses LLMs to parse your terminal's scrollback, helping you troubleshoot your last command.

## Expectations

We found that `wut` works nicely with the `Llama-3.2-3B-Instruct-Hybrid` model.

It is not especially convenient to use `wut` with Windows until the developers remove the requirement for `tmux`, however we do provide instructions for getting set up on Windows in this guide.

`wut` seems to send the entire terminal scrollback to the LLM, which can produce very long prompts that exceed the LLM's context length. We recommend restricting the terminal scrollback or using a fresh `tmux` session when trying this out.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.


`wut` currently requires a `tmux` terminal in order to function. We found the simplest way to achieve this on Windows was through the Windows Subsystem for Linux (WSL).

1. Install [Windows Subsystem for Linux](https://learn.microsoft.com/en-us/windows/wsl/install).
1. Open the `WSL Settings` app, navigate to `Networking`, and make sure the `Networking mode` is `Mirrored`.
  - This is required for WSL terminals to be able to see the Lemonade server running in Windows.

3. If needed: shut down WSL to make sure the changes apply:

```powershell
wsl --shutdown
```

4. Start a WSL terminal.
5. Install [`pipx`](https://github.com/pypa/pipx), as recommended by the following `wut` instructions:

```bash
sudo apt update
sudo apt install pipx
pipx ensurepath
```

6. Re-launch your terminal to make sure `pipx` is available, then install `wut`:

```bash
pipx install wut-cli
```

7. Add `wut`'s required environment variables to your `.bashrc` file:

```bash
export OPENAI_API_KEY="-"
export OPENAI_MODEL="Llama-3.2-3B-Instruct-Hybrid"
export OPENAI_BASE_URL="http://localhost:8000/api/v0"
```

## Usage

### Start a terminal

1. Start a WSL terminal.
2. Start a `tmux` session:

```bash
tmux
```

Then, try some of these example commands that `wut` can help explain.

### Help with Lemonade Server

People often ask exactly what Lemonade Server's `models` endpoint does. Fortunately, `wut` is able to intuit the answer!

```bash
curl http://localhost:8000/api/v0/models
wut
```

The terminal response of the `curl` command is this (only intelligible by machines):

```
curl http://localhost:8000/api/v0/models
{"object":"list","data":[{"id":"Qwen2.5-0.5B-Instruct-CPU","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"Llama-3.2-1B-Instruct-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"Llama-3.2-3B-Instruct-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"Phi-3-Mini-Instruct-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"Qwen-1.5-7B-Chat-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"DeepSeek-R1-Distill-Llama-8B-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"},{"id":"DeepSeek-R1-Distill-Qwen-7B-Hybrid","created":1744226681,"object":"model","owned_by":"lemonade"}]}
```

But `wut` does a nice job interpreting:

```
The output suggests that the API endpoint is returning a list of models, and the owned_by field indicates that all models are owned by "lemonade". Thecreated timestamp indicates when each model was created.

The output is a valid JSON response, and there is no error or warning message. The command was successful, and the output can be used for further processing or analysis. 
```


### Bad Git Command

Run a command that doesn't exist, and then ask `wut` for help:

```bash
git pull-request
wut
```

Results in:

> git: 'pull-request' is not a git command. See 'git --help'.

And then `wut` provides some helpful feedback:

> Key takeaway: The command git pull-request is not a valid Git command. The correct command to create a pull request is git request-pull, but it's not a standard Git command. The output wut is the name of the activated Conda environment. To create a pull request, use git request-pull or git pull with the --pr option. 

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->