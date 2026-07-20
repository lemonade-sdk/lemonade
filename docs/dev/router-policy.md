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
| `routing.rules` | yes* | Ordered; first match wins. (*`routing.rules` **or** the reserved `routing.router` block is required.) |
| `routing.classifiers` | no | Model-backed classifiers referenced by `classifier` conditions. |

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

A classifier condition is a band test: `{ "classifier": "<id>", "label": "<name>",
"min_score": 0.5, "max_score": 1.0 }` (omitting both bounds defaults to
`min_score: 0.5`).

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

## Not yet implemented

- The `routing.router` sugar and the `type: "llm"` classifier (LLM-as-router) are
  reserved but not implemented; the parser rejects them.
