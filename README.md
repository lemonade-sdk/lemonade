## 🍋 Lemonade: Refreshingly fast local LLMs, Image and Speech Generation

<p align="center">
  <a href="https://discord.gg/5xXzkMu8Zk">
    <img src="https://img.shields.io/badge/Discord-7289DA?logo=discord&logoColor=white" alt="Discord" /></a>
  <a href="https://github.com/lemonade-sdk/lemonade/tree/main/test" title="Check out our tests">
    <img src="https://github.com/lemonade-sdk/lemonade/actions/workflows/cpp_server_build_test_release.yml/badge.svg" alt="Lemonade Server Build" /></a>
  <a href="docs/README.md#installation" title="Check out our instructions">
    <img src="https://img.shields.io/badge/Windows-11-0078D6?logo=windows&logoColor=white" alt="Windows 11" /></a>
  <a href="https://lemonade-server.ai/install_options.html#ubuntu" title="Ubuntu 24.04 & 25.04 Supported">
    <img src="https://img.shields.io/badge/Ubuntu-24.04%20%7C%2025.04-E95420?logo=ubuntu&logoColor=white" alt="Ubuntu 24.04 | 25.04" /></a>
  <a href="https://lemonade-server.ai/install_options.html#macos" title="macOS (beta)">
    <img src="https://img.shields.io/badge/macOS-beta-999999?logo=apple&logoColor=white" alt="macOS (beta)" /></a>
  <a href="https://snapcraft.io/lemonade-server">
    <img src="https://snapcraft.io/lemonade-server/badge.svg" alt="Get it from the Snap Store" /></a>
  <a href="https://lemonade-server.ai/install_options.html#arch" title="Arch Linux Supported">
    <img src="https://img.shields.io/aur/version/lemonade-server" alt="Arch Linux"></a>
  <a href="docs/README.md#installation" title="Check out our instructions">
    <img src="https://img.shields.io/badge/Python-3.10--3.13-blue?logo=python&logoColor=white" alt="Made with Python" /></a>
  <a href="https://github.com/lemonade-sdk/lemonade/blob/main/docs/contribute.md" title="Contribution Guide">
    <img src="https://img.shields.io/badge/PRs-welcome-brightgreen.svg" alt="PRs Welcome" /></a>
  <a href="https://github.com/lemonade-sdk/lemonade/releases/latest" title="Download the latest release">
    <img src="https://img.shields.io/github/v/release/lemonade-sdk/lemonade?include_prereleases" alt="Latest Release" /></a>
  <a href="https://tooomm.github.io/github-release-stats/?username=lemonade-sdk&repository=lemonade">
    <img src="https://img.shields.io/github/downloads/lemonade-sdk/lemonade/total.svg" alt="GitHub downloads" /></a>
  <a href="https://github.com/lemonade-sdk/lemonade/issues">
    <img src="https://img.shields.io/github/issues/lemonade-sdk/lemonade" alt="GitHub issues" /></a>
  <a href="https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE">
    <img src="https://img.shields.io/badge/License-Apache-yellow.svg" alt="License: Apache" /></a>
  <a href="https://github.com/psf/black">
    <img src="https://img.shields.io/badge/code%20style-black-000000.svg" alt="Code style: black" /></a>
  <a href="https://star-history.com/#lemonade-sdk/lemonade">
    <img src="https://img.shields.io/badge/Star%20History-View-brightgreen" alt="Star History Chart" /></a>
</p>
<p align="center">
  <img src="https://github.com/lemonade-sdk/assets/blob/main/docs/banner_02.png?raw=true" alt="Lemonade Banner" />
</p>
<h3 align="center">
  <a href="https://lemonade-server.ai/install_options.html">Download</a> |
  <a href="https://lemonade-server.ai/docs/">Documentation</a> |
  <a href="https://discord.gg/5xXzkMu8Zk">Discord</a>
</h3>

Lemonade helps users discover and run local AI apps by serving optimized LLMs, images, and speech right from their own GPUs and NPUs.

