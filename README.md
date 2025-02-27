# Welcome to ONNX TurnkeyML

[![Turnkey tests](https://github.com/onnx/turnkeyml/actions/workflows/test_turnkey.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![Lemonade tests](https://github.com/onnx/turnkeyml/actions/workflows/test_lemonade.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![OS - Windows | Linux](https://img.shields.io/badge/OS-windows%20%7C%20linux-blue)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")
[![Made with Python](https://img.shields.io/badge/Python-3.8,3.10-blue?logo=python&logoColor=white)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")

We are on a mission to make it easy to use the most important tools in the ONNX ecosystem. TurnkeyML accomplishes this by providing no-code CLIs and low-code APIs for both general ONNX workflows with `turnkey` as well as LLMs with `lemonade`.

|                     [**Lemonade SDK**](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/getting_started.md)                    	|                            [**Turnkey**](https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/getting_started.md)                                	|
|:----------------------------------------------:	|:-----------------------------------------------------------------:	|
| Serve and benchmark LLMs on CPU, GPU, and NPU. <br/>	[Click here to get started with `lemonade`.](https://github.com/onnx/turnkeyml/blob/main/docs/lemonade/getting_started.md) | Export and optimize ONNX models for CNNs and Transformers. <br/>	[Click here to get started with `turnkey`.](https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/getting_started.md)	|
| <img src="https://github.com/onnx/turnkeyml/blob/main/img/llm_demo.png?raw=true"/> | <img src="https://github.com/onnx/turnkeyml/blob/main/img/classic_demo.png?raw=true"/> |


## How It Works

The `turnkey` (CNNs and transformers) and `lemonade` (LLMs) CLIs provide a set of `Tools` that users can invoke in a `Sequence`. The first `Tool` takes the input (`-i`), performs some action, and passes its state to the next `Tool` in the `Sequence`.

You can read the `Sequence` out like a sentence. For example, the demo command above was:

```
> turnkey -i bert.py discover export-pytorch optimize-ort convert-fp16
```

Which you can read like:

> Use `turnkey` on `bert.py` to `discover` the model, `export` the `pytorch` to ONNX, `optimize` the ONNX with `ort`, and `convert` the ONNX to `fp16`.

You can configure each `Tool` by passing it arguments. For example, `export-pytorch --opset 18` would set the opset of the resulting ONNX model to 18.

A full command with an argument looks like:

```
> turnkey -i bert.py discover export-pytorch --opset 18 optimize-ort convert-fp16
```


## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md).

## Maintainers

This project is sponsored by the [ONNX Model Zoo](https://github.com/onnx/models) special interest group (SIG). It is maintained by @danielholanda @jeremyfowers @ramkrishna @vgodsoe in equal measure. You can reach us by filing an [issue](https://github.com/onnx/turnkeyml/issues) or emailing `turnkeyml at amd dot com`.

## License

This project is licensed under the [Apache 2.0 License](https://github.com/onnx/turnkeyml/blob/main/LICENSE).

## Attribution

TurnkeyML used code from other open source projects as a starting point (see [NOTICE.md](NOTICE.md)). Thank you Philip Colangelo, Derek Elkins, Jeremy Fowers, Dan Gard, Victoria Godsoe, Mark Heaps, Daniel Holanda, Brian Kurtz, Mariah Larwood, Philip Lassen, Andrew Ling, Adrian Macias, Gary Malik, Sarah Massengill, Ashwin Murthy, Hatice Ozen, Tim Sears, Sean Settle, Krishna Sivakumar, Aviv Weinstein, Xueli Xao, Bill Xing, and Lev Zlotnik for your contributions to that work.

