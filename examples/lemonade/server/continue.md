# Continue Coding Assistant

## Overview

[Continue](https://www.continue.dev/) is a coding assistant that lives inside of a VS Code extension. It supports chatting with your codebase, making edits, and a lot more.

## Expectations

We have found that the `Qwen-1.5-7B-Chat-Hybrid` model is the best Hybrid model available for coding. It is good at chatting with a few files at a time in your codebase to learn more about them. It can also make simple code editing suggestions pertaining to a few lines of code at a time.

However, we do not recommend using this model for analyzing large codebases at once or making large or complex file edits.

## Setup

### Prerequisites

1. Install Lemonade Server using the [installer .exe](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/lemonade_server_exe.md#lemonade-server-installer).

### Install Continue

> Note: they provide their own instructions [here](https://marketplace.visualstudio.com/items?itemName=Continue.continue)

1. Open the Extensions tab in VS Code Activity Bar.
1. Search "Continue - Codestral, Claude, and more" in the Extensions Marketplace search bar.
1. Select the Continue extension and click install.

This will add a Continue tab to your VS Code Activity Bar.

### Add Lemonade Server to Continue

> Note: The following instructions are based on instructions from Continue found [here](https://docs.continue.dev/customize/model-providers/openai#openai-compatible-servers--apis) 

1. Open the Continue tab in your VS Code Activity Bar.
1. Click the gear icon at the top to open Settings.
1. Under "Configuration", click "Open Config File".
1. Replace the "models" key in the `config.json` with the following and save:

```json
  "models": [
    {
      "title": "Lemonade", 
      "provider": "openai",
      "model": "Qwen-1.5-7B-Chat-Hybrid",
      "apiKey": "-",
      "apiBase": "http://localhost:8000/api/v0"
    }
  ],
```

## Usage

> Note: see the Continue [user guide](https://docs.continue.dev/) to learn about all of their features.

To try out Continue:
- Open the Continue tab in your VS Code Activity Bar, and in the "Ask anything" box, type a question about your code. Use the `@` symbol to specify a file or too.
  - Example: "What's the fastest way to install lemonade in @getting_started.md?"
- Open a file, select some code, and push Ctrl+I to start a chat about editing that code.
