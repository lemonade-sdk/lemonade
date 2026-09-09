# Cloud Offload

Lemonade can route inference to any OpenAI-compatible cloud provider (Fireworks, OpenAI, OpenRouter, Together, etc.) alongside locally-loaded models. Cloud-routed models show up in `/v1/models` like any other recipe, so every client connecting to your `lemond` — the desktop app, the CLI, third-party SDKs, and agents launched via `lemonade launch` — sees the same catalog without per-client configuration.

> **Status: experimental.** Cloud routing has been validated with Fireworks, OpenAI, OpenRouter, and Together. Other OpenAI-compatible providers should work; report problems with `lemond` logs and the provider's `/v1/models` response.

## Quickstart

There are two ways to authenticate. **Env vars** are preferred — they're persistent, never written to disk by `lemond`, and visible to every connecting client.

### Option A: Environment variable (recommended)

Set the provider's API key in `lemond`'s environment before starting the server:

```bash
export LEMONADE_FIREWORKS_API_KEY=fw-XXXXX
```

Then install the provider once:

```bash
lemonade cloud install fireworks --base-url https://api.fireworks.ai/inference/v1
```

That's it. `lemond` discovers Fireworks's chat-capable models, registers them under the `fireworks.` namespace, and surfaces them in `/v1/models`:

```bash
lemonade list | grep fireworks
```

### Option B: Runtime API key

If you don't want to set an env var (e.g., on a dev box where the key shouldn't persist), register the provider and supply the key at runtime:

```bash
lemonade cloud install fireworks --base-url https://api.fireworks.ai/inference/v1
lemonade cloud auth fireworks
# Prompts: API key for fireworks:
```

Or in one step:

```bash
lemonade cloud install fireworks \
  --base-url https://api.fireworks.ai/inference/v1 \
  --api-key fw-XXXXX
```

Runtime keys live in `lemond`'s process memory only — they're never written to disk and they vanish on restart. To make them survive a restart, switch to Option A.

### Gateways with a non-standard auth header

By default the key is sent as `Authorization: Bearer <key>`. Some gateways front an OpenAI-shaped API but expect a differently-named header, often with no prefix:

```bash
lemonade cloud install acme \
  --base-url https://gateway.example.com/v1 \
  --auth-header-name X-Api-Key \
  --auth-header-prefix ""
```

Both settings persist per provider in `config.json` and apply to discovery and every forwarded request. `lemonade cloud list` prints the header when either value isn't the default. They are sticky: a later `cloud install` for the same provider that omits these flags keeps the configured header rather than reverting to `Authorization: Bearer`.

### Providers that speak the Anthropic Messages format

Some providers serve their models over the Anthropic Messages wire format rather than OpenAI chat completions. Register those with `--wire-format anthropic`:

```bash
lemonade cloud install acme \
  --base-url https://gateway.example.com/v1 \
  --wire-format anthropic \
  --auth-header-name x-api-key \
  --auth-header-prefix ""
```

The wire format and the auth header are independent settings, but usually go together: the Anthropic API authenticates with `x-api-key`, so leaving the header at its default returns `401`. Omit the auth flags only for a gateway that fronts the Anthropic format behind bearer auth.

Every request to an `anthropic` provider, discovery included, carries `anthropic-version: 2023-06-01` unless the client sends its own. `anthropic-beta` is forwarded from the client, so SDK feature opt-ins reach the provider.

Discovery is unchanged — `GET <base_url>/models`, which these providers serve in the OpenAI envelope. Inference differs:

- **`POST /v1/messages`** relays to `<base_url>/messages` byte-for-byte, rewriting only the `model` field. Nothing is converted, so thinking blocks, tool use, cache control, and future Anthropic fields pass through intact. Streaming relays the upstream SSE unmodified. `retry-after`, `request-id`, and `anthropic-ratelimit-*` response headers are relayed back. A non-streaming request reports the upstream status code directly; with `stream: true` the response has already begun by the time upstream's status is known, so an upstream error arrives as an SSE `error` event on a `200` and the status code carries no signal.
- **`POST /v1/chat/completions` and `POST /v1/completions`** are refused, since the provider does not serve the OpenAI shape. A non-streaming request gets `400` pointing at `/v1/messages`; a `stream: true` request gets the same message as an SSE error frame on a `200`, because the response has already begun by the time the body is written.

Relayed requests take no router slot — there is no local model to load — so they do not appear in `/v1/stats`.

## Using cloud models

Cloud-discovered models use a dot-namespaced name: `<provider>.<upstream-id>`. For example, after installing Fireworks you'll see entries like:

