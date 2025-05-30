# Microsoft AI Toolkit for VS Code

## Overview

The [AI Toolkit for Visual Studio Code](https://learn.microsoft.com/en-us/windows/ai/toolkit/) is a VS Code extension that simplifies generative AI app development by bringing together cutting-edge AI development tools and models from various catalogs. It supports running AI models locally or connecting to remote models via API keys.

## Demo Video

▶️ [Watch on YouTube](https://www.youtube.com/watch?v=JecpotOZ6qo)

<iframe width="560" height="315" src="https://www.youtube.com/embed/JecpotOZ6qo?si=9YcWwVEx7UX5A812" 
title="YouTube video player" frameborder="0" allowfullscreen></iframe>

## Expectations

We have found that most LLMs work well with this application. 

However, the `Inference Parameters` option is not fully supported, as Lemonade Server currently does not accept those as inputs (see [server_spec.md](../server_spec.md) for details).


## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.

### Install AI Toolkit for VS Code

1. Open the Extensions tab in VS Code Activity Bar.
2. Search for "AI Toolkit for Visual Studio Code" in the Extensions Marketplace search bar.
3. Select the AI Toolkit extension and click install.

This will add an AI Toolkit icon to your VS Code Activity Bar.

### Connect Lemonade to AI Toolkit

The AI Toolkit now supports "Bring Your Own Model" functionality, allowing you to connect to models served via the OpenAI API standard, which Lemonade uses.

1. Open the AI Toolkit tab in your VS Code Activity Bar.
2. In the right corner of the "My Models" section, click the "+" button to "Add model for remote inference".
3. Select "Add a custom model".
4. When prompted to "Enter OpenAI chat completion endpoint URL" enter:
    ```
    http://localhost:8000/api/v1/chat/completions
    ```
5. When prompted to "Enter the exact model name as in the API" select a model (e.g., `Phi-3-Mini-Instruct-Hybrid`)
    - Note: You can get a list of all models available [here](../server_models.md).
6. Select the same name as the display model name.
7. Skip the HTTP authentication step by pressing "Enter".

## Usage

Once you've set up the Lemonade model in AI Toolkit, you can:

1. Use the **AI Playground** tool to directly interact with your added model.
2. Use the **Prompt Builder** tool to craft effective prompts for your AI models.
3. Use the **Bulk Run** tool to compute responses for custom datasets and easily visualize those responses on a table format.
4. Use the **Evaluation** tool to quickly assess your model's coherence, fluency, relevance, and similarity, as well as to compute BLEU, F1, GLEU, and Meteor scores.

## Additional Resources

- [AI Toolkit for VS Code Documentation](https://learn.microsoft.com/en-us/windows/ai/toolkit/)
- [AI Toolkit GitHub Repository](https://github.com/microsoft/vscode-ai-toolkit)
- [Bring Your Own Models on AI Toolkit](https://techcommunity.microsoft.com/blog/azuredevcommunityblog/bring-your-own-models-on-ai-toolkit---using-ollama-and-api-keys/4369411)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->