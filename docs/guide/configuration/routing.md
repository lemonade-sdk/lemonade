# Model Routing

Lemonade can expose model-router aliases as OpenAI-compatible model IDs. A client sends a normal request to a router model, Lemonade chooses a concrete target model, rewrites the request, and then uses the existing local or cloud backend path.

Router aliases are configured in `routers.json` in the Lemonade cache directory, next to `config.json`.

Examples:

- Linux package install: `/var/lib/lemonade/.cache/lemonade/routers.json`
- Standalone `lemond`: `~/.cache/lemonade/routers.json`
- Explicit cache dir: `<cache_dir>/routers.json`

If `routers.json` changes while `lemond` is running, Lemonade reloads it on the next router lookup or `/models` request.

## Router Models

Each router appears in `/v1/models` as a synthetic model:

```json
{
  "id": "router.example.heuristic-qwen35",
  "object": "model",
  "owned_by": "lemonade",
  "recipe": "router",
  "downloaded": true,
  "labels": ["router", "heuristic"]
}
```

Use the router ID anywhere you would normally use a model name:

```bash
curl -X POST http://localhost:13305/v1/chat/completions \
  -H "Content-Type: application/json" \
  -d '{
    "model": "router.example.heuristic-qwen35",
    "messages": [{"role": "user", "content": "Debug this CMake error"}]
  }'
```

Lemonade adds route metadata headers to routed responses:

| Header | Meaning |
|---|---|
| `X-Lemonade-Router` | Router alias used by the client |
| `X-Lemonade-Router-Type` | `heuristic` or `agentic` |
| `X-Lemonade-Routed-Model` | Concrete model selected by the router |
| `X-Lemonade-Route-Rule` | Heuristic rule ID, when applicable |
| `X-Lemonade-Route-Reason` | Short routing reason |

## Dry Run

Use `/v1/router/evaluate` to test a routing decision without loading the selected target model:

```bash
curl -X POST http://localhost:13305/v1/router/evaluate \
  -H "Content-Type: application/json" \
  -d '{
    "router": "router.example.heuristic-qwen35",
    "endpoint": "chat.completions",
    "request": {
      "messages": [{"role": "user", "content": "Debug this Python stack trace"}]
    }
  }'
```

Example response:

```json
{
  "object": "router.evaluation",
  "resolved_model": "Qwen3.5-35B-A3B-GGUF",
  "decision": {
    "routed": true,
    "router": "router.example.heuristic-qwen35",
    "type": "heuristic",
    "original_model": "router.example.heuristic-qwen35",
    "selected_model": "Qwen3.5-35B-A3B-GGUF",
    "rule": "coding",
    "reason": "matched heuristic rule: coding"
  }
}
```

## Heuristic Router

Heuristic routers evaluate rules in order. The first matching rule wins. If no rule matches, Lemonade routes to `default_model`.

```json
{
  "version": 1,
  "routers": [
    {
      "id": "router.example.heuristic-qwen35",
      "type": "heuristic",
      "description": "Routes simple requests to 4B and harder requests to 35B-A3B.",
      "endpoints": ["chat.completions", "completions", "responses"],
      "default_model": "Qwen3.5-4B-GGUF",
      "recommended_max_loaded_models": 1,
      "candidates": [
        {
          "model": "Qwen3.5-4B-GGUF",
          "description": "Fast local default for short chat and ordinary requests."
        },
        {
          "model": "Qwen3.5-35B-A3B-GGUF",
          "description": "Larger model for coding, debugging, difficult reasoning, tools, and longer prompts."
        }
      ],
      "rules": [
        {
          "id": "coding",
          "match": {
            "regex": "\\b(code|debug|stack trace|compile|cmake|python|c\\+\\+)\\b"
          },
          "route_to": "Qwen3.5-35B-A3B-GGUF"
        },
        {
          "id": "long-context",
          "match": {
            "min_chars": 4000
          },
          "route_to": "Qwen3.5-35B-A3B-GGUF"
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
      "id": "router.example.agentic-qwen35",
      "type": "agentic",
      "description": "Uses Qwen3.5 35B-A3B as a router model and routes simple work down to 4B.",
      "endpoints": ["chat.completions", "completions", "responses"],
      "router_model": "Qwen3.5-35B-A3B-GGUF",
      "default_model": "Qwen3.5-35B-A3B-GGUF",
      "recommended_max_loaded_models": 2,
      "max_decision_tokens": 128,
      "temperature": 0,
      "on_failure": "default",
      "candidates": [
        {
          "model": "Qwen3.5-4B-GGUF",
          "description": "Fast local target for short chat, direct questions, summaries, and routine tasks."
        },
        {
          "model": "Qwen3.5-35B-A3B-GGUF",
          "description": "Router and high-quality target for coding, debugging, tool use, architecture, hard reasoning, long context, or high-value answers."
        }
      ],
      "system_prompt": "You are a routing classifier for Lemonade. Choose exactly one model from the candidate list. Route to Qwen3.5-4B-GGUF only for clearly simple, short, routine, low-risk requests where latency matters more than quality. Keep Qwen3.5-35B-A3B-GGUF for coding, debugging, tool use, hard reasoning, architecture, long context, ambiguous requests, or requests where quality matters more than latency. Return only JSON with keys model and reason."
    }
  ]
}
```

This pattern keeps the routing decision high quality: the larger model decides whether the request is simple enough to route down to the 4B target. If the agentic router fails and `on_failure` is `default`, Lemonade uses the larger model rather than accidentally routing a hard request to the smaller model.

For local agentic routing, set:

```bash
lemonade config set max_loaded_models=2
```

This lets the router model and selected target model stay loaded at the same time. If `max_loaded_models=1`, routing still works, but the target model can evict the router model and the next routed request may need to reload it.

Cloud target models do not count against local loaded-model slots.

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

Copy one to your cache directory as `routers.json`, then call `/v1/models` or `lemonade list` to see the router alias.
