# 🌩️ Self Hosted Runners 🌩️ Documentation

This page documents how to set up and maintain self-hosted runners for lemonade-sdk.

Topics:
 - [What are Self-Hosted Runners?](#what-are-self-hosted-runners)
 - [Runner Labels](#runner-labels)
 - [New Runner Setup](#new-runner-setup)
 - [Maintenance and Troubleshooting](#maintenance-and-troubleshooting)
    - [Check your runner's status](#check-your-runners-status)
    - [Actions are failing unexpectedly](#actions-are-failing-unexpectedly)
    - [Take a laptop offline](#take-a-laptop-offline)
- [Creating Workflows](#creating-workflows)
    - [Capabilities and Limitations](#capabilities-and-limitations)

## What are Self-Hosted Runners?

A "runner" is a computer that has installed GitHub's runner software, which runs a service that makes the laptop available to run GitHub Actions. In turn, Actions are defined by Workflows, which specify when the Action should run (manual trigger, CI, CD, etc.) and what the Action does (run tests, build packages, run an experiment, etc.).

You can read about all this here: [GitHub: About self-hosted runners](https://docs.github.com/en/actions/hosting-your-own-runners/managing-self-hosted-runners/about-self-hosted-runners).

## Runner Labels

Workflows target self-hosted runners by the labels the runner carries. We use two kinds of labels:

### Capability labels

These describe *what a runner can do*. A workflow should request only the capability labels it actually needs.

| Label | Meaning | Typical workflows that request it |
|-------|---------|-----------------------------------|
| `vulkan` | Runner can execute Vulkan GPU workloads | llama.cpp Vulkan backend, whisper.cpp Vulkan backend |
| `rocm` | Runner can execute ROCm GPU workloads | llama.cpp ROCm backend, stable-diffusion.cpp ROCm backend |
| `cuda` | Runner can execute CUDA GPU workloads | TBD |
| `xdna2` | Runner has a Ryzen AI 300/400 series NPU | `ryzenai` backend, `flm` (FastFlowLM) backend |

A job that exercises more than one backend should request all the labels it needs (e.g., `[Windows, vulkan, rocm]` for a test that runs both Vulkan and ROCm cases). GitHub Actions requires the runner to carry *every* label in the `runs-on` list.

CPU-only jobs should target GitHub-hosted runners when possible.

### Hardware labels

These pin a job to a specific hardware class when a capability label alone isn't enough to distinguish it. Combine them with a capability label.

| Label | Hardware | When to use |
|-------|----------|-------------|
| `stx-halo` | Strix Halo (AMD Ryzen AI Max 300 series) | ROCm workloads that specifically need Strix Halo's iGPU, e.g., `[Windows, rocm, stx-halo]` |

Add new hardware labels here as the pool grows.

### Applying labels to a runner

Capability and hardware labels must be present on each runner for the workflow to match. Add or remove them from the [runners page](https://github.com/organizations/lemonade-sdk/settings/actions/runners): click the runner, click the gear icon in the Labels section, and check/uncheck as needed. Apply only the labels that reflect the runner's real capabilities — never add `rocm` to a runner that can't actually run ROCm, for example, because that will cause workflows to be scheduled on a machine that can't complete them.

### Typical label sets by hardware

| Hardware | Labels to apply |
|----------|-----------------|
| Ryzen AI 300-series laptop (NPU + Vulkan iGPU + ROCm iGPU) | `xdna2`, `vulkan`, `rocm` |
| Strix Halo | `xdna2`, `rocm`, `stx-halo` |

## New Runner Setup

This guide will help you set up a computer as a GitHub self-hosted runner.

### New Machine Setup

- Install the following software:
    - The latest RyzenAI driver ONLY (do not install RyzenAI Software), which is [available here](https://ryzenai.docs.amd.com/en/latest/inst.html#install-npu-drivers)
    - [VS Code](https://code.visualstudio.com/Download)
    - [git](https://git-scm.com/downloads/win)
- If your laptop has an Nvidia GPU, and you want the `rocm` capability instead of `cuda`, you must disable it in device manager
- Open a PowerShell script in admin mode, and run `Set-ExecutionPolicy -ExecutionPolicy RemoteSigned`
- Go into Windows settings:
  - Go to system, power & battery, screen sleep & hibernate timeouts, and make it so the laptop never sleeps while plugged in. If you don't do this it can fall asleep during jobs.
  - Search "Change the date and time", and then click "sync" under "additional settings."

### Runner Configuration

These steps will place your machine into the production pool.

1. IMPORTANT: before doing step 2, read this:
    - Use a powershell administrator mode terminal
    - Enable permissions by running `Set-ExecutionPolicy RemoteSigned`
    - When running `./config.cmd` in step 2, make the following choices:
         - Name of the runner group = `stx`
         - For the runner name, call it `NAME-TYPE-NUMBER`, where NAME is your alias and NUMBER would tell you this is the Nth machine of TYPE you've added. TYPE examples include `stx`, `stx-halo`, `phx`, etc.
         - Apply capability labels (`xdna2`, `vulkan`, `rocm`, etc. and any hardware labels like `stx-halo`).
         - Accept the default for the work folder
         - You want the runner to function as a service (respond Y)
         - User account to use for the service = `NT AUTHORITY\SYSTEM` (not the default of `NT AUTHORITY\NETWORK SERVICE`)

1. Follow the instructions here for Windows|Ubuntu, minding what we said in step 1: https://github.com/organizations/lemonade-sdk/settings/actions/runners/new
1. You should see your runner show up in the `stx` runner group in the lemonade-sdk org

## Maintenance and Troubleshooting

This is a production system and things will go wrong. Here is some advice on what to do.

### Check your runner's status

You can run `Get-EventLog -LogName Application -Source ActionsRunnerService` in a powershell terminal on your runner to get more information about what it's been up to.

If there have been any problems recently, they may show up like:

- Error: Runner connect error: < details about the connection error >
- Information: Runner reconnected
- Information: Running Job: < job name >
- Information: Job < job name > completed with result: [Succeeded / Canceled / Failed]

### Actions are failing unexpectedly

Actions fail all the time, often because they are testing buggy code. However, sometimes an Action will fail because something is wrong with the specific runner that ran the Action.

If this happens to you, here are some steps you can take (in order):
1. Take note of which runner executed your Action. You can check this by going to the `Set up job` section of the Action's log and checking the `Runner name:` field. The machine name in that field will correspond to a machine on the [runners page](https://github.com/organizations/lemonade-sdk/settings/actions/runners).
1. Re-queue your job. It is possible that that the failure is a one-off, and it will work the next time on the same runner. Re-queuing also gives you a chance of getting a runner that is in a healthier state.
1. If the same runner is consistently failing, it is probably in an unhealthy state (or you have a bug in your code and you're just blaming the runner). If a runner is in an unhealthy state:
    1. [Take the laptop offline](#take-a-laptop-offline) so that it stops being allocated Actions.
    1. [Open an Issue](https://github.com/lemonade-sdk/lemonade/issues/new). Assign it to the maintainer of the laptop (their name should be in the runner's name). Link the multiple failed workflows that have convinced you that this runner is unhealthy.
    1. Re-queue your job. You'll definitely get a different runner now since you took the unhealthy runner offline.
1. If all runners are consistently failing your workflow, seriously think about whether your code is the problem.

### Take a laptop offline

If you need to do some maintenance on your laptop, use it for dev/demo work, etc. you can remove it from the runners pool.

Also, if someone else's laptop is misbehaving and causing Actions to fail unexpectedly, you can remove that laptop from the runners pool to make sure that only healthy laptops are selected for work.

There are three options:

Option 1, which is available to anyone in the `lemonade-sdk` org: remove the runner's capability labels.
- Workflows target runners by requesting capability labels like `xdna2`, `vulkan`, and `rocm` (see [Runner Labels](#runner-labels)). Removing every capability label from a runner will drain it completely — no workflow will match.
- To drain the runner for only one backend (e.g., take it out of ROCm jobs but keep it available for NPU jobs), remove just that one capability label.
- Go to the [runners page](https://github.com/organizations/lemonade-sdk/settings/actions/runners), click the specific runner in question, click the gear icon in the Labels section, and uncheck the capability labels you want to drain.
- To reverse this action later, go back to the [runners page](https://github.com/organizations/lemonade-sdk/settings/actions/runners), click the gear icon, and re-check the labels you removed.

Option 2, which requires physical/remote access to the laptop:
- In a PowerShell terminal, run `Stop-Service "actions.runner.*"`.
- To reverse this action, run `Start-Service "actions.runner.*"`.

Option 3 is to just turn the laptop off :)

## Creating Workflows

GitHub Workflows define the Actions that run on self-hosted laptops to perform testing and experimentation tasks. This section will help you learn about what capabilities are available and show some examples of well-formed workflows.

### Capabilities and Limitations

Because we use self-hosted systems, we have to be careful about what we put into these workflows so that we avoid:
- Corrupting the laptops, causing them to produce inconsistent results or failures.
- Over-subscribing the capacity of the available laptops

Here are some general guidelines to observe when creating or modifying workflows. If you aren't confident that you are properly following these guidelines, please contact someone to review your code before opening your PR.

- Place a 🌩️ emoji in the name of all of your self-host workflows, so that PR reviewers can see at a glance which workflows are using self-hosted resources.
    - Example: `name: Test Lemonade on NPU and Hybrid with OGA environment 🌩️`
- Avoid triggering your workflow before anyone has had a chance to review it against these guidelines. To avoid triggers, do not include `on: pull request:` in your workflow until after a reviewer has signed off.
- Request only the capability labels your job actually needs (see [Runner Labels](#runner-labels)). For example, `runs-on: [Windows, xdna2]` for NPU work, `runs-on: [Linux, vulkan, rocm]` for a job that exercises both GPU backends. Do not ask for `xdna2` or a GPU capability if your job is CPU-only — use `[self-hosted, Windows]` / `[self-hosted, Linux]`, or move the step to a GitHub-hosted runner like `runs-on: windows-latest` when possible.
- Be very considerate about installing software on to the runners:
    - Installing software into the CWD (e.g., a path of `.\`) is always ok, because that will end up in `C:\actions-runner\_work\REPO`, which is always wiped between tests.
    - Installing software into `AppData`, `Program Files`, etc. is not advisable because that software will persist across tests. See the [setup](#new-runner-setup) section to see which software is already expected on the system.
- Always create new virtual environments in the CWD, for example `python -m venv .venv`.
    - This way, the virtual environment is located in `C:\actions-runner\_work\REPO`, which is wiped between tests.
    - Make sure to activate your virtual environment before running any `pip install` commands. Otherwise your workflow will modify the system Python installation!
- PowerShell scripts do not necessarily raise errors by programs they call.
    - That means PowerShell can call a Python test, and then keep going and claim "success" even if that Python test fails and raises an error (non-zero exit code).
    - You can add `if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }` after any line of script where it is that is particularly important to fail the workflow if the program in the preceding line raised an error.
        - For example, this will make sure that lemonade installed correctly:
            1. pip install -e .
            2. if ($LASTEXITCODE -ne 0) { exit $LASTEXITCODE }
- Be considerate of how long your workflow will run for, and how often it will be triggered.
    - All workflows go into the same queue and share the same pool of runners.
    - A good target length for a workflow is 15 minutes.
- Be considerate of how much data your workflow will download.
    - It would be very bad to fill up a hard drive, since Windows machines misbehave pretty bad when their drives are full.
    - Place your Hugging Face cache inside the `_work` directory so that it will be wiped after each job.
        - Example: `$Env:HF_HOME=".\hf-cache"`
    - Place your Lemonade cache directory inside the `_work` directory so that it will be wiped after each job.
        - Example: Pass the cache dir as the first argument to `lemond`: `lemond .\ci-cache`
