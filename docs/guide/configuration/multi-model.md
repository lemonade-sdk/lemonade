# Multi-Model Support

Lemonade supports loading multiple models simultaneously, allowing you to keep frequently-used models in memory for faster switching. The server uses a Least Recently Used (LRU) cache policy to automatically manage model eviction when limits are reached.

## Configuration

Configure via `lemonade config set max_loaded_models=N`. See [Server Configuration](./README.md).

**Default:** `1` (one model of each type). Use `-1` for unlimited.

## Model Types

Models are categorized into these types:

- **LLM** - Chat and completion models (default type)
- **Embedding** - Models for generating text embeddings (identified by the `embeddings` label)
- **Reranking** - Models for document reranking (identified by the `reranking` label)
- **Transcription** - Models for audio transcription using Whisper (identified by the `transcription` label)
- **Image** - Models for image generation (identified by the `image` label)

Each type has its own independent LRU cache, all sharing the same slot limit set by `max_loaded_models`.

## Router and Classifier Dependencies

Models loaded internally to choose a candidate use a separate **routing-helper**
residency pool. This is intentionally independent from both `ModelType` and the
backend's hardware slot policy:

- A router LLM remains an **LLM**; it is not reclassified as a classification model.
- Standard pools continue to use `max_loaded_models` per `ModelType`.
- Routing-helper capacity is keyed by the distinct helper model rather than by
  `ModelType`. Multiple classifier or router models of the same type required by
  one or more policies can therefore remain warm together without consuming or
  evicting standard user-facing slots. A second process for the same helper model
  is never created.
- With the default `max_loaded_models=1`, a router LLM and one selected standard
  LLM candidate can stay warm together; additional distinct helper models can
  also remain resident until explicit or pressure-based eviction removes them.
- Residency follows the current use of the live process. A standard model used
  as a routing dependency is promoted in place; a routing helper called directly
  by the user is demoted into the counted standard pool in place. Destination-pool
  admission and pinning rules apply before either transition. The
  `router.model == candidate` case therefore promotes for routing and demotes for
  candidate inference within the same request without creating a second process.
- Pinning is pool-local for count-based LRU: a pinned normal candidate does not
  block the routing-helper slot, and a pinned helper does not block normal slots.
- Cloud (`Unmetered`) models consume neither pool and report `slot_pool: unmetered`.
- Implicit model selection prefers a normal `standard` model, so a warm internal
  helper never makes an otherwise unique candidate ambiguous.
- Explicit idle/VRAM-pressure eviction remains independent; an unpinned routing
  helper can still be evicted when dynamic eviction is enabled.
- Backend/device constraints remain authoritative. Internal routing work never
  evicts an already-resident model to acquire an exclusive hardware slot; that
  attempt fails with HTTP `409` and code `router_residency_conflict`. This also
  prevents two incompatible NPU/FLM helpers from repeatedly evicting one another.
  A direct user request has precedence and may evict a resident routing helper
  through the backend's normal exclusivity policy.

`GET /api/v1/health` exposes `residency_class` and `slot_pool` for every live model.
`pinned_models` remains the standard-pool summary used with `max_models`, while
`pinned_helper_models` separately reports pinned routing helpers by `ModelType`.

## Device Constraints

<!-- BEGIN GENERATED: npu-exclusivity -->
- **NPU Exclusivity:** `whispercpp`, `flm`, and `ryzenai-llm` are mutually exclusive on the NPU.
<!-- END GENERATED: npu-exclusivity -->
    - Loading a model from one of these backends will automatically evict all NPU models from the other backends.
    - `flm` supports loading 1 ASR model, 1 LLM, and 1 embedding model on the NPU at the same time.
    - `ryzenai-llm` supports loading exactly 1 LLM, which uses the entire NPU.
    - `whispercpp` supports loading exactly 1 ASR model at a time, which uses the entire NPU.
- **CPU/GPU:** No inherent limits beyond available RAM. Multiple models can coexist on CPU or GPU.

## Eviction Policy

When a model slot is full:
1. The least recently used model of that type is evicted
2. The new model is loaded
3. If loading fails (except file-not-found errors), all models are evicted and the load is retried

Models currently processing inference requests cannot be evicted until they finish.

