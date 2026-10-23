# Running AMICA models in Lemonade

This guide sets up Lemonade's `amica` recipe from scratch: building AMICA, building Lemonade against AMICA, configuring it, and testing the result. For the recipe's options and behavior, see [AMICA Backend Options](../../../docs/guide/configuration/amica.md).

## How the pieces fit

```
client ──HTTP──▶ lemond ──HTTP──▶ amica-engine.exe ──▶ amica.dll (D3D12) ──▶ GPU
                                       │
                                       └─ reads the model bundle: CGC MLIR graphs,
                                          safetensors weights, tokenizer.json,
                                          chat template, configs
```

- **`lemond`** routes OpenAI-style requests to the backend for the loaded model.
- **`amica-engine.exe`** (built from this directory) is the whole AMICA backend. It compiles the bundle's graphs through the public AMICA API, keeps the weights resident on the GPU, tokenizes and renders prompts, and serves `/v1/chat/completions` and `/v1/completions`. It needs no Python.
- **`amica.dll`** comes from your AMICA install and is loaded at run time from the install Lemonade is configured with, so you can update AMICA without rebuilding Lemonade unless its API changes.

## 1. Prerequisites

- Windows 10 version 1903 or later. The engine uses the ICU library built into Windows.
- An AMD RDNA3 or RDNA4 GPU (gfx110X, gfx1150, gfx1151, gfx120X) with a current Adrenalin driver.
- Visual Studio 2022 or 2026 with the "Desktop development with C++" workload, CMake 3.24 or later (for Lemonade's presets), and Git.
- Network access for the first configure of each repository (dependencies are fetched).
- Enough memory for the model. Weights that do not fit in VRAM are placed in system memory (see [step 4](#4-configure-lemonade)), and they stay allocated while the model is loaded.

| Model | Weights | Fits a 16 GB GPU without offload | Prompt / context tokens | Notes |
|-------|---------|------------------------------|-------------------------|-------|
| qwen2.5-0.5b-cgc | 0.8 GB | yes | 128 / 192 | |
| gpt-oss-20b | 12 GB | yes | 128 / 193 | Harmony chat format (reasoning is split out) |
| gpt-oss-120b | 62 GB | no; needs about 64 GB of free RAM | 128 / 193 | |
| qwen3.8-flash-next | 100 GB | no; needs about 100 GB of free RAM | 128 / 2048 |

## 2. Build AMICA

AMICA lives in the `amd-mlcompiler` repository. Its bootstrap script clones the sibling dependencies, downloads the pinned kernel toolchains, configures, and builds. From the repository root in PowerShell:

```powershell
# First build on a machine
.\amica_bootstrap.ps1

# Later rebuilds, without re-cloning dependencies or reconfiguring
.\amica_bootstrap.ps1 -SkipDeps -SkipGeneration
```

The build produces an install directory, by default `build\WinRelWithDebInfo\install`, laid out as:

```
install\
  amica\          amica.dll and the DLLs it loads (migraphx_gpu.dll, ...)
  include\amica\  amica.h, amica.hpp (the public API amica-engine compiles against)
  lib\            amica.lib
  tools\          amica-driver.exe, Chatapp.ps1, chatapp\, test suites
```

Lemonade needs `include\` and `lib\` to build `amica-engine`, and `amica\` plus `tools\` at run time.

## 3. Build Lemonade with AMICA

Configure Lemonade with `LEMON_AMICA_ROOT` pointing at the AMICA install from step 2, then build:

```powershell
cmake --preset vs18 -DLEMON_AMICA_ROOT="C:/Develop/amd-mlcompiler/build/WinRelWithDebInfo/install"
cmake --build --preset vs18
```

Use `--preset windows` for Visual Studio 2022. The configure output should include:

```
-- Building amica-engine against C:/Develop/amd-mlcompiler/build/WinRelWithDebInfo/install
```

`build\Release\` then contains `lemond.exe`, `lemonade.exe`, `LemonadeServer.exe` and `amica-engine.exe`. Without `LEMON_AMICA_ROOT` Lemonade still builds, but the `amica` recipe reports that the engine was not built.

To rebuild only the engine after changing files in this directory:

```powershell
cmake --build build --config Release --target amica-engine
```

## 4. Configure Lemonade

Point Lemonade at the AMICA `tools` directory:

```powershell
.\build\Release\lemonade.exe config set amica.dx12_bin="C:\Develop\amd-mlcompiler\build\WinRelWithDebInfo\install\tools"
```

Model bundles are the converted model directories under `test\amica\mlir\models\<name>` in the AMICA repository (download them with `tools\RunChatappSuite.ps1 -DownloadTests -BaseTestFolder <path>` if they are missing). They need no preparation: when a bundle has no `amica_app.json`, `amica-engine` derives it from the bundle's files the way ChatApp does.

Register each bundle in `user_models.json` in the Lemonade config directory (`%USERPROFILE%\.config\lemonade\user_models.json`), with `"source": "local_path"` and the bundle directory as the checkpoint. `size` is the weight size in GB and only informs the UI:

```json
{
    "Qwen2.5-0.5B-Instruct-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\Develop\\amd-mlcompiler\\test\\amica\\mlir\\models\\qwen2.5-0.5b-cgc",
        "recipe": "amica",
        "labels": ["chat"],
        "size": 0.76
    },
    "GPT-OSS-20B-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\Develop\\amd-mlcompiler\\test\\amica\\mlir\\models\\gpt-oss-20b",
        "recipe": "amica",
        "labels": ["chat"],
        "size": 13
    },
    "GPT-OSS-120B-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\Develop\\amd-mlcompiler\\test\\amica\\mlir\\models\\gpt-oss-120b",
        "recipe": "amica",
        "labels": ["chat"],
        "size": 66.6
    },
    "Qwen3.8-Flash-Next-AMICA": {
        "source": "local_path",
        "checkpoint": "C:\\Develop\\amd-mlcompiler\\test\\amica\\mlir\\models\\qwen3.8-flash-next",
        "recipe": "amica",
        "labels": ["chat"],
        "size": 101
    }
}
```

Restart `lemond` after editing the file.

### Engine options

Pass `amica-engine` options per load with `--amica-args`, or set them for every AMICA model with `lemonade config set amica.args="..."`:

| Option | Use |
|--------|-----|
| `--weights-vram-mib N` | Keep at most N MiB of weights in VRAM and put the rest in system memory. |
| `--adapter <index or name>` | Choose the GPU: an index among AMD adapters, or part of the adapter name. By default the first supported discrete AMD GPU is used. On a machine with several GPUs, choosing one that does not drive a display keeps the desktop responsive if the GPU hangs. |
| `--arch gfxNNNN` | Override the GPU architecture AMICA compiles for. |

Weights in system memory are read over PCIe for every token, so offloading trades speed for fitting. On an RX 9070 XT, gpt-oss-20b runs at about 122 tokens/s fully in VRAM and about 4 tokens/s with two thirds of its weights offloaded.

## 5. Run

Start the server, then load and use a model from another terminal:

```powershell
.\build\Release\lemond.exe

