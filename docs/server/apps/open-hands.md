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

Since OpenHands runs inside Docker containers, the containers must be able to access the Lemonade Server. The simplest way to enable this is by running the Lemonade Server on IP address `0.0.0.0`, which is accessible from within Docker. Additionally, OpenHands [recommends](https://docs.all-hands.dev/usage/llms/local-llms) using a context length of at least 32,768 tokens. To configure Lemonade with a non-default context size, include the `--ctx-size` parameter set to `32768`.

```bash
lemonade-server serve --host 0.0.0.0 --ctx-size 32768
```

### Installing OpenHands

Follow the [OpenHands documentation](https://docs.all-hands.dev/usage/local-setup#local-llm-e-g-lm-studio-llama-cpp-ollama) on how to install OpenHands locally. This can be done via the `uvx` tool or through `docker`. No special installation instructions are necessary to integrate with Lemonade.

In the next section, we will show how to configure OpenHands to talk to a local model running via Lemonade Server. 

## Launching OpenHands

To launch OpenHands, open a browser and navigate to http://localhost:3000. When first launching the application, the "AI Provider Configuration" window will appear. Select "Lemonade" as the LLM Provider and your favorite coding model from the drop-down. For a nice balance of quality and speed, we recommend `Qwen3-Coder-30B-A3B-Instruct-GGUF`. When complete, hit `Save Changes`.

<img width="1276" height="677" alt="openhands-llm-settings-wide" src="https://github.com/user-attachments/assets/b39bb75b-1593-48db-a17d-697c872cb7e4" />


## Using OpenHands

1. To launch a new conversation, click `New Conversation`. If you do not see this screen, click the `+` on the top left.
   
<img width="635" height="672" alt="open-hands-main-page" src="https://github.com/user-attachments/assets/bea7438a-a799-46f4-aea1-362d30030a18" />

2. Wait for the status on the bottom right to say `Awaiting user input.` and enter your prompt into the text box. For example: "Create a website that showcases Ryzen AI and the ability to run the OpenHands coding agents locally through the Lemonade software stack. Make the website fun with a theme of lemonade and laptops." as shown below:

<img width="632" height="653" alt="prompt" src="https://github.com/user-attachments/assets/246f85cd-4fbe-45cc-b255-b71f495ebe8a" />

4. Hit `Enter` to start the process. This will bring you to a new screen that allows you to monitor the agent operating in its environment to develop the requested application. An example of the agent working on the requested application can be seen below:

<img width="633" height="671" alt="mid-coding" src="https://github.com/user-attachments/assets/c27fa439-d30a-4042-8809-e4ffd8ef77c5" />

5. When complete, the user can interact with the environment and artifacts created by the software agent. An image of the workspace at the end of developing the application can be seen below. On the left, we can see that the coding agent has launched the web server hosting the newly developed website at port number `55519`.

<img width="632" height="652" alt="finished-code" src="https://github.com/user-attachments/assets/37f8d31e-b610-429b-b757-e7e951bef22a" />

6. Use your browser to go to the web application developed by the software agent. Below is an image showing what was created:

<img width="629" height="653" alt="website" src="https://github.com/user-attachments/assets/f9faaaae-142a-485c-b72a-8f30ff6aee1b" />

7. That's it! You just created a website from scratch using OpenHands integrated with a local LLM powered by Lemonade Server.

**Suggestions on what to try next:** Prompt OpenHands with Lemonade Server to develop some simple games that you can play via a web browser. For example, with the prompt "Write me a simple pong game that I can play on my browser. Make it so I can use the up and down arrows to control my side of the game. Make the game lemon and laptop themed." OpenHands with Lemonade Server was able to generate the following pong game, which included user-controls, a computer-controlled opponent, and scorekeeping:

<img width="668" height="499" alt="pong-game-new" src="https://github.com/user-attachments/assets/5c7568b9-2697-4c3f-9e66-f5a4bdc8b394" />


## Common Issues

* If on OpenHands you get an error with the message: `The request failed with an internal server error` and in the Lemonade log you see many `WARNING: Invalid HTTP request received` this is most likely because the base URL set in the settings is using `https` instead of `http`. If this occurs, update the base URL in the settings to `http://host.docker.internal:8000/api/v1/`

## Resources

* [OpenHands GitHub](https://github.com/All-Hands-AI/OpenHands/)
* [OpenHands Documentation](https://docs.all-hands.dev/)
* [OpenHands Documentation on integrating with local models](https://docs.all-hands.dev/usage/llms/local-llms/)




