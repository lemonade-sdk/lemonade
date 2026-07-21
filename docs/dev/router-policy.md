# Router Policies (`collection.router`)

The Lemonade **Router** selects a model per request from a policy you author. The
policy is a `collection.router` collection (a sibling of `collection.omni`).
Pointing the OpenAI `model` field at the collection's name triggers the routing
engine — there is no `/v1/route` endpoint and no `"auto"` model. Rules are
evaluated top-to-bottom, **first match wins**, and any request that matches
nothing **falls open** to `default_model`.

The engine does pure model selection and emits a decision trace. It contains no
policy-domain logic (no "PII", no "block"); anything domain-specific is expressed
on top via each rule's `outputs` pass-through bag and the request `metadata`.

## Policy shape

```json
{
  "version": "1",
  "model_name": "user.My-Router",
  "recipe": "collection.router",
  "components": ["Small-GGUF", "Big-GGUF"],
  "routing": {
    "candidates": ["Small-GGUF", "Big-GGUF"],
    "default_model": "Small-GGUF",
    "rules": [
      {
        "id": "sensitive-stays-local",
        "match": { "metadata": { "key": "consent", "equals": "denied" } },
        "route_to": "Small-GGUF",
        "outputs": { "reason": "privacy" }
      },
      {
        "id": "coding-or-long-to-big",
        "match": {
          "any": [
            { "keywords_any": ["def ", "function", "stack trace"] },
            { "min_chars": 4000 }
          ]
        },
        "route_to": "Big-GGUF"
      }
    ]
  }
}
```