Apps like [n8n](https://n8n.io/integrations/lemonade-model/), [VS Code Copilot](https://marketplace.visualstudio.com/items?itemName=lemonade-sdk.lemonade-sdk), [Morphik](https://www.morphik.ai/docs/local-inference#lemonade), and many more use Lemonade to seamlessly run generative AI on any PC.

## Getting Started

1. **Install**: [Windows](https://lemonade-server.ai/install_options.html#windows) · [Linux](https://lemonade-server.ai/install_options.html#linux) · [macOS (beta)](https://lemonade-server.ai/install_options.html#macos) · [Docker](https://lemonade-server.ai/install_options.html#docker) · [Source](./docs/dev-getting-started.md)
2. **Get Models**: Browse and download with the [Model Manager](#model-library)
3. **Generate**: Try models with the built-in interfaces for chat, image gen, speech gen, and more
4. **Mobile**: Take your lemonade to go: [iOS](https://apps.apple.com/us/app/lemonade-mobile/id6757372210) · [Android](https://play.google.com/store/apps/details?id=com.lemonade.mobile.chat.ai&pli=1) · [Source](https://github.com/lemonade-sdk/lemonade-mobile)
5. **Connect**: Use Lemonade with your favorite apps:

<!-- MARKETPLACE_START -->
<p align="center">
  <a href="https://lemonade-server.ai/docs/server/apps/continue/" title="Continue"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/continue/logo.png" alt="Continue" width="60" /></a>&nbsp;&nbsp;<a href="https://deeptutor.knowhiz.us/" title="Deep Tutor"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/deep-tutor/logo.png" alt="Deep Tutor" width="60" /></a>&nbsp;&nbsp;<a href="https://marketplace.dify.ai/plugins/langgenius/lemonade" title="Dify"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/dify/logo.png" alt="Dify" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/amd/gaia?tab=readme-ov-file#getting-started-guide" title="Gaia"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/gaia/logo.png" alt="Gaia" width="60" /></a>&nbsp;&nbsp;<a href="https://marketplace.visualstudio.com/items?itemName=lemonade-sdk.lemonade-sdk" title="GitHub Copilot"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/github-copilot/logo.png" alt="GitHub Copilot" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/lemonade-sdk/infinity-arcade" title="Infinity Arcade"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/infinity-arcade/logo.png" alt="Infinity Arcade" width="60" /></a>&nbsp;&nbsp;<a href="https://www.iterate.ai/" title="Iterate.ai"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/iterate-ai/logo.png" alt="Iterate.ai" width="60" /></a>&nbsp;&nbsp;<a href="https://n8n.io/integrations/lemonade-model/" title="n8n"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/n8n/logo.png" alt="n8n" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/open-webui/" title="Open WebUI"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/open-webui/logo.png" alt="Open WebUI" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/open-hands/" title="OpenHands"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/openhands/logo.png" alt="OpenHands" width="60" /></a>
</p>

<p align="center"><em><a href="https://lemonade-server.ai/marketplace">View all apps →</a></br>Want your app featured here? <a href="https://github.com/lemonade-sdk/marketplace">Just submit a marketplace PR!</a></em></p>
<!-- MARKETPLACE_END -->

## Using the CLI

To run and chat with Gemma 3:

```
lemonade-server run Gemma-3-4b-it-GGUF
```

More modalities:

```
# image gen
lemonade-server run SDXL-Turbo

# speech gen
lemonade-server run kokoro-v1

# transcription
lemonade-server run Whisper-Large-v3-Turbo
```

To see models availables and download them:

```
lemonade-server list

lemonade-server pull Gemma-3-4b-it-GGUF
```

To see the backends available on your PC:

```
lemonade-server recipes
```


## Model Library

<img align="right" src="https://github.com/lemonade-sdk/assets/blob/main/docs/model_manager_02.png?raw=true" alt="Model Manager" width="280" />

Lemonade supports a wide variety of LLMs (**GGUF**, **FLM**, and **ONNX**), whisper, stable diffusion, etc. models across CPU, GPU, and NPU.

Use `lemonade-server pull` or the built-in **Model Manager** to download models. You can also import custom GGUF/ONNX models from Hugging Face.

**[Browse all built-in models →](https://lemonade-server.ai/models.html)**

<br clear="right"/>

## Supported Configurations

Lemonade supports multiple recipes (LLM, speech, TTS, and image generation), and each recipe has its own backend and hardware requirements.

<table>
  <thead>
    <tr>
      <th>Modality</th>
      <th>Recipe</th>
      <th>Backend</th>
      <th>Device</th>
      <th>OS</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td rowspan="6"><strong>LLM</strong></td>
      <td rowspan="4"><code>llamacpp</code></td>
      <td><code>vulkan</code></td>
      <td>GPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>rocm</code></td>
      <td>Select AMD GPUs*</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>cpu</code></td>
      <td><code>x86_64</code></td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>metal</code></td>
      <td>Apple Silicon GPU</td>
      <td>macOS (beta)</td>
    </tr>
    <tr>
      <td><code>flm</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows</td>
    </tr>
    <tr>
      <td><code>ryzenai-llm</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows</td>
    </tr>
    <tr>
      <td rowspan="2"><strong>Speech-to-text</strong></td>
      <td rowspan="2"><code>whispercpp</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows</td>
    </tr>
    <tr>
      <td><code>cpu</code></td>
      <td><code>x86_64</code></td>
      <td>Windows</td>
    </tr>
    <tr>
      <td><strong>Text-to-speech</strong></td>
      <td><code>kokoro</code></td>
      <td><code>cpu</code></td>
      <td><code>x86_64</code></td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td rowspan="2"><strong>Image generation</strong></td>
      <td rowspan="2"><code>sd-cpp</code></td>
      <td><code>rocm</code></td>
      <td>Selected AMD GPUs</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>cpu</code></td>
      <td><code>x86_64</code> CPU</td>
      <td>Windows, Linux</td>
    </tr>
  </tbody>
</table>

To check exactly which recipes/backends are supported on your own machine, run:

```
lemonade-server recipes
```

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

| Under Development          | Under Consideration         | Recently Completed      |
|---------------------------|-----------------------------|------------------------|
| MLX support               | vLLM support                | macOS (beta)           |
| More whisper.cpp backends | Enhanced custom model usage  | Image generation       |
| More SD.cpp backends      |                              | Speech-to-text         |
|                           |                              | Text-to-speech         |
|                           |                              | Apps marketplace       |

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

This is a community project maintained by @amd-pworfolk @bitgamma @danielholanda @jeremyfowers @Geramy @ramkrishna2910 @siavashhub @sofiageo @superm1 @vgodsoe, and sponsored by AMD. You can reach us by filing an [issue](https://github.com/lemonade-sdk/lemonade/issues), emailing [lemonade@amd.com](mailto:lemonade@amd.com), or joining our [Discord](https://discord.gg/5xXzkMu8Zk).

## Code Signing Policy

Free code signing provided by [SignPath.io](https://signpath.io), certificate by [SignPath Foundation](https://signpath.org).

- **Committers and reviewers**: [Maintainers](#maintainers) of this repo
- **Approvers**: [Owners](https://github.com/orgs/lemonade-sdk/people?query=role%3Aowner)

**Privacy policy**: This program will not transfer any information to other networked systems unless specifically requested by the user or the person installing or operating it. When the user requests it, Lemonade downloads AI models from [Hugging Face Hub](https://huggingface.co/) (see their [privacy policy](https://huggingface.co/privacy)).

## License and Attribution

This project is:
- Built with C++ (server) and React (app) with ❤️ for the open source community,
- Standing on the shoulders of great tools from:
  - [ggml/llama.cpp](https://github.com/ggml-org/llama.cpp)
  - [ggml/whisper.cpp](https://github.com/ggerganov/whisper.cpp)
  - [ggml/stable-diffusion.cpp](https://github.com/leejet/stable-diffusion.cpp)
  - [kokoros](https://github.com/lucasjinreal/Kokoros)
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
