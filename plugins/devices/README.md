# Devices: The Device Enablement Plugin

The `devices` plugin installs the `benchmark` tool into the `turnkey` CLI, which in turn provides support for running models against target runtimes and hardware.

The tools currently support the following combinations of runtimes and devices:

<span id="devices-runtimes-table">

| Device Type | Device arg | Runtime                                                                               | Runtime arg                      | Specific Devices                              |
| ----------- | ---------- | ------------------------------------------------------------------------------------- | -------------------------------- | --------------------------------------------- |
| Nvidia GPU  | nvidia     | TensorRT<sup>†</sup>                                                                  | trt                              | Any Nvidia GPU supported by TensorRT          |
| x86 CPU     | x86        | ONNX Runtime<sup>‡</sup>, Pytorch Eager, Pytoch 2.x Compiled | ort, torch-eager, torch-compiled | Any Intel or AMD CPU supported by the runtime |

</span>

<sup>†</sup> Requires TensorRT >= 8.5.2  
<sup>‡</sup> Requires ONNX Runtime >= 1.13.1  
<sup>*</sup> Requires Pytorch >= 2.0.0  

## Getting Started

> Note: these steps assume that your current working directory of your terminal is at the repository root.

To install, run:
 
`pip install -e plugins/devices`

To learn more about the tool, run:

`turnkey benchmark -h`

To use the tool, run:

`turnkey -i models/transformers/bert.py discover export-pytorch benchmark --runtime ort --device x86`


# Definitions

This package uses the following definitions throughout.

## Model

A **model** is a PyTorch (torch.nn.Module) instance that has been instantiated in a Python script, or a `.onnx` file.

- Examples: BERT-Base, ResNet-50, etc.

## Device

A **device** is a piece of hardware capable of running a model.

- Examples: Nvidia A100 40GB, Intel Xeon Platinum 8380

## Runtime

A **runtime** is a piece of software that executes a model on a device.

- Different runtimes can produce different performance results on the same device because:
  - Runtimes often optimize the model prior to execution.
  - The runtime is responsible for orchestrating data movement, device invocation, etc.
- Examples: ONNX Runtime, TensorRT, PyTorch Eager Execution, etc.

## BaseRT

BaseRT is an abstract base class (ABC) that defines how our runtimes access and measure hardware.

## Benchmark

