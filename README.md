## 🍋 Lemonade: Refreshingly fast local AI

<p align="center">
  <a href="https://discord.gg/5xXzkMu8Zk">
    <img src="https://img.shields.io/badge/Discord-7289DA?logo=discord&logoColor=white" alt="Discord" /></a>
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

Lemonade is the local AI server that gives you the same capabilities as cloud APIs, except 100% free and private. Use the latest models for chat, coding, speech, and image generation on your own NPU and GPU.

Lemonade comes in two flavors:

* **Lemonade Server** installs a service you can connect to hundreds of great apps using standard OpenAI, Anthropic, and Ollama APIs.
* **Embeddable Lemonade** is a portable binary you can package into your own application to give it multi-modal local AI that auto-optimizes for your user’s PC.

*This project is built by the community for every PC, with optimizations by AMD engineers to get the most from Ryzen AI, Radeon, and Strix Halo PCs.*

## Getting Started

1. **Install**: [Windows](https://lemonade-server.ai/install_options.html#windows) · [Linux](https://lemonade-server.ai/install_options.html#linux) · [macOS (beta)](https://lemonade-server.ai/install_options.html#macos) · [Docker](https://lemonade-server.ai/install_options.html#docker) · [Source](./docs/dev-getting-started.md)
2. **Get Models**: Browse and download with the [Model Manager](#model-library)
3. **Generate**: Try models with the built-in interfaces for chat, image gen, speech gen, and more
4. **Mobile**: Take your lemonade to go: [iOS](https://apps.apple.com/us/app/lemonade-mobile/id6757372210) · [Android](https://play.google.com/store/apps/details?id=com.lemonade.mobile.chat.ai&pli=1) · [Source](https://github.com/lemonade-sdk/lemonade-mobile)
5. **Connect**: Use Lemonade with your [favorite apps](https://lemonade-server.ai/marketplace):

<!-- MARKETPLACE_START -->
<p align="center">
  <a href="https://quickthoughts.ca/posts/firefox-chatback-lemonade-sdk/" title="Firefox Chatbot"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/fx-chatbot/logo.png" alt="Firefox Chatbot" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/anythingLLM/" title="AnythingLLM"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/anythingllm/logo.png" alt="AnythingLLM" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/continue/" title="Continue"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/continue/logo.png" alt="Continue" width="60" /></a>&nbsp;&nbsp;<a href="https://marketplace.dify.ai/plugins/langgenius/lemonade" title="Dify"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/dify/logo.png" alt="Dify" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/amd/gaia?tab=readme-ov-file#getting-started-guide" title="GAIA"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/gaia/logo.png" alt="GAIA" width="60" /></a>&nbsp;&nbsp;<a href="https://admcpr.com/local-github-copilot-with-lemonade-server-on-windows" title="GitHub Copilot"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/github-copilot/logo.png" alt="GitHub Copilot" width="60" /></a>&nbsp;&nbsp;<a href="https://github.com/lemonade-sdk/infinity-arcade" title="Infinity Arcade"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/infinity-arcade/logo.png" alt="Infinity Arcade" width="60" /></a>&nbsp;&nbsp;<a href="https://n8n.io/integrations/lemonade-model/" title="n8n"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/n8n/logo.png" alt="n8n" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/open-webui/" title="Open WebUI"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/open-webui/logo.png" alt="Open WebUI" width="60" /></a>&nbsp;&nbsp;<a href="https://lemonade-server.ai/docs/server/apps/open-hands/" title="OpenHands"><img src="https://raw.githubusercontent.com/lemonade-sdk/marketplace/main/apps/openhands/logo.png" alt="OpenHands" width="60" /></a>
</p>

<p align="center"><em>Want your app featured here? <a href="https://github.com/lemonade-sdk/marketplace">Just submit a marketplace PR!</a></em></p>
<!-- MARKETPLACE_END -->

## Supported Platforms

| Platform | Build |
|----------|-------|
| [![Arch Linux](https://img.shields.io/badge/Arch%20Linux-supported-1793D1?logo=arch-linux&logoColor=white)](https://lemonade-server.ai/install_options.html#arch) | [![Build on Arch](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/linux_distro_builds.yml?branch=main&label=Build%20on%20Arch)](https://github.com/lemonade-sdk/lemonade/actions/workflows/linux_distro_builds.yml) |
| [![Debian Trixie+](https://img.shields.io/badge/Debian-Trixie%2B-A81D33?logo=debian&logoColor=white)](https://lemonade-server.ai/install_options.html#debian) | [![Build on Debian](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/linux_distro_builds.yml?branch=main&label=Build%20on%20Debian)](https://github.com/lemonade-sdk/lemonade/actions/workflows/linux_distro_builds.yml) |
| [![Docker](https://img.shields.io/badge/Docker-supported-2496ED?logo=docker&logoColor=white)](https://lemonade-server.ai/install_options.html#docker) | [![Build Container Image](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/build-and-push-container.yml?branch=main&label=Build%20Container%20Image)](https://github.com/lemonade-sdk/lemonade/actions/workflows/build-and-push-container.yml) |
| [![Fedora 43](https://img.shields.io/badge/Fedora-43-294172?logo=fedora&logoColor=white)](https://lemonade-server.ai/install_options.html#fedora) | [![Build .rpm](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/cpp_server_build_test_release.yml?branch=main&label=Build%20.rpm)](https://github.com/lemonade-sdk/lemonade/actions/workflows/cpp_server_build_test_release.yml) |
| [![macOS beta](https://img.shields.io/badge/macOS-beta-999999?logo=apple&logoColor=white)](https://lemonade-server.ai/install_options.html#macos) | [![Build .pkg](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/cpp_server_build_test_release.yml?branch=main&label=Build%20.pkg)](https://github.com/lemonade-sdk/lemonade/actions/workflows/cpp_server_build_test_release.yml) |
| [![Snap](https://img.shields.io/badge/Snap-supported-82BEA0?logo=snapcraft&logoColor=white)](https://snapcraft.io/lemonade-server) | [![Build Snap](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade-server-snap/snap-build.yaml?branch=main&label=Build%20Snap)](https://github.com/lemonade-sdk/lemonade-server-snap/actions/workflows/snap-build.yaml) |
| [![Ubuntu 24.04+](https://img.shields.io/badge/Ubuntu-24.04%2B-E95420?logo=ubuntu&logoColor=white)](https://lemonade-server.ai/install_options.html#ubuntu) | [![Build Launchpad PPA](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/launchpad-ppa.yml?branch=main&label=Build%20Launchpad%20PPA)](https://github.com/lemonade-sdk/lemonade/actions/workflows/launchpad-ppa.yml) |
| [![Windows 11](https://img.shields.io/badge/Windows-11-0078D6?logo=windows&logoColor=white)](https://lemonade-server.ai/install_options.html#windows) | [![Build .msi](https://img.shields.io/github/actions/workflow/status/lemonade-sdk/lemonade/cpp_server_build_test_release.yml?branch=main&label=Build%20.msi)](https://github.com/lemonade-sdk/lemonade/actions/workflows/cpp_server_build_test_release.yml) |

## Using the CLI

To run and chat with Gemma 3:

```
lemonade run Gemma-4-E2B-it-GGUF
```

More modalities:

```
# image gen
lemonade run SDXL-Turbo

# speech gen
lemonade run kokoro-v1

# transcription
lemonade run Whisper-Large-v3-Turbo
```

To see available models and download them:

```
lemonade list

lemonade pull Gemma-4-E2B-it-GGUF
```

To see the backends available on your PC:

```
lemonade backends
```


## Model Library

<img align="right" src="https://github.com/lemonade-sdk/assets/blob/main/docs/model_manager_02.png?raw=true" alt="Model Manager" width="280" />

Lemonade supports a wide variety of LLMs (**GGUF**, **FLM**, and **ONNX**), whisper, stable diffusion, etc. models across CPU, GPU, and NPU.

Use `lemonade pull` or the built-in **Model Manager** to download models. You can also import custom GGUF/ONNX models from Hugging Face.

**[Browse all built-in models →](https://lemonade-server.ai/models.html)**

<br clear="right"/>

## Supported Configurations

Lemonade supports multiple inference engines for LLM, speech, TTS, and image generation, and each has its own backend and hardware requirements.

<table>
  <thead>
    <tr>
      <th>Modality</th>
      <th>Engine</th>
      <th>Backend</th>
      <th>Device</th>
      <th>OS</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td rowspan="7"><strong>Text generation</strong></td>
      <td rowspan="5"><code>llamacpp</code></td>
      <td><code>vulkan</code></td>
      <td><code>x86_64</code> CPU, AMD iGPU, AMD dGPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>rocm</code></td>
      <td>Supported AMD ROCm iGPU/dGPU families*</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>cpu</code></td>
      <td><code>x86_64</code> CPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>metal</code></td>
      <td>Apple Silicon GPU</td>
      <td>macOS (beta)</td>
    </tr>
    <tr>
      <td><code>system</code></td>
      <td><code>x86_64</code> CPU, GPU</td>
      <td>Linux</td>
    </tr>
    <tr>
      <td><code>flm</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><code>ryzenai-llm</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows</td>
    </tr>
    <tr>
      <td rowspan="3"><strong>Speech-to-text</strong></td>
      <td rowspan="3"><code>whispercpp</code></td>
      <td><code>npu</code></td>
      <td>XDNA2 NPU</td>
      <td>Windows</td>
    </tr>
    <tr>
      <td><code>vulkan</code></td>
      <td><code>x86_64</code> CPU</td>
      <td>Linux</td>
    </tr>
    <tr>
      <td><code>cpu</code></td>
      <td><code>x86_64</code> CPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td><strong>Text-to-speech</strong></td>
      <td><code>kokoro</code></td>
      <td><code>cpu</code></td>
      <td><code>x86_64</code> CPU</td>
      <td>Windows, Linux</td>
    </tr>
    <tr>
      <td rowspan="2"><strong>Image generation</strong></td>
      <td rowspan="2"><code>sd-cpp</code></td>
      <td><code>rocm</code></td>
      <td>Supported AMD ROCm iGPU/dGPU families*</td>
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
lemonade backends
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
| Native multi-modal tool calling | vLLM support           | Embeddable binary release           |
| More whisper.cpp backends | Port app to Tauri            | Image generation       |
| More SD.cpp backends      |                              | Speech-to-text         |
| MLX support               |                              | Text-to-speech         |
|                           |                              | Apps marketplace       |


## Integrate Embeddable Lemonade in You Application

Embeddable Lemonade is a binary version of Lemonade that you can bundle into your own app to give it a portable, auto-optimizing, multi-modal local AI stack. This lets users focus on your app, with zero Lemonade installers, branding, or telemetry.

Check out the [Embeddable Lemonade guide](docs/embeddable/README.md).


## Connect Lemonade Server to Your Application

You can use any OpenAI-compatible client library by configuring it to use `http://localhost:13305/v1` as the base URL. A table containing official and popular OpenAI clients on different languages is shown below.

Feel free to pick and choose your preferred language.


| Python | C++ | Java | C# | Node.js | Go | Ruby | Rust | PHP |
|--------|-----|------|----|---------|----|-------|------|-----|
| [openai-python](https://github.com/openai/openai-python) | [openai-cpp](https://github.com/olrea/openai-cpp) | [openai-java](https://github.com/openai/openai-java) | [openai-dotnet](https://github.com/openai/openai-dotnet) | [openai-node](https://github.com/openai/openai-node) | [go-openai](https://github.com/sashabaranov/go-openai) | [ruby-openai](https://github.com/alexrudall/ruby-openai) | [async-openai](https://github.com/64bit/async-openai) | [openai-php](https://github.com/openai-php/client) |


### Python Client Example
```python
from openai import OpenAI

# Initialize the client to use Lemonade Server
client = OpenAI(
    base_url="http://localhost:13305/api/v1",
    api_key="lemonade"  # required but unused
)

# Create a chat completion
completion = client.chat.completions.create(
    model="Gemma-4-E2B-it-GGUF",  # or any other available model
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

Lemonade is built by the local AI community! If you would like to contribute to this project, please check out our [contribution guide](./docs/contribute.md).

## Maintainers

This is a community project maintained by @amd-pworfolk @bitgamma @danielholanda @jeremyfowers @kenvandine @Geramy @ramkrishna2910 @sawansri @siavashhub @sofiageo @superm1 @vgodsoe, and sponsored by AMD. You can reach us by filing an [issue](https://github.com/lemonade-sdk/lemonade/issues), emailing [lemonade@amd.com](mailto:lemonade@amd.com), or joining our [Discord](https://discord.gg/5xXzkMu8Zk).

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
- Licensed under the [Apache 2.0 License](https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE).
  - Portions of the project are licensed as described in [NOTICE.md](./NOTICE.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
