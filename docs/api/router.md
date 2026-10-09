# Router API

A `collection.router` model is a virtual model whose routing policy picks one of its candidate models for each request. This page covers how requests are routed and how to test a policy before registering it; see [Router Policies](../dev/router-policy.md) for writing the policy itself.

<!-- BEGIN GENERATED: router.summary -->
| Method | Endpoint | Description |
|--------|----------|-------------|
| `POST` | [`/v1/routing/validate`](#post-v1routingvalidate) | Evaluate an ad-hoc routing policy against a prompt without registering it |
<!-- END GENERATED: router.summary -->

## Routing Requests
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Naming a registered `collection.router` model in the `model` field of a [`chat/completions`](./openai.md#post-v1chatcompletions), [`completions`](./openai.md#post-v1completions) or [`responses`](./openai.md#post-v1responses) request triggers the routing engine: the server picks a candidate by the policy's first-matching rule (fail-open to `default_model`) and forwards the request to it. No dedicated endpoint or `"auto"` model is involved.

The decision is reported on the response:

- Header **`x-lemonade-route`**: the matched rule id, or `default`.
- Request field **`route_trace: true`** adds an **`x_lemonade_route`** object to the response body: `{ route_to, matched_rule, default_used, outputs, trace[] }` (`route_to` is the candidate that answered). For streaming responses it is attached to the first SSE event.

<!-- BEGIN GENERATED: router.routing_validate -->
## `POST /v1/routing/validate`
<sub>![Status](https://img.shields.io/badge/status-experimental-orange)</sub>

Evaluates a routing policy document against a prompt and returns the decision the engine would make, without registering the policy or dispatching the request to the selected candidate. The Router Builder's **Test Prompt** tab uses it to iterate on a policy before attaching it to a `collection.router` model.

The policy is validated structurally, as registration would: every `candidates` entry, `default_model`, rule `route_to` and classifier model must be listed in `components`. Component names are not looked up in the model registry, so a policy can be tested before its candidates are downloaded, and registration-time registry checks, such as whether a `semantic_similarity` model can embed, are not performed.

Deterministic conditions (`keywords_any`, `regex`, `min_chars`, `metadata`, ...) are evaluated locally. Model-backed conditions (`semantic_similarity`, `classifier` and `llm`, including `routing.router`) may load and run their models. A model failure is handled by the classifier's `on_error` policy (`match_false` by default), so routing continues to a later rule or to `default_model` instead of failing.

`decision` has the shape of the `x_lemonade_route` object a routed completion returns with `route_trace: true`, and always includes the trace. When no rule matches, `matched_rule` is empty, `default_used` is `true`, and `route_to` is the policy's `default_model`. `normalized_policy` is the policy as evaluated; see [Normalized Policies](#normalized-policies).

A `400` answers invalid JSON, a missing or non-object `policy`, a non-string `prompt`, non-boolean `has_images` or `has_tools`, `metadata` that is not an object of strings, or an invalid policy, whose `error` starts with `Invalid routing policy:`.

Also served at `/api/v0/routing/validate`, `/api/v1/routing/validate` and `/v0/routing/validate`. Requires `LEMONADE_API_KEY` when it is set.

### Parameters

| Parameter | Required | Description | Status |
|-----------|----------|-------------|--------|
| `policy` | Yes | A `collection.router` policy document; see [Router Policies](../dev/router-policy.md). `model_name` is accepted but not required. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `prompt` | No | Prompt to route. Defaults to `""`, which still exercises `min_chars` and any prompt-independent rules. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_images` | No | Simulate a request carrying image input. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `has_tools` | No | Simulate a request carrying tool definitions. Defaults to `false`. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |
| `metadata` | No | String-valued pairs matched by `metadata` conditions. | <sub>![Status](https://img.shields.io/badge/available-green)</sub> |

### Response: `Json`

=== "PowerShell"

    ```powershell
    Invoke-WebRequest `
      -Uri "http://localhost:13305/v1/routing/validate" `
      -Method POST `
      -Headers @{ "Content-Type" = "application/json" } `
      -Body '{
          "policy": {
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
          },
          "prompt": "please write a def to reverse a list"
        }'
    ```

=== "Response"

    `200`

    ```json
    {
      "decision": {
        "default_used": false,
        "matched_rule": "code-to-big",
        "outputs": {},
        "route_to": "vllm.qwen3-32b",
        "trace": [{"condition": "keywords_any", "result": true}],
        "version": "1"
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

=== "Schema"

    ```json
    {
      "type": "object",
      "required": ["decision", "normalized_policy"],
      "properties": {
        "decision": {
          "$schema": "https://json-schema.org/draft/2020-12/schema",
          "$id": "https://lemonade-sdk.github.io/schemas/decision.schema.json",
          "title": "Lemonade route decision (x_lemonade_route)",
          "description": "The decision object the engine emits, attached additively to the chat response body as `x_lemonade_route`. Pure model selection — no verdict/route-category/action in core; those are trust-customer concerns read off `outputs`. The `x-lemonade-route` response header carries the matched rule id, or `default` when the request routed through default_model.",
          "type": "object",
          "required": ["version", "route_to", "matched_rule", "default_used", "trace"],
          "properties": {
            "default_used": {
              "description": "true when no rule matched and default_model was used (fail-open).",
              "type": "boolean"
            },
            "matched_rule": {
              "description": "Id of the rule that matched; empty when the request fell through to default_model.",
              "type": "string"
            },
            "outputs": {
              "description": "Pass-through bag from the matched rule, plus engine-attached illustrative fields. The engine never interprets trust vocabulary here. When CostServices is wired, non-empty cost metadata for the selected candidate is merged under `estimated_cost` (cost_tier, cost_input_per_million, cost_output_per_million, latency_ms_hint as available) — illustrative only, not a billing figure.",
              "type": "object"
            },
            "route_to": {
              "description": "The selected candidate; also carried by the standard `model` field.",
              "type": "string"
            },
            "trace": {
              "description": "Per-condition trace of the evaluation.",
              "type": "array",
              "items": {"$ref": "#/$defs/trace_entry"}
            },
            "version": {
              "description": "Schema major version of this decision object. This file defines version \"1\"; the engine always emits it so clients and audit sinks can branch on shape.",
              "const": "1"
            }
          },
          "additionalProperties": false,
          "$defs": {
            "trace_entry": {
              "type": "object",
              "required": ["condition", "result"],
              "properties": {
                "condition": {
                  "description": "Identifies the leaf, e.g. \"classifier:pii\" or \"keywords_any\".",
                  "type": "string"
                },
                "label": {
                  "description": "Optional; the label the classifier band tested — i.e. which candidate this score belongs to. Emitted by label-scoped bands such as the identity rules synthesized from routing.router.",
                  "type": "string"
                },
                "rationale": {
                  "description": "Optional; the classifier's short reasoning for its score. The `llm` router records the model's pick rationale here.",
                  "type": "string"
                },
                "result": {"description": "The leaf's boolean outcome.", "type": "boolean"},
                "score": {
                  "description": "Present for classifier conditions; the score the band was applied to.",
                  "type": "number"
                }
              },
              "additionalProperties": false
            }
          }
        },
        "normalized_policy": {
          "description": "The policy as evaluated, with routing.router desugared into explicit classifiers and rules.",
          "type": "object"
        }
      }
    }
    ```
<!-- END GENERATED: router.routing_validate -->

### Normalized Policies

`normalized_policy` echoes the policy as it was actually evaluated. A policy with explicit `routing.rules` comes back unchanged. The field earns its place when a policy uses the `routing.router` shorthand: that sugar is desugared into an explicit `llm` classifier plus one identity rule per candidate, so a `routing` block authored as:

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

Match `decision.matched_rule` against this document rather than the one you sent: a policy authored with only `routing.router` has no `routing.rules` of its own, only the synthesized `__route_0`, `__route_1`, ... rules shown here.
