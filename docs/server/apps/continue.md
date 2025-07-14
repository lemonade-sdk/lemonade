# Continue Coding Assistant

## Overview

[Continue](https://www.continue.dev/) is a coding assistant that lives inside of a VS Code extension. It supports chatting with your codebase, making edits, and a lot more.

## Demo Video

▶️ [Watch on YouTube](https://www.youtube.com/watch?v=bP_MZnDpbUc&source_ve_path=MjM4NTE)

<iframe width="560" height="315" src="https://www.youtube.com/embed/bP_MZnDpbUc?si=0KZLzQzFlRvW9J9f" 
title="YouTube video player" frameborder="0" allowfullscreen></iframe>

## Expectations

We have found that the `Qwen-1.5-7B-Chat-Hybrid` model is the best Hybrid model available for coding. It is good at chatting with a few files at a time in your codebase to learn more about them. It can also make simple code editing suggestions pertaining to a few lines of code at a time.

However, we do not recommend using this model for analyzing large codebases at once or making large or complex file edits.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.

### Install Continue

> Note: they provide their own instructions [here](https://marketplace.visualstudio.com/items?itemName=Continue.continue)

1. Open the Extensions tab in VS Code Activity Bar.
1. Search "Continue - Codestral, Claude, and more" in the Extensions Marketplace search bar.
1. Select the Continue extension and click install.

This will add a Continue tab to your VS Code Activity Bar.

### Add Lemonade Server to Continue

> Note: The following instructions are based on instructions from Continue found [here](https://docs.continue.dev/customize/model-providers/openai#openai-compatible-servers--apis) 

1. Open the Continue tab in your VS Code Activity Bar.
1. Click the chat box. Some buttons will appear at the bottom of the box, including `Select model`.
1. Click `Select model`, then `+ Add Chat model` to open the new model dialog box.
1. Click the `config file` link at the very bottom of the dialog to open `config.yaml`.
1. Replace the "models" key in the `config.yaml` with the following and save:

```yaml
models:
  - name: Lemonade
    provider: openai
    model: Qwen-1.5-7B-Chat-Hybrid 
    apiBase: http://localhost:8000/api/v1
    apiKey: none
```

6. Close the dialog box.
7. Click the chat box again. You should see `Lemonade` where you used to see `Select model`. Ready!

## Usage

> Note: see the Continue [user guide](https://docs.continue.dev/) to learn about all of their features.

Here are some examples for trying out Continue. These examples assume you have cloned this repo and allowed Continue to index it.

### Chat with Files

Open the Continue tab in your VS Code Activity Bar, and in the "Ask anything" box, type a question about your code. Use the `@` symbol to specify a file or tool.

  - "What's the fastest way to install Lemonade in `@getting_started.md?`"
  - "According to `@README.md` what do I need to do to set up for `@api_oga_hybrid_streaming.py`?"

### Editing Files

Open a file, select some code, and push Ctrl+I to start a chat about editing that code.

  1. Open `//examples//lemonade//api_basic.py`.
  1. Select the `print(...` line at the bottom and press `ctrl+i`.
  1. Write "Add a helpful comment" in the chat box and press enter.
  1. Press "accept" if you would like to accept the change.

### Making Files

Start a new chat and prompt: 

> write a script in the style of `@api_basic.py` that uses the microsoft/Phi-4-mini-instruct model on GPU

Here's what we got:

```python
# Import necessary modules
from lemonade.api import from_pretrained

# Load the Phi-4-mini-instruct model with the hf-cpu recipe
model, tokenizer = from_pretrained("microsoft/Phi-4-mini-instruct", recipe="hf-cpu")

# Define your prompt
prompt = "This is a sample prompt for the Phi-4-mini-instruct model"

# Tokenize the prompt
input_ids = tokenizer(prompt, return_tensors="pt")

# Generate the response using the model
response = model.generate(input_ids, max_new_tokens=100)  # Adjust the max_new_tokens as needed

# Decode the generated response
generated_text = tokenizer.decode(response[0])

# Print the response
print(generated_text)
```

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->