## Dynamic VRAM Management (opt-in)

In addition to the slot-based LRU policy, Lemonade can dynamically free VRAM based on **idle time** and **global GPU memory pressure**, so the server coexists with other GPU-heavy apps (ComfyUI, Blender, games) while minimizing cold starts. This feature is **opt-in** and disabled by default.

Enable it globally:

```
lemonade config set auto_evict=true
lemonade config set auto_evict_threshold_pct=0.90   # yield VRAM at 90% global usage
```

A background monitor samples global VRAM usage (NVIDIA via `nvidia-smi`, AMD via sysfs on Linux). When usage crosses `auto_evict_threshold_pct`, the eviction engine frees the most disposable model. When `auto_evict` is disabled, no VRAM polling occurs.

**Tiered degradation.** Idle models degrade in two stages rather than a binary loaded/unloaded:

1. **Soft idle (downsize):** after `downsize_idle_timeout` seconds idle, the KV cache/context is cleared to free dynamic memory while base weights stay resident. The next request transparently restores it.
2. **Hard idle / pressure (evict):** after `evict_idle_timeout` seconds idle, or under VRAM pressure, the model is fully unloaded (VRAM released; the weights file stays in the OS page cache for a fast reload).

**Load-time-weighted scoring.** Under pressure, the engine evicts by:

```
eviction_score = idle_time_ms / (load_duration_ms * evict_weight_factor)
```

Higher score = more disposable. Fast-loading models are evicted first; slow/expensive loads are protected. Raise `evict_weight_factor` on a model to protect it further.

A request that arrives while a model is mid-downsize or mid-eviction transparently interrupts the transition and is served — no failed generations.

### Per-model eviction settings

These can be set per model on `/api/v1/load` (or globally where noted):

| Setting | Scope | Default | Meaning |
|---|---|---|---|
| `auto_evict` | global + per-model | `false` | Opt this model/server into dynamic eviction |
| `auto_evict_threshold_pct` | global | `0.90` | Global VRAM fraction that triggers pressure eviction |
| `downsize_idle_timeout` | per-model | `60` | Seconds idle before soft downsize |
| `evict_idle_timeout` | per-model | `300` | Seconds idle before full eviction |
| `evict_weight_factor` | per-model | `1.0` | Eviction-protection weight (higher = more protected) |

## Model Pinning

To prevent frequently used models from being auto-evicted by the LRU policy, you can "pin" them in memory. Pinned models are excluded from the eviction candidate search.

### Pinning Behavior

- **Load-time Pinning:** You can request a model to be pinned when loading it by passing the `--pinned` flag in the CLI:
  ```bash
  lemonade load Qwen3-0.6B-GGUF --pinned
  ```
  Or by setting `"pinned": true` in the POST request body to `/api/v1/load`.
- **Dynamic Pinning/Unpinning:** Toggles the pinned status of a currently loaded model dynamically without needing a reload:
  ```bash
  lemonade pin Qwen3-0.6B-GGUF
  lemonade unpin Qwen3-0.6B-GGUF
  ```
  Or via a POST request to `/internal/pin`.
- **Pre-emptive Warnings:** The CLI checks `/api/v1/health` before loading a new model. If all slots for that model type are occupied by pinned models, the CLI will output a pre-emptive warning alerting you that loading will fail.
- **Capacity Failures:** If every model in the target `(residency class, model type)` pool is pinned and another model needs that pool, the load request fails with a `409 Conflict` HTTP status containing a `slots_pinned_error` code. You must unpin or unload a model from that pool to free its slot.

## Per-Model Settings

Each model can be loaded with custom settings (context size, llamacpp backend, llamacpp args) via the `/api/v1/load` endpoint. These per-model settings override the default values set via CLI arguments or environment variables. See the [`/api/v1/load` endpoint documentation](../../api/lemonade.md#post-v1load) for details.

**Setting Priority Order:**
1. Values passed explicitly in `/api/v1/load` request (highest priority)
2. Per-model values from `recipe_options.json`
3. Per-architecture defaults from `architecture_defaults.json` (shipped resource; keyed by GGUF architecture name)
4. Values from environment variables or server startup arguments (see [Server Configuration](./README.md))
5. Hardcoded defaults in `lemond` (lowest priority)
