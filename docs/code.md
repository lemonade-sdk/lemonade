# Lemonade SDK Code Structure

# Repo Organization

The Lemonade SDK source code has a few major top-level directories:
- `docs`: documentation for the entire project.
- `examples`: example scripts for use with the Lemonade tools.
- `src/lemonade`: source code for the lemonade-sdk package.
  - `src/lemonade/tools`: implements `Tool` and defines the tools built in to `lemonade`.
  - `src/lemonade/sequence.py`: implements `Sequence` and defines the plugin API for `Tool`s.
  - `src/lemonade/cli`: implements the `lemonade` CLI.
  - `src/lemonade/common`: functions common to the other modules.
  - `src/lemonade/version.py`: defines the package version number.
  - `src/lemonade/state.py`: implements the `State` class.
- `test`: tests for the Lemonade SDK tools.

## Tool Classes

All of the logic for actually building models is contained in `Tool` classes. Generally, a `FirstTool` class obtains a model, and each subsequent `Tool` is a model-to-model transformation. For example:
- the `Discover(FirstTool)` (aka `discover` in the CLI) obtains a PyTorch model instance from a python script.
- the `ExportPytorchModel(Tool)` (aka `export-pytorch` in the CLI) transforms a PyTorch model instance into an ONNX model file.

### Composability

`Tools` are designed to be composable. This composability is facilitated by the `State` class, which is how `Tools` communicate with each other. Every `Tool` takes an instance of `State` as input and then returns an instance of `State`.

### Implementation

See [tools.py](https://github.com/lemonade-sdk/lemonade/blob/main/src/lemonade/tools/tool.py) for a definition of each method of `Tool` that must be implemented to create a new `Tool` subclass.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->