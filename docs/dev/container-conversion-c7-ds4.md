# C7 DS4 ROCm conversion stop

C7 inspected stack `8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78` against integration base
`4f9bb9e3e768953ddc967c59a06710259a5eed60`. This engine was **not migrated**:
the remaining behavior below cannot be preserved by the current v2 contracts.
The assignment requires stopping an engine rather than dropping behavior.

Production definition lines added: **0**. No execution grant or owner digest,
active leaf, registration, model row, version pin, generated default/support
section, parity fixture or identity transition was added for this engine.
The source captures and CPU probes are preflight evidence, not migration parity.

## Pinned inventory

| Field | Stack source inventory, not production additions |
|---|---|
| Recipe / backend | `ds4` / `rocm` |
| Version key | `ds4.rocm` |
| Repository | `docker.io/kyuz0/strix-halo-ds4-toolbox` |
| Pin | `rocm-10.0@sha256:31b4e7e3584684768ee060e3dfeb62c1d716219da468090f3483977fdf6dfb47` |
| Registry rows with this recipe | 10 |
| Defaults | `{"args":""}` |
| Pin validation | **Unvalidated. No registry access was performed.** |

Reproduce each quotation with `git show 8f8b905a1cf68b60a4d7fdc66a15d846b0cebc78:<path>`.
Exact captures, including version/default files and all recipe model rows, are
in `.synagent/artifacts/container-v2/c7/source/ds4/`.

## Missing automatic-context budget interface and contract

The named earlier `cb-3-container-ds4` source remains native at
`5df9cbe2de1c77eb7e5b28be2bf6d1d527f77272`; this audit targets the later OCI
stack identified above. Its `src/cpp/server/backends/ds4/ds4_server.cpp:257-262`
contains a further management override omitted from the C6b launch prerequisite:

```cpp
std::optional<double> auto_context_kv_budget_gib(const ModelInfo& info,
                                                 double available_gib) const override {
    const MemoryPlan plan = memory_plan(igpu_pool_gb(), weights_gib(info));
    const double working_set_gb = plan.resident ? weights_gib(info) : info.min_resident_gb;
    if (working_set_gb <= 0.0) return std::nullopt;
    return available_gib - plan.free_reserve_gb - working_set_gb;
}
```

The integration `BackendOps` has no `auto_context_kv_budget_gib` method.
`ConfiguredV2BackendOps` has no corresponding override or runner operation, and
`src/cpp/include/lemon/auto_tune.h:292` calls the generic
`estimate_auto_context_budget` directly. This automatic-context decision occurs
before `prepare_load`; C6b's `memory_plan: "ds4_rocm_v1"` collects launch facts
later and cannot repair a prior context refusal or select the source KV budget.

A CPU arithmetic counterexample compiles the exact source `MemoryPlan` and
`memory_plan` bodies. With pool 62 GiB, weights 64 GiB, streaming working set
8 GiB and available memory 50 GiB, the source reserve is 37 GiB and its KV
budget is **5 GiB**. The current generic estimator charges the full weights and
produces **0 GiB / 0 tokens**. No memory capacity was measured.
A separate declaration of the source override against this branch's actual
`BackendOps` header fails with `marked override, but does not override`.
This expected compiler rejection is evidence of the absent API, not a mutation
kill or a successful engine parity run.

The pinned `Ds4Ops::populate_metadata` at lines 244-251 also deliberately
preserves registered capability labels while populating GGUF metadata. A future
DS4 metadata binding must retain that behavior; the existing llama artifact
metadata hook is not a blanket substitute.

The missing prerequisite must connect typed DS4 planning facts to automatic
context sizing and model metadata before Router context admission, preserving
the source nullopt branch and resident/streaming/draft accounting. It needs an
independent identity disclosure. Native `ds4.json` must remain byte-identical.
Evidence is retained in `.synagent/artifacts/container-v2/c7/` as
`ds4-budget-{compile,run}.log` and `ds4-override-expected-rejection.log`.