.\build\Release\lemonade.exe list               # the AMICA models should appear
.\build\Release\lemonade.exe load Qwen2.5-0.5B-Instruct-AMICA
.\build\Release\lemonade.exe load Qwen3.8-Flash-Next-AMICA --amica-args "--weights-vram-mib 12000"
.\build\Release\lemonade.exe run Qwen2.5-0.5B-Instruct-AMICA   # opens the web UI
```

Loading compiles the graphs and uploads the weights. Expect about 12 s for qwen2.5-0.5b, 25 s for gpt-oss-20b, and about 3 minutes for the offloaded gpt-oss-120b and qwen3.8-flash-next. The `lemond` log shows the engine's progress (`[amica-engine] ...`), ending with:

```
[amica-engine] model=... adapter='AMD Radeon RX 9070 XT' arch=gfx1201 prefill_capacity=[128] load_ms=... context=192 padding=left
[amica-engine] listening on http://127.0.0.1:<port>
```

Generation is greedy and requests run one at a time. Prompts longer than the bundle's prefill capacity drop their oldest turns, and `max_tokens` is clamped to the remaining context.

## 6. Test

### Smoke test

With `lemond` running on its default port 13305, exercise each endpoint from PowerShell:

```powershell
$base = "http://localhost:13305/api/v1"
$model = "Qwen2.5-0.5B-Instruct-AMICA"

# Models
Invoke-RestMethod "$base/models" | ConvertTo-Json -Depth 4

# Chat
$body = @{ model = $model; max_tokens = 40
           messages = @(@{ role = "user"; content = "What is the capital of France?" }) } | ConvertTo-Json -Depth 4
(Invoke-RestMethod "$base/chat/completions" -Method Post -ContentType "application/json" -Body $body).choices[0].message

# Raw completion
$body = @{ model = $model; max_tokens = 8; prompt = "The capital of France is" } | ConvertTo-Json
(Invoke-RestMethod "$base/completions" -Method Post -ContentType "application/json" -Body $body).choices[0].text

# Streaming: prints the server-sent events as they arrive
curl.exe -N "$base/chat/completions" -H "Content-Type: application/json" `
  -d '{"model":"Qwen2.5-0.5B-Instruct-AMICA","stream":true,"max_tokens":64,"messages":[{"role":"user","content":"Count from 1 to 5."}]}'
```

Generation is greedy, so these outputs are deterministic for a given AMICA build:

