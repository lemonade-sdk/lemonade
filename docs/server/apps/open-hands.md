# OpenHands

[OpenHands](https://github.com/All-Hands-AI/OpenHands) is an open-source AI coding agent. This document explains how to configure OpenHands to target local AI models using Lemonade Server, enabling code generation, editing, and chat capabilities. Much of this guide uses the fantastic [guide from OpenHands](https://docs.all-hands.dev/usage/llms/local-llms) on running local models, with added details on integrating with Lemonade Server.

There are a few things to note on this integration:

* This integration is in its early stages. We encourage you to test it and share any issues you encounter—your feedback will help us make the Lemonade–OpenHands functionality as robust as possible.

* Due to the complexity of the scaffolding of agentic software agents, the compute requirements for this application is very high. For a low latency experience, we recommend using a discrete GPU with at least 16 GB of VRAM, or a Strix Halo PC with at least 64 GB of RAM. 


## Prerequisites

- **Docker**: OpenHands leverages Docker containers to create environments for the software agents. To see how to install docker for OpenHands, see their [documentation](https://docs.all-hands.dev/usage/local-setup).
- **Lemonade Server**: Install Lemonade Server using the [Getting Started Guide](https://lemonade-server.ai/docs/server/).
- **Server running**: Ensure Lemonade Server is running on `http://localhost:8000`
- **Models installed**: Ensure at least one model from the [supported models list](https://lemonade-server.ai/docs/server/server_models/) is downloaded locally. For OpenHands functionality, we recommend models denoted with the `coding` label, which can be found in your Lemonade installation's `Model Manager` or in the labels of the [models list](https://lemonade-server.ai/docs/server/server_models/). 


## Installation

### Launch Lemonade Server with the correct settings

Since OpenHands runs inside Docker containers, the containers must be able to access the Lemonade Server. The simplest way to enable this is by running the Lemonade Server on IP address `0.0.0.0`, which is accessible from within Docker. Additionally, OpenHands [recommends](https://docs.all-hands.dev/usage/llms/local-llms) using a context length of at least 32,768 tokens. To configure Lemonade with a non-default context size, include the `--ctx-size` parameter set to `32768`. **Note:** This large context size is currently supported only by the llamacpp backend.

```bash
lemonade-server serve --host 0.0.0.0 --ctx-size 32768
```

### Installing OpenHands

Follow the [OpenHands documentation](https://docs.all-hands.dev/usage/local-setup#local-llm-e-g-lm-studio-llama-cpp-ollama) on how to install OpenHands locally. This can be done via the `uvx` tool or through `docker`. No special installation instructions are necessary to integrate with Lemonade. The only thing that we suggest is that when using models that are able to use tools, such as `Qwen3-Coder-30B-A3B-Instruct-GGUF`, that native tool use is enabled. This can be done by launching OpenHands via docker and adding  `-e LLM_NATIVE_TOOL_CALLING=true` to the `docker run...` command in the OpenHands documentation. 

In the next section, we will show how to configure OpenHands to talk to a local model running via Lemonade Server. 

## Launching OpenHands

To launch OpenHands, open a browser and navigate to http://localhost:3000. When first launching the application, the "AI Provider Configuration" window will appear. Click on `see advanced settings` as shown in the image below:
<img width="567" height="342" alt="configuration" src="https://github.com/user-attachments/assets/6e8d5028-580d-484b-85e0-214f821dd911" />

1. Once in the Settings menu, toggle the `Advanced` switch to see all configuration options.

2. Set the following values in the configuration:

    * **Custom Model**: `openai/Qwen3-Coder-30B-A3B-Instruct-GGUF`
    * **Base URL**: `http://host.docker.internal:8000/api/v1/`
    * **API Key**: Use a dash or any character.

    The setup should look as follows:

    <img width="953" height="502" alt="advanced-configuration" src="https://github.com/user-attachments/assets/4c710fdd-489f-4b55-8efc-faf6096a068a" />

3. Click `Save Settings`. 

## Using OpenHands

1. To launch a new project, click `Launch from Scratch`. If you do not see this screen, click the `+` on the top left.
<img width="955" height="507" alt="intro-screen" src="https://github.com/user-attachments/assets/707652ed-a51a-4f01-a615-87e6fdef1767" />


2. Wait for the status on the bottom right to say `Awaiting user input.` and enter your prompt into the text box. For example: "Write me a flask website that prints "Welcome to OpenHands + Lemonade!" make the website fun with a theme of lemons and laptops." as shown below:
<img width="952" height="502" alt="initial-prompt-lemonade-website" src="https://github.com/user-attachments/assets/6908631f-d9f3-4c4f-95b2-51e052859b39" />

3. Hit `Enter` to start the process. This will bring you to a new screen that allows you to monitor the agent operating in its environment to develop the requested application. An example of the agent working on the requested application can be seen below:
<img width="950" height="502" alt="lemonade-website-in-progress" src="https://github.com/user-attachments/assets/2eb4b0fd-b24d-447f-888b-5e739d559716" />

4. When complete, the user can interact with the environment and artifacts created by the software agent. An image of the workspace at the end of developing the application can be seen below. In the `Terminal` at the bottom, we can see that the software agent has launched the web server hosting the newly developed website at port number `52877`.
<img width="956" height="509" alt="finished-prompt-lemonade-website" src="https://github.com/user-attachments/assets/554b06cf-4593-4a1f-af67-f9b61dca6adb" />


5. Use your browser to go to the web application developed by the software agent. Below is an image showing what was created:
<img width="941" height="500" alt="lemonade-website" src="https://github.com/user-attachments/assets/485b33ad-773a-49d3-8740-255a3bb42bd6" />


6. That's it! You just created a website from scratch using OpenHands integrated with a local LLM powered by Lemonade Server.

**Suggestions on what to try next:** Prompt OpenHands with Lemonade Server to develop some simple games that you can play via a web browser. For example, with the prompt "Write me a simple pong game that I can play on my browser. Make it so I can use the up and down arrows to control my side of the game. Make the game lemon and laptop themed." OpenHands with Lemonade Server was able to generate the following pong game, which included user-controls, a computer-controlled opponent, and scorekeeping:

<img width="668" height="499" alt="pong-game-new" src="https://github.com/user-attachments/assets/5c7568b9-2697-4c3f-9e66-f5a4bdc8b394" />


## Common Issues

* Certain small models can struggle with tool calling. This can be seen by the agent continuously running the same command that is resulting in an error. For example, we have found that it is common for certain models to initially struggle with the tool required to create files. In our experience after multiple attempts, the model is able to figure out that it is not using the tool correctly and tries another method to use the tool. An example of this can be seen below. If this issue persists we recommend enabling native tool calling in OpenHands. This can be done by launching OpenHands via docker and adding `-e LLM_NATIVE_TOOL_CALLING=true` to the `docker run...` command in the OpenHands documentation. 
<img width="1528" height="849" alt="tool-calling-struggles" src="https://github.com/user-attachments/assets/2e4cc756-4c0b-42ec-bdf8-dde541f30cf6" />

* If on OpenHands you get an error with the message: `The request failed with an internal server error` and in the Lemonade log you see many `WARNING: Invalid HTTP request received` this is most likely because the base URL set in the settings is using `https` instead of `http`. If this occurs, update the base URL in the settings to `http://host.docker.internal:8000/api/v1/`

* We have run into some issues where despite the source code for a requested website being generated correctly, it cannot be accessed through the browser. When this happens, you can still copy the generated source into your own environment and run the provided commands to serve the generated website. 


## Resources

* [OpenHands GitHub](https://github.com/All-Hands-AI/OpenHands/)
* [OpenHands Documentation](https://docs.all-hands.dev/)
* [OpenHands Documentation on integrating with local models](https://docs.all-hands.dev/usage/llms/local-llms/)









