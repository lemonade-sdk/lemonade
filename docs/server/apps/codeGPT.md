# CodeGPT with VS Code

## Overview

[CodeGPT Chat](https://codegpt.co/) is an AI-powered chatbot designed to assist developers with coding tasks directly within their preferred integrated development environments (IDEs), for example, VS Code.

## Expectations

We have found that the `Qwen-1.5-7B-Chat-Hybrid` model is the best Hybrid model available for coding. It is good at chatting with a few files at a time in your codebase to learn more about them. It can also make simple code editing suggestions pertaining to a few lines of code at a time.

However, we do not recommend using this model for analyzing large codebases at once or making large or complex file edits.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md) and using the installer .exe.

### Install CodeGPT in VS Code

> The following instructions are based off CodeGPT provided instructions found [here](https://docs.codegpt.co/docs/tutorial-basics/installation).

1. Open the Extensions tab in VS Code Activity Bar.
1. Search "CodeGPT: Chat & AI Agents" in the Extensions Marketplace search bar.
1. Select the CodeGPT extension and click install.

This will add a CodeGPT tab to your VS Code Activity Bar.

### Add Lemonade Server to CodeGPT

> Note: The following instructions are based on instructions from CodeGPT found [here](https://docs.codegpt.co/docs/tutorial-ai-providers/custom).


<ol>
  <li>Open the CodeGPT tab in your VS Code Activity Bar.</li>
  <li>Sign Up or Sign into your account.</li>
  <li>In the model dropdown menu and click "View More".</li>
  <li>Select the tab: "LLMs Cloud model"</li>
  <li>Under "All Models", set the following:
  <table>
   <tr><th>Field</th><th>Value</th></tr>
      <tr><td><b>Select Provider:</b></td><td><code>Custom</code></td></tr>
      <tr><td><b>Select Model: </b></td><td><code>Qwen-1.5-7B-Chat-Hybrid</code></td></tr>
   </table>
  </li>
  <li>Click "Change connection settings" and enter the following information:
    <table>
      <tr><th>Field</th><th>Value</th></tr>
      <tr><td><b>API Key</b></td><td><code>-</code></td></tr>
      <tr><td><b>Custom Link</b></td><td><code>http://localhost:8000/api/v1/api/v1</code></td></tr>
    </table>
  </li>
</ol>

## Usage

> Note: see the CodeGPT [user guide](https://docs.codegpt.co/docs/intro) to learn about all of their features.

To try out CodeGPT:

- Open the CodeGPT tab in your VS Code Activity Bar, and in the chat box, type a question about your code. Use the `#` symbol to specify a file.
  - Example: "What's the fastest way to install lemonade in #getting_started.md?"
- Use /Fix to find and fix a minor bug.
- Use /Document to come up with docstrings and comments for a file.
- Use /UnitTest to make a  test file.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->