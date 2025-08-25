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
lemonade-server --host 0.0.0.0 --ctx-size 32768
```

### Installing OpenHands

Follow the [OpenHands documentation](https://docs.all-hands.dev/usage/local-setup#local-llm-e-g-lm-studio-llama-cpp-ollama) on how to install OpenHands locally. This can be done via the `uvx` tool or through `docker`. No special installation instructions are necessary to integrate with Lemonade. In the next section, we will show how to configure OpenHands to talk to a local model running via Lemonade Server. 

## Launching OpenHands

To launch OpenHands, open a browser and navigate to `localhost:3000`. When first launching the application, the "AI Provider Configuration" window will appear. Click on `see advanced settings` as shown in the image below:
<img width="1221" height="740" alt="annotated-ai-provider-configuration" src="https://github.com/user-attachments/assets/1e8b65cd-2409-4e94-92c6-f96adaede491" />

1. Once in the Settings menu, toggle the `Advanced` switch to see all configuration options.

2. Set the following values in the configuration:


* **Custom Model**: `openai/Qwen3-Coder-30B-A3B-Instruct-GGUF`
* **Base URL**: `https://host.docker.internal:8000/api/v1/`
* **API Key**: Use a dash or any character.

The setup should look as follows::

<img width="953" height="502" alt="advanced-configuration" src="https://github.com/user-attachments/assets/4c710fdd-489f-4b55-8efc-faf6096a068a" />

    
3. Click `Save Settings`. 

## Using OpenHands

1. To launch a new project, click the `+` on the top left.

2. To launch a new project, simply enter your prompt into the text box. For example: "Write me a flask website that prints "Welcome to OpenHands + Lemonade!" make the website fun with a theme of lemons and laptops." as shown below:
<img width="1911" height="1071" alt="prompt-image" src="https://github.com/user-attachments/assets/29348e0b-c741-44aa-a734-e91bb06e28a7" />

3. Hit `Enter` to start off the process. This will bring you to a new screen that allows you to monitor the agent operating in its environment to develop the requested application. An example of the agent working on the requested application can be seen below:
<img width="1905" height="1058" alt="running-commands" src="https://github.com/user-attachments/assets/069ff16c-11d4-46ea-93e0-9cf8571c2044" />

4. When complete, the user can interact with the environment and artifacts created by the software agent. An image of the workspace at the end of developing the application can be seen below. Note that in the `Terminal` at the bottom the software agent has already started the web server hosting the website we requested it to develop at port number `51317`.  
<img width="1906" height="1072" alt="actual-finished-workspace" src="https://github.com/user-attachments/assets/123c496d-a158-423d-8d0d-713416c9326b" />

5. Use your browser to go to the web application developed by the software agent. Below is an image showing what was created:
<img width="1897" height="1068" alt="web-app" src="https://github.com/user-attachments/assets/cbaaf7bd-bc01-4c24-aa33-84ee509ca09a" />

6. That's it! You just created a website from scratch using OpenHands integrated with a local LLM powered by Lemonade Server.

**Suggestions on what to try after:** Prompt OpenHands with Lemonade Server to develop some simple games that you can play via a web browser. For example, with the prompt "Write me a simple pong game that I can play on my browser. Make it so I can use the up and down arrows to control my side of the game." OpenHands with Lemonade Server was able to generate the following pong game which included user-controls, a computer controlled oponent, and score-keeping:
<img width="1910" height="1070" alt="pong-game" src="https://github.com/user-attachments/assets/e1b84777-72b7-49c3-afa0-5f78379842be" />

## Common Issues

* Certain small models can struggle with tool calling. This can be seen by the agent continously running the same command that is resulting in an error. For example, we have found that it is common for certain models to initially struggle with the tool required to create files. In our experience after multiple attempts, the model is able to figure out that it is not using the tool correctly and tries another method to use the tool. An example of this can be seen below. If this issue persists we recommend trying another model or prompt.    
<img width="1528" height="849" alt="tool-calling-struggles" src="https://github.com/user-attachments/assets/2e4cc756-4c0b-42ec-bdf8-dde541f30cf6" />

## Resources

* [OpenHands GitHub](https://github.com/All-Hands-AI/OpenHands/)

* [OpenHands Documentation](https://docs.all-hands.dev/)
* [OpenHands Documentation on integrating with local models](https://docs.all-hands.dev/usage/llms/local-llms/)





