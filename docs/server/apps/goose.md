
# Running Goose agents with Lemonade Server

## Overview

[Goose](https://block.github.io/goose/) is an open source local AI agent by Block and is part of the [Agentic AI Foundation](https://aaif.io/). Goose includes a desktop application as well as a console application and both support Mac, Windows, and Linux.

## Setup

### Prerequisites

1. Install Lemonade Server by following the [Lemonade Server Instructions](../README.md)

2. Install and set up Goose from the quickstart documentation Goose provides [here](https://block.github.io/goose/docs/quickstart).

### Configure Goose to Use Lemonade

From the goose cli, you can use `goose configure` to enter configuration mode for goose. You will choose `Configure Providers`. Choose the OpenAI provider. You will be prompted for an OpenAI key. If asked to configure advanced settings, agree to update the OPENAI_HOST. Set this to your lemonade server. The OPENAI_BASE_PATH can remain the default. Finally, you should be able to search for a model you have installed in Lemonade Server and choose the model you wish to use.

The configuration should exit. The next time you run goose you will be using the Lemonade Server.

**Note:** Goose expects to be configured with a model that has tool calling enabled. You will receive an error if you choose a model that does not support tool calling.

Configuration is similar for the Goose desktop app.

## Subagents with Goose

Goose supports using subagents and recipes. You can have a separate recipe for each model your Lemonade Server supports if you like, or even mix and match other providers like Claude or Gemini.

With the variety of model options available in Lemonade Server, from NPU models to Vulkan support, goose can create subagents and recipes based on the models you have installed from OCR to Image detection to chat.

## Additional Resources

- [Goose](https://block.github.io/goose/)
- [Goose Quickstart](https://block.github.io/goose/docs/quickstart)
- [Goose Recipes Guide](https://block.github.io/goose/docs/guides/recipes/)