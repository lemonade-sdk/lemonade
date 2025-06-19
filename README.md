[![Lemonade tests](https://github.com/lemonade-sdk/lemonade/actions/workflows/test_lemonade.yml/badge.svg)](https://github.com/lemonade-sdk/lemonade/tree/main/test "Check out our tests")
[![OS - Windows | Linux](https://img.shields.io/badge/OS-windows%20%7C%20linux-blue)](docs/README.md#installation "Check out our instructions")
[![Made with Python](https://img.shields.io/badge/Python-3.8,3.10-blue?logo=python&logoColor=white)](docs/README.md#installation "Check out our instructions")

## 🍋 Lemonade SDK: Quickly serve, benchmark and deploy LLMs

The [Lemonade SDK](./docs/README.md) makes it easy to run Large Language Models (LLMs) on your PC. Our focus is using the best tools, such as neural processing units (NPUs) and Vulkan GPU acceleration, to maximize LLM speed and responsiveness.

<div align="center">
  <img src="https://download.amd.com/images/lemonade_640x480_1.gif" alt="Lemonade Demo" title="Lemonade in Action">
</div>

### Features

The [Lemonade SDK](./docs/README.md) is comprised of the following:

- 🌐 **[Lemonade Server](https://lemonade-server.ai/docs)**: A local LLM server for running ONNX and GGUF models using the OpenAI API standard. Install and enable your applications with NPU and GPU acceleration in minutes.
- 🐍 **Lemonade API**: High-level Python API to directly integrate Lemonade LLMs into Python applications.
- 🖥️ **Lemonade CLI**: The `lemonade` CLI lets you mix-and-match LLMs (ONNX, GGUF, SafeTensors) with measurement tools to characterize your models on your hardware. The available tools are:
  - Prompting with templates.
  - Measuring accuracy with a variety of tests.
  - Benchmarking to get the time-to-first-token and tokens per second.
  - Profiling the memory utilization.

### [Click here to get started with Lemonade.](./docs/README.md)

### Supported Configurations

Maximum LLM performance requires the right hardware accelerator with the right inference engine for your scenario. Lemonade supports the following configurations, while also making it easy to switch between them at runtime.

<table border="1" cellpadding="6" cellspacing="0">
  <thead>
    <tr>
      <th rowspan="2">Hardware</th>
      <th colspan="3" align="center">🛠️ Engine Support</th>
      <th colspan="2" align="center">🖥️ OS (x86/x64)</th>
    </tr>
    <tr>
      <th align="center">OGA</th>
      <th align="center">llamacpp</th>
      <th align="center">HF</th>
      <th align="center">Windows</th>
      <th align="center">Linux</th>
    </tr>
  </thead>
  <tbody>
    <tr>
      <td>🧠 CPU</td>
      <td align="center">All platforms</td>
      <td align="center">All platforms</td>
      <td align="center">All platforms</td>
      <td align="center">✅</td>
      <td align="center">✅</td>
    </tr>
    <tr>
      <td>🎮 GPU</td>
      <td align="center">—</td>
      <td align="center">Vulkan: All platforms<br><small>Focus:<br/>Ryzen™ AI 7000/8000/300<br/>Radeon™ 7000/9000</small></td>
      <td align="center">—</td>
      <td align="center">✅</td>
      <td align="center">✅</td>
    </tr>
    <tr>
      <td>🤖 NPU</td>
      <td align="center">AMD Ryzen™ AI 300 series</td>
      <td align="center">—</td>
      <td align="center">—</td>
      <td align="center">✅</td>
      <td align="center">—</td>
    </tr>
  </tbody>
</table>



#### Inference Engines Overview
| Engine | Description |
| :--- | :--- |
| **OnnxRuntime GenAI (OGA)** | Microsoft engine that runs `.onnx` models and enables hardware vendors to provide their own execution providers (EPs) to support specialized hardware, such as neural processing units (NPUs). |
| **llamacpp** | Community-driven engine with strong GPU acceleration, support for thousands of `.gguf` models, and advanced features such as vision-language models (VLMs) and mixture-of-experts (MoEs). |
| **Hugging Face (HF)** | Hugging Face's `transformers` library can run the original `.safetensors` trained weights for models on Meta's PyTorch engine, which provides a source of truth for accuracy measurement. |

## Integrate Lemonade Server with Your Application

Lemonade Server enables languages including Python, C++, Java, C#, Node.js, Go, Ruby, Rust, and PHP. For the full list and integration details, see [docs/server/README.md](./docs/server/README.md).

## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](./docs/contribute.md).

## Maintainers

This project is sponsored by AMD. It is maintained by @danielholanda @jeremyfowers @ramkrishna @vgodsoe in equal measure. You can reach us by filing an [issue](https://github.com/lemonade-sdk/lemonade/issues) or email [lemonade@amd.com](mailto:lemonade@amd.com).

## License

This project is licensed under the [Apache 2.0 License](https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE). Portions of the project are licensed as described in [NOTICE.md](./NOTICE.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->