*Benchmark* is the process by which `BaseRT.benchmark()` collects performance statistics about a [model](#model). `BaseRT` is an abstract base class that defines the common benchmarking infrastructure that TurnkeyML provides across devices and runtimes.

Specifically, `BaseRT.benchmark()` takes a [build](#build) of a model and executes it on a target device using target runtime software (see [Devices and Runtimes](#devices-and-runtimes)).

By default, `BaseRT.benchmark()` will run the model 100 times to collect the following statistics:
1. Mean Latency, in milliseconds (ms): the average time it takes the runtime/device combination to execute the model/inputs combination once. This includes the time spent invoking the device and transferring the model's inputs and outputs between host memory and the device (when applicable).
1. Throughput, in inferences per second (IPS):  the number of times the model/inputs combination can be executed on the runtime/device combination per second.
    > - _Note_: `BaseRT.benchmark()` is not aware of whether `inputs` is a single input or a batch of inputs. If your `inputs` is actually a batch of inputs, you should multiply `BaseRT.benchmark()`'s reported IPS by the batch size.

# Arguments

The following arguments are used to configure `turnkey` and the APIs to target a specific device and runtime:

### Devices

Specify a device type that will be used for benchmarking.

Usage:
- `benchmark --device TYPE`
  - Benchmark the model(s) in `INPUT_FILES` on a locally installed device of type `TYPE` (eg, a locally installed Nvidia device).

Valid values of `TYPE` include:
- `x86` (default): Intel and AMD x86 CPUs.
- `nvidia`: Nvidia GPUs.

> _Note_: The tools are flexible with respect to which specific devices can be used, as long as they meet the requirements in the [Devices and Runtimes table](#devices-runtimes-table).
>  - The `turnkey` CLI will simply use whatever device, of the given `TYPE`, is available on the machine.
>  - For example, if you specify `--device nvidia` on a machine with an Nvidia A100 40GB installed, then the tools will use that Nvidia A100 40GB device.

### Runtimes

Indicates which software runtime should be used for the benchmark (e.g., ONNX Runtime vs. Torch eager execution for a CPU benchmark).

Usage:
- `benchmark --runtime SW`

Each device type has its own default runtime, as indicated below.
- Valid runtimes for `x86` device
  - `ort`: ONNX Runtime (default).
  - `torch-eager`: PyTorch eager execution.
  - `torch-compiled`: PyTorch 2.x-style compiled graph execution using TorchInductor.
- Valid runtimes for `nvidia` device
  - `trt`: Nvidia TensorRT (default).

### Iterations

Iterations takes an integer that specifies the number of times the model inference should be run during benchmarking. This helps in obtaining a more accurate measurement of the model's performance by averaging the results across multiple runs. Default set to 100 iterations per run. 

Usage:
- `turnkey benchmark INPUT_FILES --iterations 1000`

### Custom Runtime Arguments

Users can pass arbitrary arguments into a runtime, as long as the target runtime supports those arguments, by using the `--rt-args` argument.

None of the built-in runtimes support such arguments, however plugin contributors can use this interface to add arguments to their custom runtimes. See [plugins contribution guideline](https://github.com/onnx/turnkeyml/blob/main/docs/contribute.md#contributing-a-plugin) for details.

# Contributing

To add a runtime to this plugin:

1. Pick a unique name, `<runtime_name>` for each runtime that will be supported by the plugin.
    - This name will be used in the `benchmark --runtime <runtime_name>` [argument](#runtimes)
    - For example, a runtime named `example-rt` would be invoked with `turnkey --runtime example-rt`

1. Populate the [Implements Dictionary](#implements-dictionary) in in [device's `__init__.py`](https://github.com/onnx/turnkeyml/tree/main/plugins/devices/src/turnkeyml_plugin_devices.py) with a new key per-runtime with the following fields:
    - `supported_devices: Union[Set,Dict]`: combination of devices supported by the runtime.
      - For example, in `example_rt`, `"supported_devices": {"x86"}` indicates that the `x86` device is supported by the `example` runtime.
      - A `device` typically refers to an entire family of devices, such as the set of all `x86` CPUs. However, plugins can provide explicit support for specific `device parts` within a device family. Additionally, specific `configurations` within a device model (e.g., a specific device firmware) are also supported.
        - Each supported part within a device family must be defined as a dictionary.
        - Each supported configuration within a device model must be defined as a list.
        - Example: `"supported_devices": {"family1":{"part1":["config1","config2"]}}`.
        - See [example_combined](https://github.com/onnx/turnkeyml/tree/main/examples/turnkey/cli/plugins/example_combined) for a plugin implementation example that leverages this feature.
      - Note: If a device is already supported by the tools, this simply adds support for another runtime to that device. If the device is _not_  already supported by the tools, this also adds support for that device and it will start to appear as an option for the `turnkey --device  <device_name>` argument.
    - `"build_required": Bool`: indicates whether the `build_model()` API should be called on the `model` and `inputs`.
    - `"docker_required": Bool`: indicates whether benchmarking is implemented through a docker container.
      - For example, `"build_required": False` indicates that no build is required, and benchmarking can be performed directly on the `model`   and `inputs`.
      - An example where `"build_required": True` is the `ort` runtime, which requires the `model` to be [built](#build) (via ONNX exporter)  into a `.onnx` file prior to benchmarking.
    - `"RuntimeClass": <class_name>`, where `<class_name>` is a unique name for a Python class that inherits `BaseRT` and implements the runtime.
      - For example, `"RuntimeClass": ExampleRT` implements the `example` runtime.
      - The interface for the runtime class is defined in [Runtime Class](#runtime-class) below.
    - (Optional) `"status_stats": List[str]`: a list of keys from the build stats that should be printed out at the end of benchmarking in the CLI's `Status` output. These keys, and corresponding values, must be set in the runtime class using `self.stats.save_model_eval_stat(key, value)`.
    - (Optional) `"requirement_check": Callable`: a callable that runs before each benchmark. This may be used to check whether the device selected is available and functional before each benchmarking run. Exceptions raised during this callable will halt the benchmark of all selected files.

1. (Optional) Populate the [Implements Dictionary](#implements-dictionary) in in [device's `__init__.py`](https://github.com/onnx/turnkeyml/tree/main/plugins/devices/src/turnkeyml_plugin_devices.py) with any Tools defined by the plugin.

1. Populate the package's source code with the following files (see [Plugin Directory Layout](#plugin-directory-layout)):
    - A `runtime.py` script that implements the [Runtime Class](#runtime-class).
    - (Optional) An `execute` method that follows the [Execute Method](#execute-method) template and implements the benchmarking methodology for the device/runtime combination(s).
      - See the `tensorrt` runtime's `execute.py::main()` for a fairly minimal example.
    - (Optional) A `within_conda.py` script that executes inside the conda env to collect benchmarking results.
      - See the `onnxrt` runtime's `within_conda.py` for an example.

1. Add a key-value pair to `extras_requires` in [device's setup.py](https://github.com/onnx/turnkeyml/tree/main/plugins/devices/setup.py):
    - The `key` is the plugin's name
    - The `value` is any dependencies required by the runtime. If the runtime has no special dependencies, you must still put an empty list (`[]` 

### Implements Dictionary

This dictionary has keys for each type of plugin that will be installed by this package. 
- Packages with runtime plugin(s) should have a `runtimes` key in the `implements` dictionary, which in turn includes one dictionary per runtime installed in the plugin.
- Packages with sequence plugin(s) should have a `sequences` key in the `implements` dictionary, which in turn includes one dictionary per runtime installed in the plugin.

An `implements` dictionary with both sequences and runtimes would have the form:

```python
implements = {
  "runtimes": {
    "runtime_1_name" : {
      "build_required": Bool,
      "RuntimeClass": Class(BaseRT),
      "devices": List[str],
      "default_sequence": Sequence instance,
      "status_stats": ["custom_stat_1", "custom_stat_2"],
    },
    "runtime_2_name" : {...},
    ...
  },
  "tools": [ToolClass1, ToolClass2, etc.]
}
```


### Runtime Class

A runtime class inherits the abstract base class [`BaseRT`](https://github.com/onnx/turnkeyml/tree/main/src/turnkeyml/run/basert.py) and implements a one or more [runtimes](#runtime) to provide benchmarking support for one or more [devices](https://github.com/onnx/turnkeyml/blob/main/docs/turnkey/tools_user_guide.md#devices). 

`BaseRT` has 4 methods that plugin developers must overload: 
- `_setup()`: any code that should be called prior to benchmarking as a one-time setup. Called automatically at the end of  `BaseRT.__init__()`.
- `mean_latency()`: returns the mean latency, in ms, for the benchmarking run.
- `throughput()`: returns the throughput, in IPS, for the benchmarking run.
- `device_name()`: returns the full device name for the device used in benchmarking. For example, a benchmark on a `x86` device might have a device name like `AMD Ryzen 7 PRO 6850U with Radeon Graphics`.
- [Optional] `_execute()`: method that `BaseRT` can automatically call during `BaseRT.benchmark()`, which implements the specific benchmarking methodology for that device/runtime combination. See [Execute Method](#execute-method) for more details.
- [Optional] `__init__()`: the `__init__` method can be overloaded to take additional keyword arguments, see [Custom Runtime Arguments](#custom-runtime-arguments) for details.

Developers may also choose to overload the `benchmark()` function. By default, `BaseRT` will automatically invoke the module's [Execute Method](#execute-method) and use `mean_latency()`, `throughput()`, and `device_name()` to populate a `MeasuredPerformance` instance to return. However, some benchmarking methodologies may not lend themselves to a dedicated execute method. For example, `TorchRT` simply implements all of its benchmarking logic within an overloaded `benchmark()` method. 

### Custom Runtime Arguments

The `turnkey` CLI/APIs allow users to pass arbitrary arguments to the runtime with `--rt-args`.

Runtime arguments from the user's `--rt-args` will be passed into the runtime class's `__init__()` method as keyword arguments. Runtime plugins must accept any such custom arguments in their overloaded `__init__()` method, at which point the contributor is free to use them any way they like. A common usage would be to store arguments as members of `self` and then access them during `_setup()` or `_execute()`.

The format for runtime arguments passed through the CLI is:

```
--rt-args arg1::value1 arg2::[value2,value3] flag_arg
```

Where:
- Arguments are space-delimited.
- Flag arguments (in the style of `argparse`'s `store_true`) are passed by key name only and result in `<key>=True`.
- Arguments with a single value are passed as `key::value`.
- Arguments that are a list of values are passed as `key::[value1, value2, ...]`.

### Execute Method

Contributors who are not overloading `BaseRT.benchmark()` must overload `BaseRT._execute()`. By default, `BaseRT` will automatically call `self._execute()` during `BaseRT.benchmark()`, which implements the specific benchmarking methodology for that device/runtime combination. For example, `tensorrt/runtime.py::_execute_()` implements benchmarking on Nvidia GPU devices with the TensorRT runtime.

Implementation of the execute method is optional, however if you do not implement the execute method you will have to overload `BaseRT.benchmark()` with your own functionality as in `TorchRT`.

`_execute()` must implement the following arguments (note that it is not required to make use of all of them):
- `output_dir`: path where the benchmarking artifacts (ONNX files, inputs, outputs, performance data, etc.) are located.
- `onnx_file`: path where the ONNX file for the model is located.
- `outputs_file`: path where the benchmarking outputs will be located.
- `iterations`: number of execution iterations of the model to capture the throughput and mean latency.

Additionally, `self._execute()` can access any custom runtime argument that has been added to `self` by the runtime class.