# TurnkeyML Contribution Guide

Hello and welcome to the project! 🎉

We're thrilled that you're considering contributing to the project. This project is a collaborative effort and we welcome contributors from everyone.

Before you start, please take a few moments to read through these guidelines. They are designed to make the contribution process easy and effective for everyone involved. Also take a look at the [code organization](https://github.com/onnx/turnkeyml/blob/main/docs/code.md) for a bird's eye view of the repository.

The guidelines document is organized as the following sections:
- [Contributing a model](#contributing-a-model)
- [Contributing a plugin](#contributing-a-plugin)
- [Contributing to the overall framework](#contributing-to-the-overall-framework)
- [Issues](#issues)
- [Pull Requests](#pull-requests)
- [Testing](#testing)
- [Versioning](#versioning)
- [PyPI Release Process](#pypi-release-process)
- [Public APIs](#public-apis)


## Contributing a model

One of the easiest ways to contribute is to add a model to the benchmark. To do so, simply add a `.py` file to the `models/` directory that instantiates and calls a PyTorch model. The automation in `discover` will make the PyTorch model available to the rest of the `Tools`!

Please see [`bert.py`](https://github.com/onnx/turnkeyml/blob/main/models/transformers/bert.py) for an example of a model contribution.

## Contributing a plugin

TurnkeyML supports a variety of built-in tools. You can contribute a plugin to add support for tools that do virtually anything: acquire, export, optimize, quantize, or execute ONNX models.

A turnkey plugin is a pip-installable package that implements support for a `Tool`. These packages must adhere to a specific interface that is documented below. 

### Naming Scheme

Plugins should be named in a way that makes it easy to refer to them in a sentence. 

General rules:

- The word "onnx" should be assumed wherever possible since this is an ONNX toolchain
- Name the tool as a verb that references the action it is taking
- Avoid using prepositions like "to"

Examples:

- Anything that loads anything starts with `load-`
  - `load-build`
  - `load-onnx` (formerly `onnx-load`)
  - `load-llm-checkpoint`
- Anything that exports to ONNX should start with `export-SOURCE` since the "onnx" part is assumed
  - `export-pytorch` 
- ONNX-to-ONNX transformations should have the form `ACTION[-RESULT]` where `RESULT` optionally adds necessary detail to the action
  - `optimize-ort` (formerly `optimize-onnx`), where the action is short for "optimize-with-onnxruntime"
  - `convert-fp16`
  - `quantize-int8`

This allows for sequences-as-sentences like the following (with prepositions assumed):

> `discover` then `export-[to-]pytorch[-to-onnx]` then `optimize-[with-]ort` then `quantize-[to-]int8`

### Plugin Directory Layout

Plugin packages should have this directory layout:

```
`<descriptive_name>`/
        |- setup.py
        |- README.md
        |- turnkeyml_plugin_<descriptive_name>/
                |- __init__.py
                |- tool.py
                    
```
### Package Template

Plugins are pip-installable packages, so they each take the form of a directory that contains a `setup.py` script and a Python module containing the plugin source code.

We require the following naming scheme:

- The top level directory can be named any `<descriptive_name>` you like.
  - For example, `example_tool/`
- The package name is `turnkeyml_plugin_<descriptive_name>`
  - For example, `turnkeyml_plugin_example_tool`
  - Note: this naming convention is used by the tools to detect installed plugins. If you do not follow the convention your plugin will not be detected.
- Within the module, a `turnkeyml_plugin_<descriptive_name>/__init__.py` file that has an `implements` nested dictionary (see [Example](#example)).
  - Note: a single plugin can implement any number of `tools`.
- Source code files that implement the plugin capabilities (see [Plugin Directory Layout](#plugin-directory-layout)).


### Example

See the [example_tool](https://github.com/onnx/turnkeyml/tree/main/examples/turnkey/cli/plugins/example_tool) plugin for an example.

The `__init__.py` file with its `implements` dictionary looks like:

```
from .tool import ExamplePluginTool

implements = {"tools": [ExamplePluginTool]}
```

## Contributing to the overall framework
If you wish to contribute to any other part of the repository such as examples or reporting, please open an [issue](#issues) with the following details.

1. **Title:** A concise, descriptive title that summarizes the contribution.
1. **Tags/Labels:** Add any relevant tags or labels such as 'enhancement', 'good first issue', or 'help wanted'
1. **Proposal:** Detailed description of what you propose to contribute. For new examples, describe what they will demonstrate, the technology or tools they'll use, and how they'll be structured.

## Issues

Please file any bugs or feature requests you have as an [Issue](https://github.com/onnx/turnkeyml/issues) and we will take a look.

## Pull Requests

Contribute code by creating a pull request (PR). Your PR will be reviewed by one of the [repo maintainers](https://github.com/onnx/turnkeyml/blob/main/CODEOWNERS).

Please have a discussion with the team before making major changes. The best way to start such a discussion is to file an [Issue](https://github.com/onnx/turnkeyml/issues) and seek a response from one of the [repo maintainers](https://github.com/onnx/turnkeyml/blob/main/CODEOWNERS).

## Testing

Tests are defined in `tests/` and run automatically on each PR, as defined in our [testing action](https://github.com/onnx/turnkeyml/blob/main/.github/workflows/test.yml). This action performs both linting and unit testing and must succeed before code can be merged.

We don't have any fancy testing framework set up yet. If you want to run tests locally:
- Activate a `conda` environment that has `turnkey` (this package) installed.
- Run `conda install pylint` if you haven't already (other pylint installs will give you a lot of import warnings).
- Run `pylint src --rcfile .pylintrc` from the repo root.
- Run `python *.py` for each test script in `test/`.

## Versioning

We use semantic versioning, as described in [versioning.md](https://github.com/onnx/turnkeyml/blob/main/docs/versioning.md).

## PyPI Release Process

TurnkeyML is provided as a package on PyPI, the Python Package Index, as [turnkeyml](https://pypi.org/project/turnkeyml/). The release process for pushing an updated package to PyPI is mostly automated, however (by design), there are a few manual steps.
1. Make sure the version number in [version.py](https://github.com/onnx/turnkeyml/blob/main/src/turnkeyml/version.py) has a higher value than the current [PyPI package](https://pypi.org/project/turnkeyml/).
    - Note: if you don't take care of this, PyPI will reject the updated package and you will need to start over from Step 1 of this guide.
    - If you are unsure what to set the version number to, consult [versioning.md](https://github.com/onnx/turnkeyml/blob/main/docs/versioning.md).
1. Make sure all of the changes you want to release have been merged to `main`.
1. Go to the [TurnkeyML GitHub front page](https://github.com/onnx/turnkeyml) and click "Releases" in the side bar.
1. At the top of the page, click "Draft a new release".
1. Click "Choose a tag" (near the top of the page) and write `v` (lowercase), followed by the contents of the string in [version.py](https://github.com/onnx/turnkeyml/blob/main/src/turnkeyml/version.py).
  - For example, if `version.py` contains `__version__ = "4.0.5"`, the string is `4.0.5` and you should write `v4.0.5` into the text box.
1. Click the "+Create new tag:... on publish" button that appears under the next box.
1. Click "Generate release notes" (near the top of the page). Modify as necessary. Make sure to give credit where credit is due!
1. Click "Publish release" (green button near the bottom of the page). This will start the release automations, in the form of a [Publish Distributions to PyPI Action](https://github.com/onnx/turnkeyml/actions/workflows/publish-to-test-pypi.yml).
  - Note: if you forgot the "v" in the "Choose a tag" step, this Action will not run.
1. Wait for the Action launched by the prior step to complete. Go to [the turnkeyml PyPI page](https://pypi.org/project/turnkeyml/) and spam refresh. You should see the version number update.
  - Note: `pip install turnkeyml` may not find the new update for a few more minutes.

## Public APIs

The following public APIs are available for developers. The maintainers aspire to change these as infrequently as possible, and doing so will require an update to the package's major version number.

- From the top-level `__init__.py`:
    - `turnkeycli`: the `main()` function of the `turnkey` CLI
    - `evaluate_files()`: the top-level API called by the CLI
    - `turnkeyml.version`: The package version number
- From the `common.filesystem` module:
    - `get_available_builds()`: list the builds in a turnkey cache
    - `make_cache_dir()`: create a turnkey cache
    - `MODELS_DIR`: the location of turnkey's model corpus on disk
    - `Stats`: handle for saving and reading evaluation statistics
    - `Keys`: reserves keys in the evaluation statistics
- From the `common.printing` module:
    - `log_info()`: print an info statement in the style of the turnkey APIs/CLIs 
    - `log_warning()`: print a warning statement in the style of the turnkey APIs/CLIs
    - `log_error()`: print an error statement in the style of the turnkey APIs/CLIs
 - From the `common.onnx_helpers` module:
    - `onnx_dir()`: location on disk of a build's ONNX files
 - From the `tools` module:
    - The `Tool` and `FirstTool` classes for defining new tools
    - `export.ExportPytorchModel(Tool)`: Tool for exporting models to ONNX
    - `onnx.OptimizeOnnxModel(Tool)`: Tool for using ONNX Runtime to optimize an ONNX model
    - `onnx.ConvertOnnxToFp16(Tool)`: Tool for using ONNX ML Tools to downcast an ONNX model to fp16
    - `onnx.LoadOnnx(FirstTool)`: Tool for loading an ONNX model into the sequence from disk.
    - `discovery.Discover(FirstTool)`: Tool for getting a PyTorch model from a Python script.
 - From the `sequence` module:
    - The `Sequence` class: ordered collection of `Tool`s
 - From the `state` module:
    - The `State` class: data structure that holds the inputs, outputs, and intermediate values for a Sequence 
    - `load_state()`: API for loading the `State` of a previous build
 - From the `common.exceptions` module:
    - `StageError`: exception raised when something goes wrong during a Stage
    - `ModelRuntimeError`: exception raised when something goes wrong running a model in hardware