| Field | Required | Notes |
|-------|----------|-------|
| `version` | yes | Must be `"1"`. |
| `recipe` | yes | Must be `"collection.router"`. |
| `components` | yes | Registered model names this policy may use. Each must already be pulled/registered (or a cloud model that has been discovered — see below). |
| `routing.candidates` | yes | The models the router may pick. Each is a `components` member. |
| `routing.default_model` | yes | Fallback when no rule matches. Must be one of `candidates`. |
| `routing.rules` | yes* | Ordered; first match wins. (*a policy provides **either** `routing.rules` **or** a `routing.router` block — see [LLM-as-router](#llm-as-router-routingrouter).) |
| `routing.classifiers` | no | Model-backed classifier definitions referenced by `classifier` / `semantic_similarity` conditions. |

A **rule** is `{ id, match, route_to, outputs? }`. `route_to` must be one of
`candidates`. `outputs` is an arbitrary object copied verbatim into the decision
(the engine never interprets it).

## Conditions

A `match` is a match-expression. Combine with the logical operators `any` (OR),
`all` (AND), and `not`; a leaf object with several keys is an implicit `all`.

**Deterministic (no model needed):**

| Condition | Meaning |
|-----------|---------|
| `keywords_any` / `keywords_all` | Case-insensitive substring match over the input. |
| `regex` | ECMAScript regex over the input. |
| `min_chars` / `max_chars` | Input length in UTF-8 bytes. |
| `has_tools` / `has_images` | Boolean — request carries tools / images. |
| `metadata` | `{ key, equals \| any \| exists }` over the request's OpenAI `metadata`. |

**Model-backed:**

| Type | Meaning |
|------|---------|
| `semantic_similarity` | Cosine similarity of the input against labelled `reference_phrases`, via an embedding model. |
| `classifier` | A `{label: score}` classifier — an encoder model via the `onnxruntime` backend (`/v1/classify`), or any model as an LLM-as-classifier via chat. |
| `llm` | An LLM picks exactly one of the declared `labels` for the request (with a rationale); the chosen label scores `1.0`, the rest `0`. You supply the `model` and a `prompt` describing when to choose each label. |

A classifier condition is a band test: `{ "classifier": "<id>", "label": "<name>",
"min_score": 0.5, "max_score": 1.0 }` (omitting both bounds defaults to
`min_score: 0.5`).

Each model-backed condition references an entry defined in `routing.classifiers`,
and every entry's `model` must be one of `components`:

```json
"routing": {
  "classifiers": [
    {
      "id": "topic",
      "type": "semantic_similarity",
      "model": "nomic-embed-text-v1-GGUF",
      "reference_phrases": {
        "coding": ["write a function", "fix this bug", "refactor this code"]
      }
    },
    {
      "id": "phishing",
      "type": "classifier",
      "model": "Phishing-Email-Detection-ONNX",
      "labels": ["LABEL_0", "LABEL_1", "LABEL_2", "LABEL_3"],
      "default_label": "LABEL_1"
    },
    {
      "id": "risk",
      "type": "llm",
      "model": "Qwen3-1.7B-GGUF",
      "prompt": "Classify the request's risk for tool execution. Reply with exactly one label: SAFE or RISKY.",
      "labels": ["SAFE", "RISKY"],
      "default_label": "SAFE",
      "on_error": "match_false"
    }
  ],
  "rules": [
    { "id": "coding-to-big",  "match": { "classifier": "topic",    "label": "coding",  "min_score": 0.6 }, "route_to": "Big-GGUF" },
    { "id": "phishing-local", "match": { "classifier": "phishing", "label": "LABEL_1", "min_score": 0.5 }, "route_to": "Small-GGUF" },
    {
      "id": "risky-tool-calls-stay-local",
      "match": {
        "all": [
          { "classifier": "risk", "label": "RISKY", "min_score": 0.5 },
          { "has_tools": true }
        ]
      },
      "route_to": "Small-GGUF",
      "outputs": { "reason": "llm-judged-risky" }
    }
  ]
}
```

- `semantic_similarity` scores each concept as the max cosine similarity of the
  input against that concept's `reference_phrases` (needs an embedding model).
- `classifier` uses the model's `{label: score}` output; declare its `labels`
  (an onnxruntime encoder serves `/v1/classify`, else it runs as an
  LLM-as-classifier via chat).
- `llm` shows the request to an LLM and asks it to choose one of `labels` (the
  chosen label scores `1.0`). Because it produces a plain label, it's a
  **composable signal** — combine it with any other condition, as in
  `risky-tool-calls-stay-local` above. The engine wraps your `prompt` with the
  request context and the label set, so the prompt just needs to say when to pick
  each label; a malformed reply fails open to `default_model`.

> A `type: "llm"` classifier and the [`routing.router`](#llm-as-router-routingrouter)
> block are the two LLM forms. `routing.router` picks the final candidate itself
> and replaces rules entirely (it's shorthand for a single `llm` classifier whose
> labels are the candidate models); a `type: "llm"` classifier only produces a
> label that rules combine with any other condition.

## Registering and invoking

Register the policy like any collection — `POST /v1/pull` with the policy JSON:

```bash
curl -X POST http://localhost:13305/api/v1/pull \
     -H "Content-Type: application/json" \
     --data-binary @my-router.json
```

Then call it by name with a normal client:

```python
from openai import OpenAI
client = OpenAI(base_url="http://localhost:13305/api/v1", api_key="lemonade")
client.chat.completions.create(model="user.My-Router", messages=[...])
```

## The decision on the response

Every routed response carries the header **`x-lemonade-route`** — the matched rule
id, or `default`. Add `"route_trace": true` to the request body and the response
also carries an **`x_lemonade_route`** object:

```json
{
  "route_to": "Big-GGUF",
  "matched_rule": "coding-or-long-to-big",
  "default_used": false,
  "outputs": {},
  "trace": [
    { "condition": "metadata", "result": false },
    { "condition": "keywords_any", "result": true }
  ]
}
```

`route_to` is the candidate that actually answered. For streaming responses the
same object is attached to the first SSE event as `x_lemonade_route`.

## Cloud candidates

A candidate may be a cloud model — the router derives the route category from the
candidate's recipe, so a `recipe: "cloud"` component is dispatched to its
provider. Because cloud models are only discovered after a provider is installed
and authenticated, **install/auth the provider before registering the
collection**, or the component won't resolve:

```bash
export LEMONADE_FIREWORKS_API_KEY=fw-XXXX
lemonade cloud install fireworks --base-url https://api.fireworks.ai/inference/v1
lemonade list | grep fireworks     # e.g. fireworks.kimi-k2p5
```

Then list `fireworks.<id>` in `components`/`candidates` next to a local model. A
`consent: denied` request can be kept on the local candidate while heavy work
routes to the cloud one — the split is entirely policy-driven.

See `examples/router/` for runnable local and local+cloud demos, and
`test/server_router.py` for the end-to-end tests.

The `classifier` condition runs a real encoder classifier: a model of type
`ModelType::CLASSIFICATION` (the `onnxruntime` backend, `/v1/classify`) is called
directly; any other model backing a `classifier` is used as an LLM-as-classifier
via chat. The classifier's model must be able to serve one of those paths, and
that capability is checked when the collection is registered.

## LLM-as-router (`routing.router`)

Instead of authoring rules, you can hand the decision to a small LLM. Provide a
`routing.router` block **in place of** `routing.rules` / `routing.classifiers`:

```json
"routing": {
  "candidates": ["Qwen3-8B-GGUF", "Qwen3.5-35B-A3B-GGUF"],
  "default_model": "Qwen3-8B-GGUF",
  "router": {
    "type": "llm",
    "model": "Qwen3-1.7B-GGUF",
    "prompt": "Route the request. Use Qwen3-8B-GGUF for everyday questions; use Qwen3.5-35B-A3B-GGUF for hard reasoning, coding, or long context."
  }
}
```

The router `model` must be one of `components`. At request time the engine asks it
to pick a candidate, and that desugars into the same first-match engine and
`Decision`/trace as the rule form. `routing.router.type` must be `"llm"`, and the
block is **mutually exclusive** with `routing.rules` and `routing.classifiers`.
