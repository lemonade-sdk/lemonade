
# Running agents locally with Lemonade and AnythingLLM

## Overview

[AnythingLLM](https://github.com/Mintplex-Labs/anything-llm) is a versatile local LLM platform that allows you to chat with your documents and code using a variety of models. It supports the OpenAI-compatible API interface, allowing easy integration with local servers like Lemonade.

This guide will help you configure AnythingLLM to use Lemonade's OpenAI-compatible server, and utilize the powerful `@agent` capability to interact with documents, webpages, and more.

## Expectations

Lemonade integrates best with AnythingLLM when using models such as `Qwen-1.5-7B-Chat-Hybrid` and `Llama-3.2-1B-Instruct-Hybrid`, both of which support a context length of up to 3,000 tokens.

Keep in mind that when using the `@agent` feature, multi-turn conversations can quickly consume available context. As a result, the number of back-and-forth turns in a single conversation may be limited due to the growing context size.


## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.
2. Install and set up AnythingLLM from their [GitHub](https://github.com/Mintplex-Labs/anything-llm#quick-start) or [website](https://anythingllm.com/desktop).


### Configure AnythingLLM to Use Lemonade

<ol>
  <li>In the bottom of the left menu, click on the wrench icon to "Open Settings".</li>
  <li>Under the menu "AI Providers", click "LLM".</li>
  <li>
    Select "Generic OpenAI" and enter the following info:
    <table>
      <tr><th>Setting</th><th>Value</th></tr>
      <tr><td><b>Base URL</b></td><td><code>http://localhost:8000/api/v1</code></td></tr>
      <tr><td><b>API Key</b></td><td><code>-</code></td></tr>
      <tr><td><b>Chat Model Name</b></td><td><code>Qwen-1.5-7B-Chat-Hybrid</code></td></tr>
      <tr><td><b>Token context window</b></td><td><code>3000</code></td></tr>
      <tr><td><b>Max Tokens</b></td><td><code>3000</code></td></tr>
    </table>
  </li>
  <li>In the bottom left, click the back button to exit.</li>
  <li>In the left menu, click "New Workspace" and give it a name.</li>
  <li>Where you see your new workspace, click the gear icon to open the "Workspace Settings"</li>
  <li>In the top menu of the window that opens, click on "Agent Configuration"</li>
  <li>Under Chat Settings, select Generic OpenAI and click save.</li>
  <li>Under Workspace Agent LLM Provider, select "Generic OpenAI" and click save.</li>
</ol>

## Usage with @agent

### Overview

Agents are capable of scraping websites, listing and summarizing documents, searching the web, creating charts, and even saving files to your desktop or their own memory.

To start an agent session, simply go to any workspace and type `@agent <your prompt>`. To exit the session, just type `exit`.

### Agent Skills

You may turn on and off specific `Agent Skills` by going to your `Workspace Settings` → `Agent Configuration` → `Configure Agent Skills`.

Available agent skills include:

* RAG & long-term memory
* View and summarize documents
* Scrape Websites
* Generate & save files to browser
* Generate Charts
* Web Search
* SQL Connector

### Examples

Here are some examples on how you can interact with Anything LLM agents:

- **Rag & long-term memory**
    - `@agent My name is Dr Lemon. Remember this in our next conversation`
    - Then, on a follow up chat you can ask `@agent What is my name according to your memory?`
- **Scrape Websites**
    - `@agent Scrape this website and tell me what are the two ways of installing lemonade https://github.com/lemonade-sdk/lemonade/blob/main/docs/server/README.md`
- **Web Search** (enable skill before trying)
    - `@agent Search the web for the best place to buy shoes`

You can find more details about agent usage [here](https://docs.anythingllm.com/agent/usage).

## Additional Resources

- [AnthingLLM Website](https://anythingllm.com/)
- [AnythingLLM GitHub](https://github.com/Mintplex-Labs/anything-llm)
- [AnythingLLM Documentation](https://docs.anythingllm.com/)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->