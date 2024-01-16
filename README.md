# Welcome to ONNX TurnkeyML

[![Turnkey tests](https://github.com/onnx/turnkeyml/actions/workflows/test_turnkey.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![Build API tests](https://github.com/onnx/turnkeyml/actions/workflows/test_build_api.yml/badge.svg)](https://github.com/onnx/turnkeyml/tree/main/test "Check out our tests")
[![OS - Windows | Linux](https://img.shields.io/badge/OS-windows%20%7C%20linux-blue)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")
[![Made with Python](https://img.shields.io/badge/Python-3.8,3.10-blue?logo=python&logoColor=white)](https://github.com/onnx/turnkeyml/blob/main/docs/install.md "Check out our instructions")


We are on a mission to understand and use as many models as possible while leveraging the right toolchain and AI hardware for the job in every scenario. 

Evaluating a deep learning model with a familiar toolchain and hardware accelerator is pretty straightforward. Scaling these evaluations to get apples-to-apples insights across a landscape of millions of permutations of models, toolchains, and hardware targets is not straightforward. Not without help, anyways.

TurnkeyML is a *tools framework* that integrates models, toolchains, and hardware backends to make evaluation and actuation of this landscape as simple as turning a key.

## Get started

For most users its as simple as:

```
pip install turnkeyml
turnkey my_model.py
```

The [installation guide](https://github.com/onnx/turnkeyml/blob/main/docs/install.md), [tutorials](https://github.com/onnx/turnkeyml/tree/main/examples/cli), and [user guide](https://github.com/onnx/turnkeyml/blob/main/docs/tools_user_guide.md) have everything you need to know.

## Use Cases

TurnkeyML is designed to support the following use cases. Of course, it is also quite flexible, so we are sure you will come up with some use cases of your own too.

| Use Case               | Description | Recipe |
|------------------------|-------------|---------|
| ONNX Model Zoo         | Export thousands of ONNX files across different ONNX opsets. This is how we generated the contents of the new [ONNX Model Zoo](https://github.com/onnx/models). | `turnkey */*.py -b --onnx-opset 16` <br /> `turnkey */*.py -b --onnx-opset 17` |
| Performance validation | Measure latency and throughput in hardware across devices and runtimes to understand product-market fit. | `turnkey model.py --runtime ort` <br /> `turnkey model.py --runtime torch-eager` <br />`turnkey cache report` |
| Functional coverage    | Measure the functional coverage of toolchain/hardware combinations over a large corpus of models (e.g., how many models are supported by a novel compiler?). | `turnkey transformers/*.py --sequence MY_COMPILER` <br />`turnkey cache report` |
| Stress testing         | Run millions of inferences across thousands of models and log all the results to find the bugs in a HW/SW stack. | `turnkey timm/*.py --iterations 1000 --device MY_DEVICE --runtime MY_RUNTIME` |
| Model insights         | Analyze a model to learn its parameter count, input shapes, which ONNX ops it uses, etc. | `turnkey model.py` <br /> `turnkey cache stats MY_BUILD`|



## Demo

Let's say you have a Python script that includes a PyTorch model. Maybe you downloaded the model from Huggingface, grabbed it from our corpus, or wrote it yourself. Doesn't matter, just call `turnkey` and get to work.   

The `turnkey` CLI will analyze your script, find the model(s), run an ONNX toolchain on the model, and execute the resulting ONNX file in CPU hardware:

```
> turnkey bert.py
```

```
Models discovered during profiling:

bert.py:
        model (executed 1x)
                Model Type:     Pytorch (torch.nn.Module)
                Class:          BertModel (<class 'transformers.models.bert.modeling_bert.BertModel'>)
                Location:       /home/jfowers/turnkeyml/models/transformers/bert.py, line 23
                Parameters:     109,482,240 (417.64 MB)
                Input Shape:    'attention_mask': (1, 128), 'input_ids': (1, 128)
                Hash:           bf722986
                Build dir:      /home/jfowers/.cache/turnkey/bert_bf722986
                Status:         Successfully benchmarked on AMD Ryzen 9 7940HS w/ Radeon 780M Graphics (ort v1.15.1) 
                                Mean Latency:   44.168  milliseconds (ms)
                                Throughput:     22.6    inferences per second (IPS)
```

Let's say you want a fp16 ONNX file of the same model: incorporate the ONNX ML Tools fp16 converter tool into the build sequence, and the `Build dir` will contain the ONNX file you seek:

```
> turnkey bert.py --sequence optimize-fp16 --build-only
```

```
bert.py:
        model (executed 1x)
                ...
                Build dir:      /home/jfowers/.cache/turnkey/bert_bf722986
                Status:         Model successfully built!
```

```
> ls /home/jfowers/.cache/turnkey/bert_bf722986/onnx

bert_bf722986-op14-base.onnx  bert_bf722986-op14-opt-f16.onnx  bert_bf722986-op14-opt.onnx
```

Now you want to see the fp16 model running on your Nvidia GPU with the Nvidia TensorRT runtime:

```
> turnkey bert.py --sequence export optimize-fp16 --device nvidia --runtime tensorrt
```

```
bert.py:
        model (executed 1x)
                ...
                Status:         Successfully benchmarked on NVIDIA GeForce RTX 4070 Laptop GPU (trt v23.09-py3) 
                                Mean Latency:   2.573   milliseconds (ms)
                                Throughput:     377.8   inferences per second (IPS)
```

Mad with power, you want to see dozens of fp16 Transformers running on your Nvidia GPU:

```
> turnkey REPO_ROOT/models/transformers/*.py --sequence optimize-fp16 --device nvidia --runtime tensorrt
```

```
Models discovered during profiling:

albert.py:
        model (executed 1x)
                Class:          AlbertModel (<class 'transformers.models.albert.modeling_albert.AlbertModel'>)
                Parameters:     11,683,584 (44.57 MB)
                Status:         Successfully benchmarked on NVIDIA GeForce RTX 4070 Laptop GPU (trt v23.09-py3) 
                                Mean Latency:   1.143   milliseconds (ms)
                                Throughput:     828.3   inferences per second (IPS)

bart.py:
        model (executed 1x)
                Class:          BartModel (<class 'transformers.models.bart.modeling_bart.BartModel'>)
                Parameters:     139,420,416 (531.85 MB)
                Status:         Successfully benchmarked on NVIDIA GeForce RTX 4070 Laptop GPU (trt v23.09-py3) 
                                Mean Latency:   2.343   milliseconds (ms)
                                Throughput:     414.5   inferences per second (IPS)

bert.py:
        model (executed 1x)
                Class:          BertModel (<class 'transformers.models.bert.modeling_bert.BertModel'>)
                Parameters:     109,482,240 (417.64 MB)
                Status:         Successfully benchmarked on NVIDIA GeForce RTX 4070 Laptop GPU (trt v23.09-py3) 
                                Mean Latency:   2.565   milliseconds (ms)
                                Throughput:     378.0   inferences per second (IPS)

...
```

Finally, you want to visualize the results in one place so that your boss can see how productive you've been. This command will collect all of the statistics across all prior commands into a single spreadsheet.

```
> turnkey cache report

Summary spreadsheet saved at /home/jfowers/2023-11-30.csv
```

You're probably starting to get the idea :rocket:

There's a lot more features you can learn about in the [tutorials](https://github.com/onnx/turnkeyml/tree/main/examples/cli) and [user guide](https://github.com/onnx/turnkeyml/blob/main/docs/tools_user_guide.md).

## What's Inside

The TurnkeyML framework has 5 core components:
- **Analysis tool**: Inspect Python scripts to find the PyTorch models within. Discover insights and pass the models to the other tools.
- **Build tool**: Prepare your model using industry-standard AI tools (e.g., exporters, optimizers, quantizers, and compilers). Any model-to-model transformation is fair game.
- **Runtime tool**: Invoke AI runtimes (e.g., ONNX Runtime, TensorRT, etc.) to execute models in hardware and measure key performance indicators.
- **Reporting tool**: Visualize statistics about the models, builds, and invocations.  
- **Models corpus**: Hundreds of popular PyTorch models that are ready for use with `turnkey`.

All of this is seamlessly integrated together such that a command like `turnkey repo/models/corpus/script.py` gets you all of the functionality in one shot. Or you can access functionality piecemeal with commands and APIs like `turnkey analyze script.py` or `build_model(my_model_instance)`. The [tutorials](https://github.com/onnx/turnkeyml/tree/main/examples/cli) show off the individual features.

You can read more about the code organization [here](https://github.com/onnx/turnkeyml/blob/main/docs/code.md).

## Extensibility

### Models

[![transformers](https://img.shields.io/github/directory-file-count/onnx/turnkeyml/models/transformers?label=transformers)](https://github.com/onnx/turnkeyml/tree/main/models/transformers "Transformer models")
[![graph_convolutions](https://img.shields.io/github/directory-file-count/onnx/turnkeyml/models/graph_convolutions?label=graph_convolutions)](https://github.com/onnx/turnkeyml/tree/main/models/graph_convolutions "Graph Convolution models")
[![torch_hub](https://img.shields.io/github/directory-file-count/onnx/turnkeyml/models/torch_hub?label=torch_hub)](https://github.com/onnx/turnkeyml/tree/main/models/torch_hub "Models from Torch Hub")
[![torchvision](https://img.shields.io/github/directory-file-count/onnx/turnkeyml/models/torchvision?label=torchvision)](https://github.com/onnx/turnkeyml/tree/main/models/torchvision "Models from Torch Vision")
[![timm](https://img.shields.io/github/directory-file-count/onnx/turnkeyml/models/timm?label=timm)](https://github.com/onnx/turnkeyml/tree/main/models/timm "Pytorch Image Models")

This repository is home to a diverse corpus of hundreds of models. We are actively working on increasing the number of models in our model library. You can see the set of models in each category by clicking on the corresponding badge.

Evaluating a new model is as simple as taking a Python script that instantiates and invokes a PyTorch `torch.nn.module` and call `turnkey` on it. Read about model contributions [here](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md#contributing-a-model).

### Plugins

The build tool has built-in support for a variety of export and optimization tools (e.g., the PyTorch-to-ONNX exporter, ONNX ML Tools fp16 converter, etc.). Likewise, the runtime tool comes out-of-box with support for x86 and Nvidia devices, along with ONNX Runtime, TensorRT, torch-eager, and torch-compiled runtimes. 

If you need more, the TurnkeyML plugin API lets you extend the build and runtime tools with any functionality you like:

```
> pip install -e my_custom_plugin
> turnkey my_model.py --sequence my-custom-sequence --device my-custom-device --runtime my-custom-runtime --rt-args my-custom-args
```

All of the built-in sequences, runtimes, and devices are implemented against the plugin API. Check out the [example plugins](https://github.com/onnx/turnkeyml/tree/main/examples/cli/plugins) and the [plugin API guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md#contributing-a-plugin).

## Contributing

We are actively seeking collaborators from across the industry. If you would like to contribute to this project, please check out our [contribution guide](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md).

## Maintainers

This project is sponsored by the [ONNX Model Zoo](https://github.com/onnx/models) special interest group (SIG). It is maintained by @danielholanda @jeremyfowers @ramkrishna @vgodsoe in equal measure. You can reach us by filing an [issue](https://github.com/onnx/turnkeyml/issues).

## License

This project is licensed under the [Apache 2.0 License](https://github.com/onnx/turnkeyml/blob/main/LICENSE).

## Attribution

TurnkeyML used code from other open source projects as a starting point (see [NOTICE.md](NOTICE.md)). Thank you Philip Colangelo, Derek Elkins, Jeremy Fowers, Dan Gard, Victoria Godsoe, Mark Heaps, Daniel Holanda, Brian Kurtz, Mariah Larwood, Philip Lassen, Andrew Ling, Adrian Macias, Gary Malik, Sarah Massengill, Ashwin Murthy, Hatice Ozen, Tim Sears, Sean Settle, Krishna Sivakumar, Aviv Weinstein, Xueli Xao, Bill Xing, and Lev Zlotnik for your contributions to that work.
