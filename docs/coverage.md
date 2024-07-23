# Evaluating TurnkeyML Coverage

This is a simple tutorial on how to evaluate TurnkeyML test coverage using [Coverage.py](https://coverage.readthedocs.io/en/7.3.2/#quick-start).

## Basic Test Coverage

### Installation

On your main `tkml` environment, run `pip install coverage`.

### Gathering Results

To gather results, cd into the test folder on `REPO_ROOT/test` and call `coverage run` on each of the tests as shown below.

```
coverage run --data-file=.coverage_unit -m unittest unit.py
coverage run --data-file=.coverage_analysis -m unittest analysis.py
coverage run --data-file=.coverage_cli -m unittest cli.py
```

### Combining Results

You can the combine all results into a single file using the command shown below.

```
coverage combine --keep .coverage_analysis .coverage_cli .coverage_unit
```

This will generate a combined file called `.coverage`.

### Generating Report

For a human-readable report, run `coverage html -i --data-file=.coverage` to generate an html report. If your goal is to later read this information programmatically, `coverage json -i --data-file=.coverage` is a better option.

## Advanced Test Coverage

TurnkeyML spawns sub-processes in multiple scenarios to do things such as enabling the use of multiple conda environments. Sub-processes are also spawned in many of our tests.

Measuring coverage in those sub-processes can be tricky because we have to modify the code spawning the process to invoke `coverage.py`.

Enabling tracing of coverage on sub-processes is currently only partially possible, as some of the subprocesses used inside `turnkey` fail when used with `coverage.py`.

The instructions below show how to measure coverage using this advanced setup.

Please note that, without this advanced setup, files that only execute within a subprocess are not analyzed at all. 

### Preparation

#### Step 1: Installing coverage on all environments

First, make sure to `pip install coverage` on all environments used by `turnkey`. Run `conda env list` and install `coverage` on all environments that are named `turnkey-onnxruntime-*-ep`. From now on, we will refer to those as `turnkey environments`.

#### Step 2: Edit Python processes startup

Now, we have to configure Python to invoke `coverage.process_startup()` when Python processes start. To do this, add a file named `sitecustomize.py` to `<YOUR_PATH>\miniconda3\envs\<turnkey-onnxruntime-*-ep>\Lib\site-packages\sitecustomize.py`, where `<turnkey-onnxruntime-*-ep>` corresponds to each of your turnkey environments. Each of those files should have the content shown below:

```python
import coverage
coverage.process_startup()
print("STARTING COVERAGE MODULE")
```
