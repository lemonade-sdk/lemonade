# Router API

A `collection.router` model is a virtual model whose routing policy picks one of its candidate models for each request. This page covers how requests are routed and how to test a policy before registering it.

<!-- BEGIN GENERATED: router.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/routing/validate`](#post-v1routingvalidate) | Test a routing policy against a prompt without registering it |
<!-- END GENERATED: router.summary -->

## Routing (`collection.router`)
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Naming a registered `collection.router` model in the `model` field of a
[`chat/completions`](./openai.md#post-v1chatcompletions),
[`completions`](./openai.md#post-v1completions) or
[`responses`](./openai.md#post-v1responses) request triggers the routing engine: the
server picks a candidate by the policy's first-matching rule (fail-open to
`default_model`) and forwards the request to it. No dedicated endpoint or `"auto"`
model is involved.

The decision is reported on the response:

- Header **`x-lemonade-route`**: the matched rule id, or `default`.
- Request field **`route_trace: true`** adds the routing decision to the response body as
  **`x_lemonade_route`**, with the fields listed under
  [`POST /v1/routing/validate`](#post-v1routingvalidate)'s `decision`. For streaming
  responses it is attached to the first SSE event.

See [Router Policies](../dev/router-policy.md) for authoring the policy.

<!-- BEGIN GENERATED: router.routing_validate -->
## `POST /v1/routing/validate`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Returns the decision a routing policy makes for a prompt, without registering the policy or sending the request to the chosen candidate. The Router Builder's **Test Prompt** tab uses it to iterate on a policy before attaching it to a `collection.router` model.

Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `policy` | Yes | A `collection.router` policy document. `model_name` is accepted but is not required for validation. See [Router Policies](../dev/router-policy.md). | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | The prompt text to route. Defaults to `""`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_images` | No | Simulate a request carrying image input. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_tools` | No | Simulate a request carrying tool definitions. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `metadata` | No | String-valued metadata pairs matched by `metadata` conditions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

Validation is structural: every `candidates` entry, `default_model`, rule `route_to` and classifier model must be listed in `components`. Component names are not looked up in the model registry, so a policy can be tested before its candidates are downloaded, and registration's model-type checks, such as whether a `semantic_similarity` model can embed, do not run.

Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, etc.) are evaluated locally. Model-backed conditions (`semantic_similarity`, `classifier` and `llm`, including `routing.router`) may load and run their models. A model failure follows the classifier's `on_error` policy (`match_false` by default), so routing continues to a later rule or to `default_model`.

The example below uses explicit `routing.rules`, so its `normalized_policy` comes back unchanged. The `routing.router` shorthand is expanded into an `llm` classifier plus one rule per candidate, so a `routing` block written as:

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

comes back with `router` replaced by these `classifiers` and `rules`:

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

Match `decision.matched_rule` against this document: a policy written with only `routing.router` has no rules of its own, only the generated `__route_0`, `__route_1`, etc.

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

- `decision` - The routing decision, shaped like the `x_lemonade_route` object a routed completion returns with `route_trace: true`.
- `normalized_policy` - The policy as evaluated, with any `routing.router` shorthand expanded.
<!-- END GENERATED: router.routing_validate -->