```
fireworks.accounts/fireworks/models/kimi-k2p5
fireworks.qwen3-235b-a22b
```

They work everywhere a local model name works:

```bash
lemonade load fireworks.kimi-k2p5
lemonade run fireworks.kimi-k2p5
```

Standard OpenAI chat completions:

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "fireworks.kimi-k2p5",
    "messages": [{"role": "user", "content": "hi"}]
  }'
```

No special headers, no per-request credentials — `lemond` resolves the key from its registry and forwards the request transparently.

## Authentication precedence

When `lemond` needs an API key for a provider, it resolves it in this order:

1. **`LEMONADE_<PROVIDER>_API_KEY` env var**, if set in `lemond`'s environment.
2. **Runtime key** from `POST /v1/cloud/auth`, if previously supplied this session.
3. **None** — `lemond` refuses to call the provider and returns a structured error.

Env vars always win. If you `POST /v1/cloud/auth` while the env var is set, the server returns **409 Conflict** with `{"error":{"type":"auth_conflict","env_var":"LEMONADE_<PROVIDER>_API_KEY"}}` and does *not* store the supplied key. This means an operator who provisions a "house" key via env can trust that a client can't silently override it.

## How discovery works

`lemond` calls `GET <base_url>/models` for each installed provider with a resolvable key, then filters the results to chat-capable models (using `supports_chat`, `capabilities`, `architecture.modality`, `type`, or id-pattern fallback depending on the provider). For each model it captures:

- **Public name** — `<provider>.<cleaned_upstream_id>` after stripping `accounts/<x>/models/` wrappers and deduplicating leading provider segments.
- **Capability labels** — `vision`, `tool-calling`, `reasoning`, normalized from each provider's divergent metadata into Lemonade's shared vocabulary.
- **Context window** — from `context_length`, when reported.
- **Per-million-token cost** — USD per 1M input/output tokens, from OpenRouter (per-token × 1e6) or Together (per-1M), when reported. Surfaced on `/v1/models` for display, and attached to `collection.router` decisions as illustrative `outputs.estimated_cost` (not a billing figure).

Discovery runs at every cache build (server startup, install, auth) and is best-effort: an unreachable provider logs a warning and is skipped without blocking the rest of the catalog.

## Admin / multi-client deployments

A single `lemond` can serve multiple connecting clients (GUI, CLI, SDKs, coding agents on the same or different machines). Cloud config is **shared infrastructure config**, not per-client state:

- **Provider URLs** persist in `lemond`'s `config.json` under `cloud_providers`. Every connecting client sees the same list.
- **API keys** live in env vars (persistent, shared) or `lemond` process memory (ephemeral). They are **never** written to disk by `lemond`, and `GET /v1/system-info` reports auth status but never the key value.

A common admin pattern: set `LEMONADE_FIREWORKS_API_KEY` in the systemd / Docker / service environment, install the provider once, and every user pointing their client at the server gets cloud access without seeing the key.

## Troubleshooting

| Symptom | Check |
|---|---|
| Provider installed but `models_discovered: 0` in `system-info` | No resolvable key — env var missing or runtime key not POSTed. |
| `POST /v1/cloud/auth` returns 409 | Env var is set for that provider. Unset it or use the env-var value going forward. |
| Chat returns "No API key for cloud provider X" | Same as above — check `LEMONADE_<PROVIDER>_API_KEY` is exported in `lemond`'s environment, not your shell. |
| Cloud model missing from `/v1/models` | Provider doesn't expose it as chat-capable, or discovery failed. Check `lemond` logs for warnings from the `Cloud` component. |
| Chat completions returns "speaks the 'anthropic' wire format" | The provider was installed with `--wire-format anthropic`; send the request to `POST /v1/messages` instead. With `stream: true` this arrives as an SSE error frame on a `200`, not a `400`. |
| An `anthropic` provider 401s with a valid key | It likely expects `x-api-key` rather than the default `Authorization: Bearer `. Re-install with `--auth-header-name x-api-key --auth-header-prefix ""`. With `stream: true` on `/v1/messages` the `401` appears as an SSE `error` event on a `200`, so check the event body rather than the status code. |

For a structured view of every installed provider's auth state and discovered model count, hit `GET /v1/system-info` — the `cloud.providers[]` block reports `env_var_set`, `runtime_key_set`, and `models_discovered` per provider.

See also:

- [CLI reference: `cloud` subcommands](../cli.md#options-for-cloud)
- [API reference: `/v1/cloud/auth`, `/v1/install`, `/v1/uninstall`](../../api/lemonade.md)
