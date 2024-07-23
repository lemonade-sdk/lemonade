# TurnkeyML Code Structure

# Repo Organization

The TurnkeyML source code has a few major top-level directories:
- `docs`: documentation for the entire project.
- `examples`: example scripts for use with the TurnkeyML tools.
  - `examples/cli`: tutorial series starting in `examples/cli/readme.md` to help learn the `turnkey` CLI.
    - `examples/cli/scripts`: example scripts that can be fed as input into the `turnkey` CLI. These scripts each have a docstring that recommends one or more `turnkey` CLI commands to try out.
  - `examples/api`: examples scripts that invoke `Tools` via APIs.
- `models`: the corpora of models that makes up the TurnkeyML models (see [the models readme](https://github.com/onnx/turnkeyml/blob/main/models/readme.md)).
  - Each subdirectory under `models` represents a corpus of models pulled from somewhere on the internet. For example, `models/torch_hub` is a corpus of models from [Torch Hub](https://github.com/pytorch/hub).
- `src/turnkeyml`: source code for the TurnkeyML package.
  - `src/turnkeyml/tools`: implements `Tool` and defines the tools built in to `turnkey`.
  - `src/turnkeyml/sequence`: implements `Sequence` and defines the plugin API for `Tool`s.
  - `src/turnkeyml/run`: implements `BaseRT`, an abstract base class that defines TurnkeyML's vendor-agnostic benchmarking functionality. This module also includes the runtime and device plugin APIs and the built-in runtimes and devices.
  - `src/turnkeyml/cli`: implements the `turnkey` CLI.
  - `src/turnkeyml/common`: functions common to the other modules.
  - `src/turnkeyml/version.py`: defines the package version number.
  - `src/turnkeyml/state.py`: implements the `State` class.
  - `src/turnkeyml/files_api.py`: implements the `evaluate_files()` API, which is the top-level API called by the CLI.
- `test`: tests for the TurnkeyML tools.
  - `test/analysis.py`: tests focusing on the `discover` `Tool`.
  - `test/cli.py`: tests focusing on top-level CLI features.

## Tool Classes

All of the logic for actually building models is contained in `Tool` classes. Generally, a `FirstTool` class obtains a model, and each subsequent `Tool` is a model-to-model transformation. For example:
- the `Discover(FirstTool)` (aka `discover` in the CLI) obtains a PyTorch model instance from a python script.
- the `ExportPytorchModel(Tool)` (aka `export-pytorch` in the CLI) transforms a PyTorch model instance into an ONNX model file.

### Composability

`Tools` are designed to be composable, for example, there are already a few ONNX-to-ONNX `Tools` defined in `src/turnkeyml/tools/onnx.py` that could sequenced in any order.

This composability is facilitated by the `State` class, which is how `Tools` communicate with each other. Every `Tool` takes an instance of `State` as input and then returns an instance of `State`. For example:
- `Discover(FirstTool)` takes a freshly initialized instance of `State` as input, and modifies it so that `state.result` points to a PyTorch model.
- `ExportPytorchModel(Tool)` takes a PyTorch model in `state.result` then modifies `State` such that `state.result` points to the exported ONNX file.

### Implementation

See [tools.py](https://github.com/onnx/turnkeyml/blob/main/src/turnkeyml/tools/tool.py) for a definition of each method of `Tool` that must be implemented to create a new `Tool` subclass.
