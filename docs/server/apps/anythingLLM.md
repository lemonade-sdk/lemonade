
# Running agents locally with Lemonade and AnythingLLM

## Overview

[AnythingLLM](https://github.com/Mintplex-Labs/anything-llm) is a Docker and Desktop application that allows you to leverage agents, RAG, and more with your local LLMs. It comes pre-packaged with everything you need to get started with local LLMs for productivity. AnythingLLM supports the OpenAI-compatible API interface, allowing easy integration with local servers like Lemonade!

This guide will help you configure AnythingLLM to use Lemonade's OpenAI-compatible server, and utilize the powerful `@agent` capability to interact with documents, webpages, custom tools, and more.

## AnythingLLM - Docker vs Desktop

Docker and Desktop are two different ways to run AnythingLLM. Both are powerful in their own ways and both are absolutely local-first for any features.

### You want to run AnythingLLM in Docker

- You want to run AnythingLLM in a containerized environment
- You want to support multiple users on the same machine
- You want admin access over all users to curate documents and agent skills that are available during inference

### You want to run AnythingLLM in Desktop

- You want a "one-click" or "zero configuration" experience to get started using AnythingLLM for your own use cases
- Want your model to be able to access knowledge on your own machine (file search, web-browsing, etc.)
- Want to have an assistant that "lives" across your entire operating system all powered by your local LLMs via Lemonade on optimized hardware.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.
2. Install and set up AnythingLLM from their [Documentation](https://docs.anythingllm.com/installation-docker/quickstart) or [Desktop Installer](https://anythingllm.com/desktop).


### Configure AnythingLLM to Use Lemonade

During onboarding or from the "LLM" submenu on the settings sidebar you can set `Lemonade` as your LLM provider. This is the preferred way to get started as it will automatically configure everything for you and provide a point-and-click interface to download and configure your Lemonade LLMs.

<div align="center">
  <br><em>Setting up AnythingLLM to use Lemonade on the in-app settings page.</em></br>
  <img src="https://github.com/lemonade-sdk/assets/blob/main/anythingllm/llm-settings.png?raw=true" alt="Setting up AnythingLLM to use Lemonade on the in-app settings page" width="600"/>
</div>

> AnythingLLM will automatically detect your Lemonade Server if running on the same machine. Otherwise, you can manually input the server URL from Lemonade Server's settings page


From this page, you can also search and select from all available Lemonade models based on your hardware and preferences. You can also configure the context size as well as managed the models that are available to you through Lemonade without ever needing to leave the AnythingLLM app.


> AnythingLLM also supports running Lemonade models for your Embedder to run on your optimized hardware! 

## Sending chats to Lemonade

In AnythingLLM, we will automatically detect if your model is capable of native tool calling. This gives you the ability to leverage the "@agent" mode to its maximum potential. If your model is not capable of native tool calling, you can still use the "@agent" mode, but you may have worse performance.

### Overview

Agents are capable of scraping websites, listing and summarizing documents, searching the web, creating charts, and even saving files to your desktop or their own memory.

To start an agent session, simply go to any workspace and type `@agent <your prompt>`. To exit the session, just type `exit`.

### Agent Skills

You may turn on and off specific `Agent Skills` by going to your `Workspace Settings` → `Agent Configuration` → `Configure Agent Skills` or toggling them from the prompt input box.

<div align="center">
  <br><em>Toggling agent skills from the prompt input box.</em></br>
  <img src="https://github.com/lemonade-sdk/assets/blob/main/anythingllm/agent-skills.png?raw=true" alt="Toggling agent skills from the prompt input box" width="600"/>
</div>

Available agent skills include:

* RAG & long-term memory
* View and summarize documents
* Scrape Websites
* Generate & save files to browser
* Generate Charts
* Web Search
* SQL Connector

You can always use the built in no-code [Agent Flow builder](https://docs.anythingllm.com/agent-flows/overview), [create your own custom skills](https://docs.anythingllm.com/agent/custom/introduction), or [use MCPs](https://docs.anythingllm.com/mcp-compatibility/overview) to have powerful and flexible agentic capabilities with your local models.

### Examples

Here are some examples on how you can interact with Anything LLM agents:

- **Rag & long-term memory**
    - `@agent My name is Dr Lemon. Remember this in our next conversation`
    - Then, on a follow up chat you can ask `@agent What is my name according to your memory?`
- **Scrape Websites**
    - `@agent Scrape this website and tell me what are the two ways of installing lemonade https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/README.md`
- **Web Search** (enable skill before trying)
    - `@agent Search the web for the best place to buy shoes`

AnythingLLM also supports complex multi-step agentic tool calling. You can do this with simple natural language.

> "@agent Look online for information about the AMD Lemonade SDK and tell me how many GitHub stars it has"

<div align="center">
  <br><em>Complex multi-step agentic tool calling with simple natural language.</em></br>
  <img src="https://github.com/lemonade-sdk/assets/blob/main/anythingllm/tool-calling.png?raw=true" alt="Complex multi-step agentic tool calling with simple natural language" width="600"/>
</div>

You can find more details about agent usage [here](https://docs.anythingllm.com/agent/usage).

## Additional Resources

- [AnthingLLM Website](https://anythingllm.com/)
- [AnythingLLM GitHub](https://github.com/Mintplex-Labs/anything-llm)
- [AnythingLLM Documentation](https://docs.anythingllm.com/)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
