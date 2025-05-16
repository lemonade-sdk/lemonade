[![Lemonade tests](https://github.com/lemonade-sdk/lemonade/actions/workflows/test_lemonade.yml/badge.svg)](https://github.com/lemonade-sdk/lemonade/tree/main/test "Check out our tests")
[![OS - Windows | Linux](https://img.shields.io/badge/OS-windows%20%7C%20linux-blue)](docs/README.md#installation "Check out our instructions")
[![Made with Python](https://img.shields.io/badge/Python-3.8,3.10-blue?logo=python&logoColor=white)](docs/README.md#installation "Check out our instructions")

## 🍋 Lemonade SDK: Quickly serve, benchmark and deploy LLMs

The [Lemonade SDK](./docs/README.md) is designed to make it easy to serve, benchmark, and deploy large language models (LLMs) on a variety of hardware platforms, including CPU, GPU, and NPU. 

<div align="center">
  <img src="https://download.amd.com/images/lemonade_640x480_1.gif" alt="Lemonade Demo" title="Lemonade in Action">
</div>

The [Lemonade SDK](./docs/README.md) is comprised of the following:

- 🌐 **Lemonade Server**: A server interface that uses the standard Open AI API, allowing applications to integrate with local LLMs.
- 🐍 **Lemonade Python API**: Offers High-Level API for easy integration of Lemonade LLMs into Python applications and Low-Level API for custom experiments.
- 🖥️ **Lemonade CLI**: The `lemonade` CLI lets you mix-and-match LLMs, frameworks (PyTorch, ONNX, GGUF), and measurement tools to run experiments. The available tools are:
  - Prompting an LLM.
  - Measuring the accuracy of an LLM using a variety of tests.
  - Benchmarking an LLM to get the time-to-first-token and tokens per second.
  - Profiling the memory usage of an LLM.

### [Click here to get started with Lemonade.](./docs/README.md)

## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](./docs/contribute.md).

## Maintainers

This project is sponsored by AMD. It is maintained by @danielholanda @jeremyfowers @ramkrishna @vgodsoe in equal measure. You can reach us by filing an [issue](https://github.com/lemonade-sdk/lemonade/issues) or email [lemonade@amd.com](mailto:lemonade@amd.com).

## License

This project is licensed under the [Apache 2.0 License](https://github.com/lemonade-sdk/lemonade/blob/main/LICENSE). Portions of the project are licensed as described in [NOTICE.md](./NOTICE.md).

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->

