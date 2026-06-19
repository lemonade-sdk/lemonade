# Model Routing

Lemonade can expose model-router aliases and a `/v1/route` endpoint. A client asks Lemonade for a route decision, receives the selected concrete model plus a rewritten request, and then sends that request to the normal OpenAI-compatible endpoint.

Router aliases are configured in `routers.json` in the Lemonade cache directory, next to `config.json`.

Examples:

- Linux package install: `/var/lib/lemonade/.cache/lemonade/routers.json`
- Standalone `lemond`: `~/.cache/lemonade/routers.json`
- Explicit cache dir: `<cache_dir>/routers.json`

If `routers.json` changes while `lemond` is running, Lemonade reloads it on the next router lookup or `/routers` request.

## Router Discovery

Routers are separate from models. List them with `/v1/routers`:

```bash
curl http://localhost:13305/v1/routers
```

Each router entry looks like:

```json
{
  "id": "router.example.heuristic-qwen35-fireworks",
  "type": "heuristic",
  "default_model": "Qwen3.5-35B-A3B-GGUF",
  "recommended_max_loaded_models": 1,
  "candidates": [
    {"model": "Qwen3.5-35B-A3B-GGUF"},
    {"model": "fireworks.kimi-k2p6"}
  ]
}
```

Use that router ID with `/v1/route`:

```bash
curl -X POST http://localhost:13305/v1/route \
  -H "Content-Type: application/json" \
  -d '{
    "router": "router.example.heuristic-qwen35-fireworks",
    "endpoint": "chat.completions",
    "request": {
      "messages": [{"role": "user", "content": "Debug this CMake error"}]
    }
  }'
```

You can also put the router ID in the path:

```bash
curl -X POST http://localhost:13305/v1/route/router.example.heuristic-qwen35-fireworks \
  -H "Content-Type: application/json" \
  -d '{
    "endpoint": "chat.completions",
    "request": {
      "messages": [{"role": "user", "content": "Debug this CMake error"}]
    }
  }'
```

`/v1/route` returns a decision and a rewritten request:

```json
{
  "object": "route.decision",
  "endpoint": "chat.completions",
  "router": "router.example.heuristic-qwen35-fireworks",
  "selected_model": "fireworks.kimi-k2p6",
  "request": {
    "model": "fireworks.kimi-k2p6",
    "messages": [{"role": "user", "content": "Debug this CMake error"}]
  },
  "decision": {
    "routed": true,
    "fallback": false,
    "router": "router.example.heuristic-qwen35-fireworks",
    "type": "heuristic",
    "selected_model": "fireworks.kimi-k2p6",
    "rule": "coding",
    "reason": "matched heuristic rule: coding"
  }
}
```

Then send the returned `request` object to the endpoint named by `endpoint`, for example `/v1/chat/completions`.

When a routed request is executed through Lemonade compatibility paths, Lemonade also adds route metadata headers:

| Header | Meaning |
|---|---|
| `X-Lemonade-Router` | Router alias used by the client |
| `X-Lemonade-Router-Type` | `heuristic` or `agentic` |
| `X-Lemonade-Routed-Model` | Concrete model selected by the router |
| `X-Lemonade-Route-Rule` | Heuristic rule ID, when applicable |
| `X-Lemonade-Route-Reason` | Short routing reason |

Lemonade also records routed traffic in its serving telemetry:

- `GET /v1/stats` includes a nested `routing` object with totals and breakdowns.
- `GET /metrics` exports Prometheus counters with the `lemonade_router_` prefix.

## Heuristic Router

Heuristic routers evaluate rules in order. The first matching rule wins. If no rule matches, Lemonade routes to `default_model`.

```json
{
  "version": 1,
  "routers": [
    {
      "id": "router.example.heuristic-qwen35-fireworks",
      "type": "heuristic",
      "description": "Routes ordinary requests to local Qwen3.5 35B-A3B and escalates harder cloud-appropriate requests to Fireworks Kimi K2.6.",
      "endpoints": ["chat.completions", "completions", "responses"],
      "default_model": "Qwen3.5-35B-A3B-GGUF",
      "recommended_max_loaded_models": 1,
      "candidates": [
        {
          "model": "Qwen3.5-35B-A3B-GGUF",
          "description": "Local default for ordinary requests and privacy-sensitive work."
        },
        {
          "model": "fireworks.kimi-k2p6",
          "description": "Remote Fireworks target for harder reasoning, tool-heavy work, vision, or very long context."
        }
      ],
      "rules": [
        {
          "id": "coding",
          "match": {
            "regex": "\\b(code|debug|stack trace|compile|cmake|python|c\\+\\+)\\b"
          },
          "route_to": "fireworks.kimi-k2p6"
        },
        {
          "id": "long-context",
          "match": {
            "min_chars": 4000
          },
          "route_to": "fireworks.kimi-k2p6"
        }
      ]
    }
  ]
}
```

