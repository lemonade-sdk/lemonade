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

### Pull the desired models with Lemonade

Ensure that the models that are to be integrated with OpenHands are within the Lemonade server. Run the following command to pull the desired models into Lemonade: 

```bash
lemonade-server pull <model-name>
```

### Installing OpenHands

Follow the instruction on the [OpenHands github](https://github.com/All-Hands-AI/OpenHands/) on how to install OpenHands locally. This can be done through the `uvx` tool or through `docker`. No special installation instructions are necessary to integrate with Lemonade. In the next section we will show how to configure OpenHands to talk to a local model running via Lemonade Server. 

## Launching OpenHands

Launch OpenHands in the browser and go to `localhost:3000`. When first launching the application, it will prompt to configure the model. 

1. When prompted, click on `see advanced settings`. 

2. Click the `Advanced` switch to see all of the configurations 

3. Set the following values in the configuration:

<img width="953" height="502" alt="advanced-configuration" src="https://github.com/user-attachments/assets/4c710fdd-489f-4b55-8efc-faf6096a068a" />
    
4. Click `Save Settings` to save the configuration. 

## Examples

1. After hitting `Save Configuration` hit the `+` on the top left and hit `Launch from Scratch` to launch a new project. 

2. Wait for the runtime to start. This will take 1-2 minutes and you can track the progress on the bottom left. 

3. Input the prompt "Write me a flask website that prints "Welcome to OpenHands + Lemonade!" make the website fun with a theme of lemons and laptops.".

4. After processing the initial prompt, the agent’s actions will begin appearing on the left side of the screen. Any files it creates will show up on the right.


<img width="947" height="503" alt="first-prompt" src="https://github.com/user-attachments/assets/78e7d87f-effa-4a7a-a7ee-1ec3b01917d1" />

6. As a user, you can interact with the environment where the software agent operates to monitor progress and view generated artifacts. Below is an example using the Web Browser feature to preview the website created by the agent.


<img width="947" height="497" alt="completed-prompt" src="https://github.com/user-attachments/assets/5dd394bc-0f7f-4263-8019-02fd99534b2d" />

7. That's it! You just created a website from scratch using OpenHands integrated with a local LLM powered by Lemonade Server.

## Common Issues

* Certain small models can struggle with tool calling which can cause them to run in infinite loops. This can be seen by the agent continously running the same command that is resulting in an error. When this occurs, we recommend stopping the agent and providing a prompt to get the agent out of the loop or using a model that is better at using the tools provided by OpenHands.  

## Resources

* [OpenHands github](https://github.com/All-Hands-AI/OpenHands/)
* [OpenHands Documentation](https://docs.all-hands.dev/)
* [OpenHands Documentation on integrating with local models](https://docs.all-hands.dev/usage/llms/local-llms/)

