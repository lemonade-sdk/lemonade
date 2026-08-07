# Working Group: Auto-Tune

## Overview

**Lead:** This working group is led by Michele Balistreri, whose handle is @bitgamma on GitHub and @mikkoph on Discord.

**Background:** Running a model well requires choosing the right backend and tuning performance parameters such as batch size, GPU layer offload, thread count, and context size. Today, users either guess, look up Reddit threads, or ask an LLM — approaches that are error-prone and often outdated due to knowledge cutoff. Lemonade already has a `bench` command that measures TTFT, TPS, and VRAM usage across backends and parameter combinations, and a recipe system that defines per-model configuration. What is missing is a mechanism to turn benchmark data into actionable, hardware-aware defaults that apply automatically.

**Why:** A user should install Lemonade and get good performance without needing to understand backend flags or run manual benchmarks. Hardware-aware defaults lower the barrier to entry and make Lemonade competitive with cloud-based alternatives where performance is abstracted away.

**Goal:** Enable Lemonade instances to self-optimize models and backends by detecting the machine's hardware profile and applying community-validated performance parameters. The end state is that a user pulls a model, loads it, and Lemonade selects the best backend and tuning flags for their hardware — with the option to override or fine-tune manually.

## Contributing

Please see the general [contribution guidelines](../contribute.md), then contact @mikkoph on Discord before getting started to discuss the roadmap.

## Scope

This working group focuses on **performance parameters** (batch size, GPU layers, threads, context size, backend selection) that are determined by hardware characteristics. Quality parameters such as temperature, top_p, and chat template are model- or use-case-dependent and are explicitly **out of scope**.

## Roadmap

> Roadmap items are high-level objectives that may span multiple issues and PRs. Details can also be re-defined.

### Phase 1: Archetype Detection & Profile Schema

- [ ] Define the hardware archetype classification logic. Archetypes are identified by GPU family/architecture, VRAM capacity bucket, memory bandwidth tier, and whether memory is unified or discrete. This keeps the profile space bounded — two machines with the same archetype receive the same recommendations.
- [ ] Design the profile JSON schema. A profile maps an archetype ID to recommended performance parameters per backend (e.g., `strix-halo-128gb` → `{ "llamacpp_backend": "vulkan", "llamacpp_vulkan_args": { "-b 2048 -ub 1024" }, "vllm_args": { ... } }`). The schema is backend-agnostic so it works for llama.cpp, FastFlowLM, vLLM, RyzenAI, and future backends.
- [ ] Implement archetype detection in `lemond`, using data already collected by the `system-info` endpoint (GPU name, VRAM, bandwidth, unified vs. discrete memory).
- [ ] Ship an initial set of hand-curated profiles for common hardware configurations (e.g., Strix Halo, Radeon 780M, Apple M3).

### Phase 2: Auto-Apply at Load Time

- [ ] On startup, `lemond` detects the machine archetype and caches it.
- [ ] When loading a model, the router checks for archetype-specific profile overrides and merges them into the model's recipe options. A fallback chain ensures graceful degradation: exact archetype + model overlay → archetype defaults for the backend → recipe defaults (current behavior).
- [ ] Optional per-model overlays allow model-specific tuning on top of global archetype defaults (for cases where certain models benefit from different flags on the same hardware).
- [ ] `lemonade status` displays the detected archetype and any active auto-tune overrides.
- [ ] CLI or config option to enable/disable auto-tune (enabled by default).

### Phase 3: Profile Distribution

- [ ] Profiles are fetched at runtime from a remote source, allowing updates without reinstalling Lemonade. A set of default profiles ships bundled with Lemonade as a fallback when no network is available.
- [ ] Profile cache is stored locally with versioning; stale profiles are re-fetched periodically.
- [ ] `lemonade bench --submit` generates a structured benchmark contribution from local benchmark runs. The submission workflow (upload endpoint, PR-based contribution, or other) will be designed based on infrastructure study.

### Phase 4: Community Data Collection & Curation

- [ ] Tooling to merge community-submitted benchmarks into curated profiles. This includes selecting the best-performing parameter sets per scenario, validating consistency across submissions, and detecting outliers.
- [ ] Documentation for the contribution workflow so community members can benchmark their hardware and contribute results.
- [ ] Profiles cover a broad range of hardware archetypes across all supported backends.

### Phase 5: Adaptive Tuning

- [ ] Runtime monitoring: if observed performance (TPS, TTFT) deviates significantly from profile expectations, `lemond` can suggest re-benchmarking or flag the profile for review.
- [ ] "Learn from this run" — after a session, offer to save locally-observed good parameters into the user's config for persistence.
- [ ] To be continued, based on learnings from the above.
