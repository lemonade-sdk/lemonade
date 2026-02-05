## 🍋 Lemonade: Local LLMs with GPU and NPU acceleration

<div align="center">

| Community & Support | Platform Support |
|:---:|:---:|
| [![Discord](https://img.shields.io/badge/Discord-7289DA?logo=discord&logoColor=white)](https://discord.gg/5xXzkMu8Zk) | [![Windows 11](https://img.shields.io/badge/Windows-11-0078D6?logo=windows&logoColor=white)](docs/README.md#installation) |
| [![PRs Welcome](https://img.shields.io/badge/PRs-welcome-brightgreen.svg)](https://github.com/lemonade-sdk/lemonade/blob/main/docs/contribute.md) | [![Ubuntu 24.04 \| 25.04](https://img.shields.io/badge/Ubuntu-24.04%20%7C%2025.04-E95420?logo=ubuntu&logoColor=white)](https://lemonade-server.ai/install_options.html#ubuntu) |
| [![GitHub issues](https://img.shields.io/github/issues/lemonade-sdk/lemonade)](https://github.com/lemonade-sdk/lemonade/issues) | [![Snap Store](https://snapcraft.io/lemonade-server/badge.svg)](https://snapcraft.io/lemonade-server) |
| | [![Arch Linux](https://img.shields.io/aur/version/lemonade-server)](https://lemonade-server.ai/install_options.html#arch) |

| Release & Stats | License & Analytics | CI/CD & Build |
|:---:|:---:|:---:|
| [![Latest Release](https://img.shields.io/github/v/release/lemonade-sdk/lemonade?include_prereleases)](https://github.com/lemonade-sdk/lemonade/releases/latest) | [![License: Apache](https://img.shields.io/badge/License-Apache-yellow.svg)](https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE) | [![Lemonade Server Build](https://github.com/lemonade-sdk/lemonade/actions/workflows/cpp_server_build_test_release.yml/badge.svg)](https://github.com/lemonade-sdk/lemonade/tree/main/test) |
| [![GitHub downloads](https://img.shields.io/github/downloads/lemonade-sdk/lemonade/total.svg)](https://tooomm.github.io/github-release-stats/?username=lemonade-sdk&repository=lemonade) | [![Star History Chart](https://img.shields.io/badge/Star%20History-View-brightgreen)](https://star-history.com/#lemonade-sdk/lemonade) | [![Code style: black](https://img.shields.io/badge/code%20style-black-000000.svg)](https://github.com/psf/black) |

</div>
<p align="center">
  <img src="https://github.com/lemonade-sdk/assets/blob/main/docs/banner_02.png?raw=true" alt="Lemonade Banner" />
</p>
<h3 align="center">
  <a href="https://lemonade-server.ai/install_options.html">Download</a> |
  <a href="https://lemonade-server.ai/docs/">Documentation</a> |
  <a href="https://discord.gg/5xXzkMu8Zk">Discord</a>
</h3>

Lemonade helps users discover and run local AI apps by serving optimized LLMs right from their own GPUs and NPUs.

Apps like [n8n](https://n8n.io/integrations/lemonade-model/), [VS Code Copilot](https://marketplace.visualstudio.com/items?itemName=lemonade-sdk.lemonade-sdk), [Morphik](https://www.morphik.ai/docs/local-inference#lemonade), and many more use Lemonade to seamlessly run LLMs on any PC.

## Getting Started

1. **Install**: [Windows](https://lemonade-server.ai/install_options.html#windows) · [Linux](https://lemonade-server.ai/install_options.html#linux) · [Docker](https://lemonade-server.ai/install_options.html#docker) · [Source](https://lemonade-server.ai/install_options.html)
2. **Get Models**: Browse and download with the [Model Manager](#model-library)
3. **Chat**: Try models with the built-in chat interface
4. **Mobile**: Take your lemonade to go: [iOS](https://apps.apple.com/us/app/lemonade-mobile/id6757372210) · Android (soon) · [Source](https://github.com/lemonade-sdk/lemonade-mobile)
5. **Connect**: Use Lemonade with your favorite apps:

<p align="center">
  <a href="https://lemonade-server.ai/docs/server/apps/open-webui/" title="Open WebUI"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/refs/heads/main/partner_logos/openwebui.jpg" alt="Open WebUI" width="60" /></a>&nbsp;&nbsp;<a href="https://n8n.io/integrations/lemonade-model/" title="n8n"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/n8n.png" alt="n8n" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/amd/gaia" title="Gaia"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/gaia.png" alt="Gaia" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/lemonade-sdk/infinity-arcade" title="Infinity Arcade"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/infinity_arcade.png" alt="Infinity Arcade" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/continue/" title="Continue"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/continue.png" alt="Continue" width="60" /></a>&nbsp;&nbsp;<a href="https://marketplace.visualstudio.com/items?itemName=lemonade-sdk.lemonade-sdk" title="GitHub Copilot"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/github_copilot.png" alt="GitHub Copilot" width="60" /></a>&nbsp;&nbsp;<a href="https://openhands.dev/" title="OpenHands"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/openhands.png" alt="OpenHands" width="60" /></a>&nbsp;&nbsp;<a href="https://marketplace.dify.ai/plugins/langgenius/lemonade" title="Dify"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/dify.png" alt="Dify" width="60" /></a>&nbsp;&nbsp;<a href="https://deeptutor.knowhiz.us/" title="Deep Tutor"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/deep_tutor.png" alt="Deep Tutor" width="60" /></a>&nbsp;&nbsp;<a href="https://www.iterate.ai/" title="Iterate.ai"><img src="https://raw.githubusercontent.com/lemonade-sdk/assets/main/app/marketplace/iterate_ai.png" alt="Iterate.ai" width="60" /></a>
</p>

<p align="center"><em>Want your app featured here? <a href="https://discord.gg/5xXzkMu8Zk">Discord</a> · <a href="https://github.com/lemonade-sdk/lemonade/issues">GitHub Issue</a> · <a href="mailto:lemonade@amd.com">Email</a></em></p>

## Using the CLI

To run and chat with Gemma 3:

```
lemonade-server run Gemma-3-4b-it-GGUF
```

To install models ahead of time, use the `pull` command:

```
lemonade-server pull Gemma-3-4b-it-GGUF
```

To check all models available, use the `list` command:

```
lemonade-server list
```

> **Tip**: You can use `--llamacpp vulkan/rocm` to select a backend when running GGUF models.


## Model Library

<img align="right" src="https://github.com/lemonade-sdk/assets/blob/main/docs/model_manager_02.png?raw=true" alt="Model Manager" width="280" />

Lemonade supports **GGUF**, **FLM**, and **ONNX** models across CPU, GPU, and NPU (see [supported configurations](#supported-configurations)).

Use `lemonade-server pull` or the built-in **Model Manager** to download models. You can also import custom GGUF/ONNX models from Hugging Face.

**[Browse all built-in models →](https://lemonade-server.ai/docs/server/server_models/)**

<br clear="right"/>

## Image Generation

Lemonade supports image generation using Stable Diffusion models via [stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp).

```bash
# Pull an image generation model
lemonade-server pull SD-Turbo

# Start the server
lemonade-server serve
```

Available models: **SD-Turbo** (fast, 4-step), **SDXL-Turbo**, **SD-1.5**, **SDXL-Base-1.0**

> See `examples/api_image_generation.py` for complete examples.

## Supported Configurations

Lemonade supports the following configurations, while also making it easy to switch between them at runtime.

| Hardware | Engine: OGA | Engine: llamacpp | Engine: FLM | Windows | Linux |
|----------|-------------|------------------|------------|---------|-------|
| **🧠 CPU** | All platforms | All platforms | - | ✅ | ✅ |
| **🎮 GPU** | — | Vulkan: All platforms<br>ROCm: Selected AMD platforms*<br>Metal: Apple Silicon | — | ✅ | ✅ |
| **🤖 NPU** | AMD Ryzen™ AI 300 series | — | Ryzen™ AI 300 series | ✅ | — |

<details>
<summary><small><i>* See supported AMD ROCm platforms</i></small></summary>

<br>

<table>
  <thead>
    <tr>
      <th>Architecture</th>
      <th>Platform Support</th>
      <th>GPU Models</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td><b>gfx1151</b> (STX Halo)</td>
      <td>Windows, Ubuntu</td>
      <td>Ryzen AI MAX+ Pro 395</td>
    </tr>
    <tr>
      <td><b>gfx120X</b> (RDNA4)</td>
      <td>Windows, Ubuntu</td>
      <td>Radeon AI PRO R9700, RX 9070 XT/GRE/9070, RX 9060 XT</td>
    </tr>
    <tr>
      <td><b>gfx110X</b> (RDNA3)</td>
      <td>Windows, Ubuntu</td>
      <td>Radeon PRO W7900/W7800/W7700/V710, RX 7900 XTX/XT/GRE, RX 7800 XT, RX 7700 XT</td>
    </tr>
  </tbody>
</table>
</details>

## Project Roadmap

| Under Development                                 | Under Consideration                            | Recently Completed                       |
|---------------------------------------------------|------------------------------------------------|------------------------------------------|
| macOS    | vLLM support                                   | Image generation (stable-diffusion.cpp)                 |
| Apps marketplace    | Text to speech     | General speech-to-text support (whisper.cpp)  |
| lemonade-eval CLI     | MLX support                       | ROCm support for Ryzen AI 360-375 (Strix) APUs                     |
|      | ryzenai-server dedicated repo                               | Lemonade desktop app                     |
|      | Enhanced custom model support                               |                      |

## Integrate Lemonade Server with Your Application

You can use any OpenAI-compatible client library by configuring it to use `http://localhost:8000/api/v1` as the base URL. A table containing official and popular OpenAI clients on different languages is shown below.

Feel free to pick and choose your preferred language.


| Python | C++ | Java | C# | Node.js | Go | Ruby | Rust | PHP |
|--------|-----|------|----|---------|----|-------|------|-----|
| [openai-python](https://github.com/openai/openai-python) | [openai-cpp](https://github.com/olrea/openai-cpp) | [openai-java](https://github.com/openai/openai-java) | [openai-dotnet](https://github.com/openai/openai-dotnet) | [openai-node](https://github.com/openai/openai-node) | [go-openai](https://github.com/sashabaranov/go-openai) | [ruby-openai](https://github.com/alexrudall/ruby-openai) | [async-openai](https://github.com/64bit/async-openai) | [openai-php](https://github.com/openai-php/client) |


### Python Client Example
```python
from openai import OpenAI

# Initialize the client to use Lemonade Server
client = OpenAI(
    base_url="http://localhost:8000/api/v1",
    api_key="lemonade"  # required but unused
)

# Create a chat completion
completion = client.chat.completions.create(
    model="Llama-3.2-1B-Instruct-Hybrid",  # or any other available model
    messages=[
        {"role": "user", "content": "What is the capital of France?"}
    ]
)

# Print the response
print(completion.choices[0].message.content)
```

For more detailed integration instructions, see the [Integration Guide](./docs/server/server_integration.md).

## FAQ

To read our frequently asked questions, see our [FAQ Guide](./docs/faq.md)

## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](./docs/contribute.md).

New contributors can find beginner-friendly issues tagged with "Good First Issue" to get started.

<a href="https://github.com/lemonade-sdk/lemonade/issues?q=is%3Aissue+is%3Aopen+label%3A%22good+first+issue%22">
  <img src="https://img.shields.io/badge/🍋Lemonade-Good%20First%20Issue-yellowgreen?colorA=38b000&colorB=cccccc" alt="Good First Issue" />
</a>

## Maintainers

This is a community project maintained by @amd-pworfolk @bitgamma @danielholanda @jeremyfowers @Geramy @ramkrishna2910 @siavashhub @sofiageo @vgodsoe, and sponsored by AMD. You can reach us by filing an [issue](https://github.com/lemonade-sdk/lemonade/issues), emailing [lemonade@amd.com](mailto:lemonade@amd.com), or joining our [Discord](https://discord.gg/5xXzkMu8Zk).

## License and Attribution

This project is:
- Built with C++ (server) and Python (SDK) with ❤️ for the open source community,
- Standing on the shoulders of great tools from:
  - [ggml/llama.cpp](https://github.com/ggml-org/llama.cpp)
  - [OnnxRuntime GenAI](https://github.com/microsoft/onnxruntime-genai)
  - [Hugging Face Hub](https://github.com/huggingface/huggingface_hub)
  - [OpenAI API](https://github.com/openai/openai-python)
  - [IRON/MLIR-AIE](https://github.com/Xilinx/mlir-aie)
  - and more...
- Accelerated by mentorship from the OCV Catalyst program.
- Licensed under the [Apache 2.0 License](https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE).
  - Portions of the project are licensed as described in [NOTICE.md](./NOTICE.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
