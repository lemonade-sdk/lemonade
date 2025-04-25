# Welcome to ONNX TurnkeyML

[![Lemonade tests](https://github.com/onnx/turnkeyml/actions/workflows/test_lemonade.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![Turnkey tests](https://github.com/onnx/turnkeyml/actions/workflows/test_turnkey.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![OS - Windows | Linux](https://img.shields.io/badge/OS-windows%20%7C%20linux-blue)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")
[![Made with Python](https://img.shields.io/badge/Python-3.8,3.10-blue?logo=python&logoColor=white)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")

We are on a mission to make it easy to use the most important tools in the ONNX ecosystem. TurnkeyML accomplishes this by providing a full SDK for LLMs with the Lemonade SDK, as well as a no-code CLIs for general ONNX workflows with `turnkey`.

## 🍋 Lemonade SDK: Quickly serve, benchmark and deploy LLMs

The [Lemonade SDK](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md) is designed to make it easy to serve, benchmark, and deploy large language models (LLMs) on a variety of hardware platforms, including CPU, GPU, and NPU. 

<div align="center">
  <img src="https://download.amd.com/images/lemonade_640x480_1.gif" alt="Lemonade Demo" title="Lemonade in Action">
</div>

The [Lemonade SDK](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md) is comprised of the following:

- 🌐 **Lemonade Server**: A server interface that uses the standard Open AI API, allowing applications to integrate with local LLMs.
- 🐍 **Lemonade Python API**: Offers High-Level API for easy integration of Lemonade LLMs into Python applications and Low-Level API for custom experiments.
- 🖥️ **Lemonade CLI**: The `lemonade` CLI lets you mix-and-match LLMs, frameworks (PyTorch, ONNX, GGUF), and measurement tools to run experiments. The available tools are:
  - Prompting an LLM.
  - Measuring the accuracy of an LLM using a variety of tests.
  - Benchmarking an LLM to get the time-to-first-token and tokens per second.
  - Profiling the memory usage of an LLM.

### [Click here to get started with Lemonade.](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/README.md)

## 🔑 Turnkey: A Complementary Tool for ONNX Workflows

While Lemonade focuses on LLMs, [Turnkey](https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/README.md) is a no-code CLI designed for general ONNX workflows, such as exporting and optimizing CNNs and Transformers.

To see the list of supported tools, using the following command:

```bash
turnkey -h
```

<div align="center">
  <img src="https://download.amd.com/images/tkml_640x480_1.gif" alt="Turnkey Demo" title="Turnkey CLI">
</div>

### [Click here to get started with `turnkey`.](https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/README.md)

## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md).

## Maintainers

This project is sponsored by the [ONNX Model Zoo](https://github.com/onnx/models) special interest group (SIG). It is maintained by @danielholanda @jeremyfowers @ramkrishna @vgodsoe in equal measure. You can reach us by filing an [issue](https://github.com/onnx/turnkeyml/issues) or emailing `turnkeyml at amd dot com`.

## License

This project is licensed under the [Apache 2.0 License](https://github.com/onnx/turnkeyml/blob/main/LICENSE).

## Attribution

TurnkeyML used code from other open source projects as a starting point (see [NOTICE.md](NOTICE.md)). Thank you Philip Colangelo, Derek Elkins, Jeremy Fowers, Dan Gard, Victoria Godsoe, Mark Heaps, Daniel Holanda, Brian Kurtz, Mariah Larwood, Philip Lassen, Andrew Ling, Adrian Macias, Gary Malik, Sarah Massengill, Ashwin Murthy, Hatice Ozen, Tim Sears, Sean Settle, Krishna Sivakumar, Aviv Weinstein, Xueli Xao, Bill Xing, and Lev Zlotnik for your contributions to that work.