Supported match fields:

| Field | Meaning |
|---|---|
| `keywords_any` | Match if any keyword appears in the normalized prompt text |
| `keywords_all` | Match only if every keyword appears |
| `regex` | Case-insensitive ECMAScript regular expression |
| `min_chars` | Match prompt text at or above this character count |
| `max_chars` | Match prompt text at or below this character count |
| `has_tools` | Match whether the request includes OpenAI tool definitions |
| `has_images` | Match whether the request includes image content parts |
| `any` | Match if any nested condition matches |
| `all` | Match if every nested condition matches |
| `not` | Invert a nested condition |

## Agentic Router

Agentic routers call a router model first, then validate the model it selects. The router model must return JSON with a `model` field matching one of the configured candidates.

Lemonade disables thinking for the internal router decision call so the router returns parseable JSON instead of a reasoning trace. This does not change the final routed request: the target model still follows the client's `enable_thinking` / `thinking` setting.

```json
{
  "version": 1,
  "routers": [
    {
      "id": "router.example.agentic-qwen35-fireworks",
      "type": "agentic",
      "description": "Uses local Qwen3.5 35B-A3B as a router model and escalates cloud-appropriate work to Fireworks Kimi K2.6.",
      "endpoints": ["chat.completions", "completions", "responses"],
      "router_model": "Qwen3.5-35B-A3B-GGUF",
      "default_model": "Qwen3.5-35B-A3B-GGUF",
      "recommended_max_loaded_models": 1,
      "max_decision_tokens": 128,
      "temperature": 0,
      "on_failure": "default",
      "candidates": [
        {
          "model": "Qwen3.5-35B-A3B-GGUF",
          "description": "Local default for ordinary requests and privacy-sensitive work."
        },
        {
          "model": "fireworks.kimi-k2p6",
          "description": "Remote Fireworks target for harder reasoning, tool-heavy work, vision, or very long context."
        }
      ],
      "system_prompt": "You are a routing classifier for Lemonade. Choose exactly one model from the candidate list. Prefer Qwen3.5-35B-A3B-GGUF for ordinary requests, local/private work, and tasks that do not clearly need cloud offload. Choose fireworks.kimi-k2p6 for harder reasoning, very long context, tool-heavy work, vision requests, or requests where the user explicitly allows or benefits from remote cloud quality. Return only JSON with keys model and reason."
    }
  ]
}
```

This pattern keeps the routing decision local and high quality: Qwen3.5 35B-A3B decides whether the request should stay local or escalate to the Fireworks cloud model. If the agentic router fails and `on_failure` is `default`, Lemonade uses the local 35B model rather than accidentally sending a request to the cloud.

For this local-plus-cloud agentic example, `recommended_max_loaded_models` is `1`: the local Qwen3.5 35B-A3B router/default model is the only local model, and Fireworks cloud models do not count against local loaded-model slots. For routers with two local models, use `max_loaded_models=2` so the router model and selected target model can stay loaded together.

Before using a Fireworks target, install and authenticate the provider:

```bash
export LEMONADE_FIREWORKS_API_KEY=fw-...
lemonade cloud install fireworks --base-url https://api.fireworks.ai/inference/v1
```

## Failure Behavior

Agentic routers support:

| `on_failure` | Behavior |
|---|---|
| `default` | Route to `default_model` if the router model fails, returns invalid JSON, or chooses a non-candidate |
| `error` | Return a routing error |

Lemonade blocks router chaining for now: a router cannot select another router alias.

## Examples

Example router configs live in:

- `examples/routing/heuristic-qwen35-router.json`
- `examples/routing/agentic-qwen35-router.json`

Copy one to your cache directory as `routers.json`, then call `/v1/routers` or `lemonade routers list` to see the router alias.
