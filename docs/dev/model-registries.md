# Remote Model Registries

Lemonade supports Hugging Face and ModelScope as remote model registries. This document records the implementation contract so later providers can be added without reintroducing provider-specific assumptions.

## Source identity

Remote provenance is stored separately from local origin:

- `ModelInfo::source` describes a local origin such as `local_upload`, `local_path`, or `extra_models_dir`.
- `ModelInfo::registry_source` identifies the remote registry: `huggingface` or `modelscope`.
- The public registration field is `source`. For a remote model it is canonicalized and persisted as the registry name. `registry_source` is also accepted in exported/internal records.

Hugging Face is the shipped default for backward compatibility, but the effective default for source-less pulls is the `default_model_source` config key (see [Default source policy](#default-source-policy) below). A model and all of its checkpoints have one registry source. Collection components inherit the collection source unless an inline component definition explicitly declares another source.

## Default source policy

When a pull request names no registry — no `--source`, no `source`/`registry_source` field, and no provider URL — lemond falls back to the `default_model_source` config key (`huggingface` or `modelscope`, default `huggingface`). Resolution is server-side and authoritative so every client (CLI, desktop, web, Ollama-compatible endpoints) inherits the same policy, per the many-clients-one-server contract. The resolution happens once, up front in the `/pull` handler, so the chosen registry is persisted with the model and stays consistent across download, cache layout, and later refresh. A bare `pull <registered-name>` refresh carries no checkpoint and keeps its recorded provenance rather than re-applying the default. `GET /pull/variants` echoes the resolved `source` so the CLI's interactive flow records the same registry the weights came from.

Provider URL detection is shared code (`detect_registry_url`) applied server-side in `apply_default_pull_source`, not only in the CLI. A provider URL is authoritative for its registry: every registry-backed checkpoint in the body (primary and secondary entries such as `mmproj`, `vae`, or `text_encoder`) is normalized to `owner/repo`, and the whole model must resolve to a single registry. The primary checkpoint follows the same precedence as model registration — `checkpoints.main` when a `checkpoints` object is present, otherwise `checkpoint`. A `source`/`registry_source` that conflicts with a checkpoint's provider URL, or checkpoints that disagree on the registry, are rejected with `400`, matching the CLI.

## Registry interface

`ModelRegistry` normalizes provider behavior into:

- repository file-tree discovery;
- a stable local snapshot identifier;
- provider-specific file download URLs;
- provider-specific authentication headers.

Variant discovery and the main download engine consume this normalized representation. `download_from_manifest()` remains provider-independent.

## Cache contract

Both providers use the configured `models_dir` and the same runtime structure:

```text
<repo-cache>/
  refs/main
  snapshots/<snapshot-id>/...
  .lemonade_registry.json
```

Repository directory names are provider-qualified:

- Hugging Face: `models--org--repo`
- ModelScope: `modelscope--models--org--repo`

This prevents collisions while allowing existing snapshot resolution, deletion, and cache cleanup code to stay shared.

Hugging Face uses the immutable commit SHA returned by the Hub. ModelScope normally downloads a mutable branch/tag such as `master`; Lemonade therefore derives `snapshot-id` from the normalized file tree. The fingerprint changes when paths, sizes, or available hashes change, allowing update checks to work without pretending that the branch name is immutable. A branch can still move while a download is in progress because ModelScope does not provide the same commit-pinned download contract as Hugging Face.

## Authentication and endpoints

- Hugging Face: `HF_TOKEN`, optional `HF_ENDPOINT`
- ModelScope: `MODELSCOPE_API_TOKEN` (or compatibility alias `MODELSCOPE_ACCESS_TOKEN`), optional `MODELSCOPE_ENDPOINT`

ModelScope requests send both Bearer authorization and the `m_session_id` cookie used by its legacy model endpoints.

## v1 product scope

The CLI and API support repository IDs, variants, and ModelScope model URLs. The desktop manual-registration form exposes a source selector and source-aware model links. The desktop marketplace search remains Hugging Face-only in v1; server-side multi-registry search can be introduced later without changing persisted model records or cache semantics.
