# Tools User Guide

The TurnkeyML package provides a CLI, `turnkey`, and Python API for evaluating models. This document reviews the functionality provided by the package. If you are looking for repo and code organization, you can find that [here](https://github.com/onnx/turnkeyml/blob/main/docs/code.md).

# Table of Contents
- [Learning the Tools](#learning-the-tools)
- [Running the Tools](#running-the-tools)
- [Important Concepts](#concepts)
  - [Model Discovery](#model-discovery)
  - [Providing Input Files](#providing-input-files)
  - [File Labels](#file-labels)
  - [Cache Directory](#cache-directory)
  - [Process Isolation](#process-isolation)
  - [Use Slurm](#use-slurm)
- [Environment Variables](#environment-variables)


## Learning the Tools

Use `turnkey -h` to display the help page for the CLI. This page has two main sections, the `tool` positional arguments and the optional arguments.

### Tool Positional Arguments

Most tools are called in a sequence, where the order of the tools in the sequence determines the order they run. State is passed from one tool to the next. 

There is a special class of tools that can start a sequence (i.e., **first tools**), and after that any tool can run in any order as long as it is compatible with the incoming state from the previous tool.

There is another special kind of tool: **management tools**. These provide some management function, such as printing the package version number, and are not intended to run in a sequence with any other tool.

You can learn more about a specific tool by calling `turnkey -h` to get the names of the tools, then `turnkey <tool name> -h`.

Every call to `turnkey` must invoke at least one tool.

### Optional Arguments

These are global arguments that apply to all `Tools` that are invoked by `turnkey`.

The most important of these is `--inputs-files, -i`, which provides the input to the first tool invoked. Input files is designed as a list that can take multiple file paths, or wildcards that refer to multiple files. `turnkey` will send each input through the sequence individually.

## Running the Tools

You can specify and run a sequence of tools with the following syntax:

```
turnkey -i some_input.py some_other_input.py --global-arg0 value0 tool-one --arg1 value1 tool-two --arg2 value2 ...
```

Let's break that down:
1. `turnkey -i some_input.py` will start all sequences. 
    - Multiple input files can be accepted, so `some_other_input.py` provides a 
  second input file
1. `--global-arg0 value0` provides an optional global argument that applies to all tools in the sequence.
    - Global arguments are always specified before the first tool in the sequence.
    - For example, `--cache-dir ~/my_turnkey_cache` would inform all `Tools` that their results should be placed under `~/my_turnkey_cache`
1. `tool-one` is the first tool in the sequence, and it must match one of the tool names from the **first tools** list.
1. `--arg1 value1` is an optional argument passed to `tool-one`.
    - We know that it is passed to `tool-one`, not globally or to `tool-two`, because syntactically it comes after `tool-one` in the command.
    - For example, `--opset 18` is an argument that can be passed to `export-pytorch`.
    - You can learn what optional arguments are supported for each tool by running `turnkey <tool name> -h`.
1. `tool-two` is the name of the second tool in the sequence.
    - We know that `--arg2 value2` will apply to `tool-two` because it was entered after `tool-two` in the command.

A concrete example of the syntax above would be:

```
turnkey -i models/transformers/bert.py models/timm/resnet50.py discover --script-args="--batch_size 8" export-pytorch --opset 18
```

## Concepts

This section explains some of the more nuanced details of the `turnkey` tooling.

### Model Discovery

**Discovery** is the process by which the `discover` `Tool` inspects a Python script and identifies the models within.

`discover` performs does this by running and profiling your scripts. When a model object (`torch.nn.Module`) is encountered, a pointer to it is saved that can be used by other `Tools`.

> _Note_: `discover` runs your entire python script. Please ensure that these scripts are safe to run, especially if you got them from the internet.

#### Model Hashes

If there are multiple models in the same script, `discover` will print information about all of them, and automatically select one to be passed to the next tool in the sequence.

A specific model can be selected by passing its `model hash` as part of the `-i` input. The `model hash` is calculated based on the compute graph and input/output shapes for the model.

For example, if `my_script.py` contains two models, you can select the one with hash `479b1332` by running `turnkey -i my_script.py::479b1332 discover`.

#### Set Script Arguments

The `--script-args` option for `discover` can set command line arguments for the input script. Useful for customizing the behavior of the input script, for example sweeping parameters such as batch size. Format these as a comma-delimited string.

Usage:
- `turnkey -i INPUT_FILES discover --script-args="--batch_size=8 --max_seq_len=128"`
  - This will evaluate the input script with the arguments `--batch_size=8` and `--max_seq_len=128` passed into the input script.

#### Maximum Analysis Depth

The `--max-depth` option for `discover` sets the depth of sub-models to inspect within the script. Default value is 0, indicating to only analyze models at the top level of the script. Depth of 1 would indicate to analyze the first level of sub-models within the top-level models.

Usage:
- `turnkey -i INPUT_FILES discover --max-depth DEPTH`

### Providing Input Files

Name of one or more script (.py), ONNX (.onnx), or cached build (_state.yaml) files to be evaluated. You may also specify a (.txt) that lists file paths separated by line breaks.

Examples: 
- `turnkey -i models/selftest/linear.py` 
- `turnkey -i models/selftest/linear.py models/selftest/twolayer.py` 
- `turnkey -i examples/turnkey/cli/onnx/sample.onnx` 

You may also use [Bash regular expressions](https://tldp.org/LDP/Bash-Beginners-Guide/html/sect_04_01.html) to locate the files you want to benchmark.

Examples:
- `turnkey *.py`
  - Benchmark all scripts which can be found at the current working directory.
- `turnkey models/*/*.py`
  - Benchmark the entire corpora of models.
- `turnkey *.onnx`
  - Benchmark all ONNX files which can be found at the current working directory.
- `turnkey selected_models.txt`
  - Benchmark all models listed inside the text file.

> _Note_: Using bash regular expressions and filtering model by hashes are mutually exclusive. To filter models by hashes, provide the full path of the Python script rather than a regular expression.

### File Labels

Each `script` may have one or more labels which correspond to a set of key-value pairs that can be used as attributes of that given script. Labels must be in the first line of a `.py` file and are identified by the pragma `#labels`. Keys are separated from values by `::` and each label key may have one or more label values as shown in the example below:

For example:

```
#labels domain::nlp author::google task::question_answering,translation
```

These labels are collected as statistics in the `turnkey_stats.yaml` file in each build directory.

You can filter the inputs to `turnkey` by using the `--labels` optional argument. Inputs that do not have the specified label(s) will be filtered out.

For example: `turnkey -i models/*/*.py --labels task::Computer_Vision` will skip any file that doesn't have `#lables task::Computer_Vision` in it.

### Cache Directory

The results of all `Tools` are stored in a cache directory. The cache location defaults to `~/.cache/turnkey`, but it can also be set via the `--cache-dir` global argument or `TURNKEY_CACHE_DIR` environment variable (the former takes precedence over the latter).

Each run of a sequence on an input stores all of its results under a single **build directory** within the cache directory. The build name is automatically selected based on the input name, `author` label (if available), and model hash (if manually provided).

The `turnkey cache` management tool provides utility for managing the cache (e.g., deleting a build directory).

Each build directory contains:
- The state file, `<build_name>_state.yaml`, which contains all of the build state required to load that build into a new sequence using the `load-build` tool.
- The stats file, `turnkey_stats.yaml`, which collects all of the statistics collected by the tools.
  - This is what forms the content of the CSV reports generated by the `turnkey report` tool.
- One log file per tool that was executed, which may contain additional information about what happened during the tool run.
  - For example, `cache_dir/build_dir/log_discover.txt`.
- All of the artifacts produced by the tools.
  - For example, `cache_dir/build_dir/onnx/my_model.onnx`.

The `--lean-cache` global argument ensures that all build artifacts are removed at the end of the sequence. This is useful for saving disk space when gathering statistics over a large amount of models. Log files (.txt), json files (.json), and yaml files (.yaml, such as state.yaml and stats.yaml) are not removed.

### Process Isolation

Evaluate each `turnkey` input in its own isolated subprocess. This option allows the main process to continue on to the next input if the current input fails for any reason (e.g., a bug in the input script, the operating system running out of memory, etc.). 

Usage:
- `turnkey -i INPUT_FILES --process-isolation --timeout TIMEOUT`

Process isolation mode applies a timeout to each subprocess. The default timeout is 3600 seconds (1 hour) and this default can be changed with the [timeout environment variable](#set-the-default-timeout). If the child process is still running when the timeout elapses, turnkey will terminate the child process and move on to the next input file. 

> _Note_: Process isolation mode is mutually exclusive with [Slurm mode](#use-slurm).

### Use Slurm

Execute the build(s) and benchmark(s) on Slurm instead of using local compute resources. Each input runs in its own Slurm job.

Usage:
- `turnkey -i INPUT_FILES --use-slurm --timeout TIMEOUT`
  - Use Slurm to run turnkey on INPUT_FILES.
- `turnkey -i SEARCH_DIR/*.py --use-slurm --timeout TIMEOUT`
  - Use Slurm to run turnkey on all scripts in the search directory. Each script is evaluated as its on Slurm job (i.e., all scripts can be evaluated in parallel on a sufficiently large Slurm cluster).

> _Note_: Requires setting up Slurm as shown [here](https://github.com/onnx/turnkeyml/blob/main/docs/install.md).

> _Note_: while `--use-slurm` is implemented, and we use it for our own purposes, it has some limitations and we do not recommend using it. Currently, `turnkey` has some Slurm to be configuration assumptions that we have not documented yet. Please contact the developers by [filing an issue](https://github.com/onnx/turnkeyml/issues/new) if you need Slurm support for your project.

> _Note_: Slurm mode applies a timeout to each job, and will cancel the job move if the timeout is exceeded.


## Environment Variables

There are some environment variables that can control the behavior of the tools.

### Overwrite the Cache Location

By default, the tools will use `~/.cache/turnkey` as the cache location. You can override this cache location with the `--cache-dir` option.

However, you may want to override cache location for future runs without setting those arguments every time. This can be accomplished with the `TURNKEY_CACHE_DIR` environment variable. For example:

```
export TURNKEY_CACHE_DIR=~/a_different_cache_dir
```

### Show Traceback

By default, `turnkey` will display the traceback for any exceptions caught during model build. However, you may sometimes want a cleaner output on your terminal. To accomplish this, set the `TURNKEY_TRACEBACK` environment variable to `False`, which will catch any exceptions during model build and benchmark and display a simple error message like `Status: Unknown turnkey error: {e}`. 

For example:

```
export TURNKEY_TRACEBACK=False
```

### Set the ONNX Opset

By default, `turnkey` will use the default ONNX opset defined in `turnkey.common.build.DEFAULT_ONNX_OPSET`. You can set a different default ONNX opset by setting the `TURNKEY_ONNX_OPSET` environment variable.

For example:

```
export TURNKEY_ONNX_OPSET=16
```

### Set the Default Timeout

`turnkey` applies a default timeout, `turnkey.cli.spawn.DEFAULT_TIMEOUT_SECONDS`, when evaluating each input file when in [Slurm](#use-slurm) or [process isolation](#process-isolation) modes. If the timeout is exceeded, evaluation of the current input file is terminated and the program moves on to the next input file.

This default timeout can be overridden by setting the `TURNKEY_TIMEOUT_SECONDS` environment variable. 

For example:

```
export TURNKEY_TIMEOUT_SECONDS=1800
```

would set the timeout to 1800 seconds (30 minutes).

### Disable the Build Status Monitor

`turnkey` and the APIs display a build status monitor that shows progress through the various build stages. This monitor can cause problems on some terminals, so you may want to disable it.

This build monitor can be disabled by setting the `TURNKEY_BUILD_MONITOR` environment variable to `"False"`. 

For example:

```
export TURNKEY_BUILD_MONITOR="False"
```

### Adjust Build Monitor Update Frequency

The build status monitor updates its display periodically to show progress. By default, it updates every 0.5 seconds, but you can adjust the update frequency by setting the `TURNKEY_BUILD_MONITOR_FREQUENCY` environment variable to the desired number of seconds between updates.

For example:

```
export TURNKEY_BUILD_MONITOR_FREQUENCY="10.0"
```

This can be useful in long runs where frequent terminal updates might cause excessive terminal output.