| Model | Request | Expected |
|-------|---------|----------|
| qwen2.5-0.5b | chat "What is the capital of France?" | `The capital of France is Paris.` |
| qwen2.5-0.5b | completion "The capital of France is" | ` Paris, a city that has been a` |
| qwen2.5-0.5b | streamed "Count from 1 to 5." | `1\n2\n3\n4\n5` |
| gpt-oss-20b | chat "What is the capital of France?" | `content`: `The capital of France is **Paris**.`, plus a `reasoning_content` field |
| qwen3.8-flash-next | chat "What is the capital of France?" | reasoning ending in `</think>`, then `The capital of France is **Paris**.` |

Each response carries `usage` and llama.cpp-style `timings` (`prompt_ms` is the time to first token; `predicted_per_second` is the decode rate). For reference on an RX 9070 XT: qwen2.5-0.5b decodes at about 300 tokens/s, gpt-oss-20b at about 125, and the offloaded gpt-oss-120b and qwen3.8-flash-next at about 3 to 4 tokens/s.

Also check that errors come back as HTTP 400 with a message, for example a prompt longer than the bundle accepts:

```
The prompt is 3030 tokens, but this AMICA bundle accepts at most 128 prompt tokens
```

### Long-prompt check

A quick check is a raw completion of about 120 tokens whose answer depends on its first sentence:

```powershell
$filler = "The weather in the valley was mild that week, and the farmers spent their mornings repairing fences, " +
          "feeding animals, and talking about the harvest that was coming in the autumn months ahead. "
$body = @{ model = "Qwen3.8-Flash-Next-AMICA"; max_tokens = 6
           prompt = "The secret password is 7391. " + ($filler * 3) + "The secret password is" } | ConvertTo-Json
(Invoke-RestMethod "$base/completions" -Method Post -ContentType "application/json" -Body $body).choices[0].text
```

The answer should start with ` 7391.` (llama.cpp running the GGUF of the same model gives the same continuation). Anything else points at the AMICA build.

### Running the engine without Lemonade

`amica-engine.exe` serves the same API on its own, which isolates engine problems from Lemonade:

```powershell
.\build\Release\amica-engine.exe `
  --model-dir C:\Develop\amd-mlcompiler\test\amica\mlir\models\qwen2.5-0.5b-cgc `
  --amica-dir C:\Develop\amd-mlcompiler\build\WinRelWithDebInfo\install\amica `
  --port 8090 --model-name qwen

Invoke-RestMethod http://127.0.0.1:8090/health
```

Then send the requests above to `http://127.0.0.1:8090/v1/...`.

### Comparing with AMICA's own runner

ChatApp runs the same bundle through `amica-driver`. Greedy output from Lemonade should match it token for token for the same prompt:

```powershell
cd C:\Develop\amd-mlcompiler\build\WinRelWithDebInfo\install\tools
.\Chatapp.ps1 --model qwen2.5-0.5b --prompt "What is the capital of France?" --max-tokens 40
```

### Unit tests

After changing Lemonade's AMICA backend code, run the related Lemonade tests from `build\Release`:

```powershell
foreach ($t in "test_backend_mode_contract", "test_custom_args", "test_recipe_options_precedence",
                "test_recipe_arg_resolution", "test_model_registry") { & ".\build\Release\$t.exe" }
```

## Troubleshooting

| Symptom | Cause and fix |
|---------|---------------|
| `AMICA is not distributed with Lemonade. Point it at an AMICA install ...` | `amica.dx12_bin` is not set or does not contain `amica-driver.exe`. Set it to `install\tools` (step 4). |
| `amica-engine was not built with this Lemonade` | Lemonade was configured without `LEMON_AMICA_ROOT`. Reconfigure and rebuild (step 3). |
| `amica.dll was not found in ...\amica` | `amica.dx12_bin` points at a `tools` directory without a sibling `amica` directory. Use the full AMICA install. |
| `... has no amica_app.json, and its graph <name>.mlir was not found`, `Neither weights.safetensors nor model.safetensors was found`, or `A runnable FP16 LM-head graph was not found` | The bundle is incomplete or uses a layout the engine cannot derive a manifest for. Download the bundle again, or run `Chatapp.ps1 --model <name>` once so ChatApp writes an `amica_app.json` for it. |
| `tokenizer.json uses ..., which amica-engine's tokenizer does not support` | The bundle's tokenizer is not one of the byte-level BPE tokenizers the engine implements. |
| The load fails with out-of-memory errors, or Windows becomes unresponsive while loading | The weights do not fit. Lower `--weights-vram-mib` if VRAM is short, and free system RAM for the offloaded part (see the table in step 1). |
| `GPU execution (device removed) failed: HRESULT 0x887A0006` | The GPU hung or was reset. The engine exits and Lemonade reloads the model on the next request. Check the driver, the AMICA build, and VRAM headroom; running on a GPU that does not drive a display (`--adapter`) avoids desktop freezes while investigating. |
| `amica-engine.exe` keeps running after `lemond` was killed | A forced kill skips Lemonade's child cleanup. End it with `taskkill /IM amica-engine.exe /F`. |
