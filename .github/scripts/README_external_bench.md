# Bring-your-own backend benchmarking (external_openai)

This is the path for benchmarking **non-llama.cpp** inference servers (gufo,
halogen, vLLM, TGI, custom engines) against the same regression + leaderboard
machinery as the llama.cpp forks — without changing lemonade core. A backend is
onboarded as a single JSON entry in `src/cpp/resources/benchmark_forks.json`;
the harness launches its container, benches it over its OpenAI endpoint, and
tears it down.

## The contract a backend must satisfy

The only hard requirement:

- `POST /v1/chat/completions` (OpenAI-compatible), returning
  `choices[].message.content` **and** `usage.completion_tokens`.

That alone yields tokens-per-second (computed from tokens ÷ wall-clock) and
output-token counts. Optional extras:

- `usage.prompt_tokens` → input token count
- `timings` or `usage` prefill fields → accurate TTFT (otherwise TTFT is 0,
  since a non-streaming call can't measure it)

VRAM is **not** captured for external backends (it comes from lemond, which is
bypassed). Add `rocm-smi` polling later if you need it.

## Running it locally (recommended for first bring-up)

Container backends like gufo need a dedicated **gfx1151 Linux** box with enough
unified memory (gufo's smallest model is ~27B, so it wants a **128GB** machine —
it will NOT fit on 32GB). Rather than fight a CI runner, validate locally:

```bash
# On the 128GB gfx1151 Linux box, from a clone of this branch:
export HF_TOKEN=hf_xxx          # for model downloads
./.github/scripts/run_external_bench_local.sh gufo
```

The script replicates the CI `bench-external` job:
1. Downloads the latest lemonade embeddable (or reuse `--lemonade /path/to/lemonade`).
2. Detects docker/podman and resolves GPU group flags.
3. Caches the model outside the repo (`~/lemonade-bench-cache` by default).
4. Runs the driver: launch container → poll endpoint → `lemonade bench
   --base-url` → teardown.

Results land in `ci/results/<fork_id>/<model>/run-*.json`.

### Prerequisites on the box

- A container engine: `sudo apt-get install -y podman crun` (rootless, no
  daemon) **or** docker with your user in the `docker` group.
- The user in the `render` and `video` groups (owns `/dev/kfd`, `/dev/dri`).
  Check with `getent group render video` and `id`.
- ROCm gfx1151 driver so `/dev/kfd` and `/dev/dri` exist.

## Known unknowns to resolve at first bring-up

The launch configs are researched but a few things can only be confirmed by
running once. Watch for these:

1. **Does the server return `usage.completion_tokens` on a NON-streaming
   `/v1/chat/completions`?** If not, every run is invalid (tps=0). Some servers
   (gufo included) may only emit usage on the streaming path
   (`stream_options.include_usage`). If runs fail with no data, this is why.
2. **The exact served model id.** BYO servers often derive the model name from
   the weights, not the path. The driver reads it live from `GET /v1/models`
   and logs `resolved model id from /models: ...`. Confirm it looks right.
3. **`--ipc=host`.** Some ROCm engines (halogen) die ~2s after start without it.
   If the container exits immediately, add `--ipc=host` to the fork's
   `launch_cmd`.

### gufo-specific caveat

gufo compiles **model-specific kernel libraries** — it does NOT load an
arbitrary GGUF. It supports only a curated roster (Qwen3.8-27B dense,
Qwen3.8-Flash-Next, DeepSeek-V4). The `benchmark_forks.json` gufo entry points
at a Qwen3-30B GGUF as a placeholder; **before the first real run, set
`launch_cmd`'s `--model` (and any `--speculative`/`--dflash-model` flags) to a
model gufo actually supports**, and update `prepare_cmd` to download that model.
Read gufo's README for the exact `gufo serve llm` invocation for the dense
Qwen3.8-27B path.

## Enabling in CI after local validation

Once a backend runs clean locally:
1. In `benchmark_forks.json`, set the fork's `enabled` to `true` and pin the
   container image to an immutable tag (not `:latest`).
2. Dispatch the workflow (or let the nightly pick it up). The `bench-external`
   job runs it on the self-hosted gfx1151 runner. To run a still-disabled fork
   on demand, dispatch with `fork_filter=<fork_id>`.
