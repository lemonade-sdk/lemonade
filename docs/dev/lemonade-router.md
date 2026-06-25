# Lemonade Router

Lemonade Router is a generic model-selection layer. A router is distributed as a
normal Lemonade collection model with `recipe: "collection.router"`. Clients
invoke it by sending a standard OpenAI-compatible request whose `model` names the
router collection. The router evaluates the collection's policy, selects a
concrete candidate model, and dispatches the request to that model.

The router core performs pure model selection. It does not interpret trust
concepts such as warn, block, consent, or audit persistence. Policy authors can
attach arbitrary rule `outputs`; the engine copies them into the route decision
without interpreting them.

## Model Shape

A router collection is a model definition with `components` and a `routing`
block:

```jsonc
{
  "model_name": "user.Router-Shopping",
  "recipe": "collection.router",
  "checkpoint": "",
  "components": [
    "Qwen3-8B-GGUF",
    "vllm.qwen3-32b",
    "nomic-embed-text-v1.5-GGUF"
  ],
  "routing": {
    "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
    "default_model": "vllm.qwen3-32b",
    "classifiers": [
      {
        "id": "sim_returns",
        "type": "semantic_similarity",
        "model": "nomic-embed-text-v1.5-GGUF",
        "candidates": ["how do I return", "track my order"]
      }
    ],
    "rules": [
      {
        "id": "support",
        "match": {
          "classifier": "sim_returns",
          "min_score": 0.75
        },
        "route_to": "vllm.qwen3-32b"
      }
    ]
  }
}
```

`components` lists every model bundled with the router, including routing
targets and classifier models. `routing.candidates` is the explicit set of
models that rules may route to. `default_model` is the fail-open target and must
be one of the candidates.

The optional top-level `models` array is for self-contained redistribution, such
as Hugging Face collection JSONs. Local authoring usually only needs
`components` plus `routing`.

## Routing Rules

Rules are evaluated in order. The first matching rule wins:

```jsonc
{
  "id": "keep-private",
  "match": {
    "any": [
      { "classifier": "pii", "min_score": 0.5 },
      { "keywords_any": ["ssn", "credit card"] }
    ]
  },
  "route_to": "Qwen3-8B-GGUF",
  "outputs": { "verdict": "warn" }
}
```

If no rule matches, the router uses `default_model`.

`outputs` is an engine-opaque object. It can carry consumer-specific fields, but
the generic router does not interpret them.

## Match Expressions

Match expressions support logical operators:

- `any`: true when any child expression matches.
- `all`: true when every child expression matches.
- `not`: true when the child expression is false.

Supported deterministic leaves:

- `keywords_any`
- `keywords_all`
- `regex`
- `min_chars`
- `max_chars`
- `has_tools`
- `has_images`

Supported classifier leaves:

- `classifier`: classifier id.
- `label`: optional label constraint.
- `min_score`: optional score lower bound.
- `max_score`: optional score upper bound.

If a classifier condition omits both score bounds, runtime evaluation uses
`min_score: 0.5`.

Multiple leaf fields in one object are treated as an implicit `all`:

```jsonc
{
  "keywords_any": ["return", "refund"],
  "max_chars": 1000
}
```

Logical operators cannot be mixed with leaf fields in the same object. Write the
grouping explicitly instead:

```jsonc
{
  "all": [
    {
      "any": [
        { "keywords_any": ["return"] },
        { "keywords_any": ["refund"] }
      ]
    },
    { "max_chars": 1000 }
  ]
}
```

Empty match objects are invalid. Use `default_model` for fallback.

## Classifiers

Active v1 classifier types:

- `semantic_similarity`
- `classifier`

Reserved preset names:

- `pii_detection`
- `prompt_safety`
- `language_detection`
- `domain_classification`
- `complexity`
- `sentiment`

Reserved presets are schema-valid vocabulary, but runtime support can be staged.

### Semantic Similarity

`semantic_similarity` compares the request text with configured candidate
phrases using embeddings and cosine similarity:

```jsonc
{
  "id": "sim_returns",
  "type": "semantic_similarity",
  "model": "nomic-embed-text-v1.5-GGUF",
  "candidates": ["how do I return", "track my order"]
}
```

### Generic Classifier

`classifier` represents model-backed text classification. Results are modeled as
`label -> score` with scores in `[0, 1]`.

```jsonc
{
  "id": "pii",
  "type": "classifier",
  "model": "pii-detector-small",
  "labels": ["PII", "NO_PII"],
  "default_label": "PII",
  "on_error": "match_true"
}
```

If `on_error` is omitted, it defaults to `match_false`.

### LLM Router

For zero-config LLM-as-router authoring, use `routing.router`:

```jsonc
{
  "routing": {
    "candidates": ["Qwen3-8B-GGUF", "vllm.qwen3-32b"],
    "default_model": "Qwen3-8B-GGUF",
    "router": {
      "type": "llm",
      "model": "Qwen3-1.7B-GGUF",
      "prompt": "Choose the cheapest model that can handle the request."
    }
  }
}
```

`routing.router` is the only v1 authoring surface for LLM-as-router. The parser
can lower it into internal classifier/rule structures later, but explicit
`type: "llm"` entries in `routing.classifiers[]` are not part of the v1 schema.

If `routing.router` is combined with authored `rules`, parser-generated router
rules are appended after authored rules so explicit policy rules have
first-match priority.

## Request Extensions

Routing inputs ride the standard OpenAI `metadata` field:

```jsonc
{
  "model": "user.Router-Shopping",
  "messages": [{ "role": "user", "content": "..." }],
  "metadata": {
    "task_class": "payment",
    "site_tags": "shopping,checkout"
  },
  "route_trace": true
}
```

For this contract, metadata values are strings. Lists are comma-encoded strings.
Nested objects and arrays are not accepted.

`route_trace: true` opts the client into full route trace. Without it, response
attachment includes the small route object only.

## Response Attachment

When attached to a response, route information lives in `x_lemonade_route`:

```jsonc
{
  "x_lemonade_route": {
    "route_to": "Qwen3-8B-GGUF",
    "matched_rule": "keep-private",
    "default_used": false,
    "outputs": { "verdict": "warn" },
    "trace": [
      { "condition": "classifier:pii", "score": 0.81, "result": true },
      { "condition": "keywords_any", "result": false }
    ]
  }
}
```

`trace` is present only when requested with `route_trace: true`.

Fallback decisions use:

```jsonc
{
  "x_lemonade_route": {
    "route_to": "vllm.qwen3-32b",
    "matched_rule": null,
    "default_used": true,
    "outputs": {}
  }
}
```

The HTTP header `x-lemonade-route` carries the matched rule id. For fallback
decisions, the header value is `default`.

Trace entries include evaluated conditions only. Short-circuited branches are
omitted. Trace entries must not include raw user input.

## Validation Boundary

JSON Schema validates the structural contract:

- Required fields.
- Field types.
- Enum values.
- Score ranges.
- Strict classifier shapes.
- Match expression structure.

The runtime parser validates semantic cross references later:

- `default_model` is in `routing.candidates`.
- Every `route_to` is in `routing.candidates`.
- Every candidate is in `components`.
- Every classifier model is in `components`.
- Every condition classifier id exists.
- Every classifier label reference exists.
- `min_score <= max_score`.
