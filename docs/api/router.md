# Router API

A `collection.router` model is a virtual model whose routing policy picks one of its candidate models for each request. This page covers how requests are routed and how to test a policy before registering it.

<!-- BEGIN GENERATED: router.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/routing/validate`](#post-v1routingvalidate) | Evaluate an ad-hoc routing policy against a prompt without registering it |
<!-- END GENERATED: router.summary -->

## Routing (`collection.router`)
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Naming a registered `collection.router` model in the `model` field of a
`chat/completions`, `completions` or `responses` request triggers the routing engine: the
server picks a candidate by the policy's first-matching rule (fail-open to
`default_model`) and forwards the request to it. No dedicated endpoint or `"auto"`
model is involved.

The decision is reported on the response:

- Header **`x-lemonade-route`** — the matched rule id, or `default`.
- Request field **`route_trace: true`** adds an **`x_lemonade_route`** object to the
  response body: `{ route_to, matched_rule, default_used, outputs, trace[] }`
  (`route_to` is the candidate that answered). For streaming responses it is
  attached to the first SSE event.

See [Router Policies](../dev/router-policy.md) for authoring the policy.

<!-- BEGIN GENERATED: router.routing_validate -->
## `POST /v1/routing/validate`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Evaluate a routing policy document against a prompt and return the decision the
engine would make, without registering the policy or dispatching the user
request to the selected candidate. This is the endpoint behind the Router
Builder's **Test Prompt** tab: it lets a policy be iterated on before it is
attached to a `collection.router` model.

The endpoint performs parser-level structural policy validation: every
`candidates` entry, `default_model`, rule `route_to`, and classifier model must
be listed in `components`. It does not consult the live model registry:
component names are accepted as-is, so a policy can be tested before its
candidates are downloaded. Because names and component model types are not
resolved through the registry, registration-time registry checks (for example,
whether a `semantic_similarity` model can embed or a `classifier` model can
classify/chat) are not performed by this endpoint.

Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, …)
are evaluated locally. Model-backed conditions (`semantic_similarity`,
`classifier`, and `llm`, including `routing.router`) may load and run their
referenced models. A model-evaluation failure is handled by the classifier's
`on_error` policy (`match_false` by default), so routing normally continues to a
later rule or falls through to `default_model` rather than treating the policy
as invalid.

Also served at `/api/v0/routing/validate`, `/api/v1/routing/validate` and `/v0/routing/validate`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `policy` | Yes | A `collection.router` policy document. `model_name` is accepted but is not required for validation. See [Router Policies](../dev/router-policy.md). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | The prompt text to route. Defaults to `""`, which still exercises `min_chars` (0 chars) and any prompt-independent rules. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_images` | No | Simulate a request carrying image input. Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_tools` | No | Simulate a request carrying tool definitions. Default `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `metadata` | No | String-valued metadata pairs matched by `metadata` conditions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

`decision` has the same shape as the `x_lemonade_route` object a routed
completion returns with `route_trace: true`, and the trace is always included
here. When no rule matches, `matched_rule` is empty, `default_used` is `true`,
and `route_to` is the policy's `default_model`.

`normalized_policy` echoes the policy as it was actually evaluated. The policy
above uses explicit `routing.rules`, so it comes back unchanged. The field earns
its place when a policy uses the `routing.router` shorthand: that sugar is
desugared into an explicit `llm` classifier plus one identity rule per
candidate, so a `routing` block authored as:

```json
{
  "router": {
    "type": "llm",
    "model": "Qwen3-8B-GGUF",
    "prompt": "Pick the best model for this request."
  },
  "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
  "default_model": "Qwen3-8B-GGUF"
}
```

is echoed back with `router` removed and synthesized `classifiers`/`rules`:

```json
{
  "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
  "default_model": "Qwen3-8B-GGUF",
  "classifiers": [
    {
      "id": "__router",
      "type": "llm",
      "model": "Qwen3-8B-GGUF",
      "prompt": "Pick the best model for this request.",
      "labels": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"]
    }
  ],
  "rules": [
    {
      "id": "__route_0",
      "match": {"classifier": "__router", "label": "Qwen3-8B-GGUF", "min_score": 1.0},
      "route_to": "Qwen3-8B-GGUF"
    },
    {
      "id": "__route_1",
      "match": {"classifier": "__router", "label": "vllm.qwen3-32b", "min_score": 1.0},
      "route_to": "vllm.qwen3-32b"
    }
  ]
}
```

Match `decision.matched_rule` against this document rather than the one you
sent — a policy authored with only `routing.router` has no `routing.rules` of
its own, only the synthesized `__route_0`, `__route_1`, … rules shown here.

A `400` answers a body that is not valid JSON, a missing or non-object `policy`, a non-string `prompt`, non-boolean `has_images`/`has_tools`, a `metadata` that is not an object of string values, or a policy document that is invalid or internally inconsistent, whose `error` is prefixed with `Invalid routing policy:`.

### Example request

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/routing/validate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "policy": {
            "version": "1",
            "recipe": "collection.router",
            "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
            "routing": {
              "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
              "default_model": "Qwen3-8B-GGUF",
              "rules": [
                {
                  "id": "code-to-big",
                  "match": {"keywords_any": ["def ", "function", "compile"]},
                  "route_to": "vllm.qwen3-32b"
                }
              ]
            }
          },
          "prompt": "please write a def to reverse a list"
        }'
    ```

=== "Bash"

    ```bash
    curl http://localhost:13305/v1/routing/validate \
      -H "Content-Type: application/json" \
      -d '{
          "policy": {
            "version": "1",
            "recipe": "collection.router",
            "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
            "routing": {
              "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
              "default_model": "Qwen3-8B-GGUF",
              "rules": [
                {
                  "id": "code-to-big",
                  "match": {"keywords_any": ["def ", "function", "compile"]},
                  "route_to": "vllm.qwen3-32b"
                }
              ]
            }
          },
          "prompt": "please write a def to reverse a list"
        }'
    ```

### Response format

`200`

```json
{
  "decision": {
    "version": "1",
    "route_to": "vllm.qwen3-32b",
    "matched_rule": "code-to-big",
    "default_used": false,
    "outputs": {},
    "trace": [{"condition": "keywords_any", "result": true}]
  },
  "normalized_policy": {
    "components": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
    "recipe": "collection.router",
    "routing": {
      "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
      "default_model": "Qwen3-8B-GGUF",
      "rules": [
        {
          "id": "code-to-big",
          "match": {"keywords_any": ["def ", "function", "compile"]},
          "route_to": "vllm.qwen3-32b"
        }
      ]
    },
    "version": "1"
  }
}
```

**Field Descriptions:**

- `decision` - The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.
<!-- END GENERATED: router.routing_validate -->
