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
| `min_chars` / `max_chars` | Input length in UTF-8 bytes. With opt-in [`routing.momentum`](#momentum-routingmomentum), compares the momentum-filtered effective length instead of the raw last turn. |
| `has_tools` / `has_images` | Boolean — request carries tools / images. |
| `min_total_chars` / `max_total_chars` | Length of **all** text in the request, in UTF-8 bytes. |
| `has_tools` / `has_images` | Boolean - request carries tools / images. |
| `metadata` | `{ key, equals \| any \| exists }` over the request's OpenAI `metadata`. |

The text conditions above - `keywords_any` / `keywords_all`, `regex`, and the
`chars` pair - see only the **routing input**: the last user message (or the
`prompt` / `input` text). `total_chars` instead sums every text part the
request carries - all roles of a chat `messages` array, all items of a
Responses `input` array - so a rule can select on conversation size rather
than the size of the latest turn:

```json
{ "id": "long-conversation", "match": { "min_total_chars": 60000 }, "route_to": "cloud-model" }
```

That rule fires on a long history even when the final message is `"go on"`,
which `min_chars` would see as 5 bytes. Bounds are inclusive and counted in
bytes; images and audio contribute nothing. Sizing a bound against a context
window, estimate ~4 bytes per token.

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

## Momentum (`routing.momentum`)

By default, `min_chars`/`max_chars` measure only the **latest user turn**. In a
real conversation that turn's length swings wildly — a long coding question
escalates to a big model via `min_chars`, then the follow-up "yes, do it" (a
handful of bytes) falls straight back to `default_model` **mid-task**. Every
such flap pays a real switch cost (model load/eviction, cache miss) and tends
to hurt multi-turn quality.

`routing.momentum` fixes this by replacing the raw last-turn length with a
momentum-filtered **effective length**, recomputed fresh from that request's
own message history — there is no server-side session state; the whole
conversation is already resent on every turn, exactly like any OpenAI-style
chat client already does.

```json
"routing": {
  "momentum": { "enabled": true, "attack": 1.0, "release": 0.3 }
}
```

| Field | Required | Notes |
|-------|----------|-------|
| `enabled` | yes (once the block is authored) | Turns momentum on for every `min_chars`/`max_chars` condition in this policy. |
| `attack` | no | `(0, 1]`. Weight applied when a turn's length *rises*. Default `1.0` — a sudden long/complex turn escalates immediately. |
| `release` | no | `(0, 1]`. Weight applied when a turn's length *falls*. Default `0.3` — a short follow-up only decays the effective length by 30% per turn, so the conversation stays on the escalated route for several turns before de-escalating. |

Momentum applies **only** to `min_chars`/`max_chars`; `keywords_any`/`keywords_all`,
`regex`, `metadata`, and classifier conditions are unaffected. Absent, or
`enabled: false`, is behavior-identical to a policy without this block.

**The formula.** Given the UTF-8 byte length of every user turn in the request,
oldest-first, `c_1 .. c_n`: `m_1 = c_1`, and for `i > 1`,

```
m_i = m_{i-1} + attack  * (c_i - m_{i-1})   when c_i >= m_{i-1}  (rising)
m_i = m_{i-1} + release * (c_i - m_{i-1})   when c_i <  m_{i-1}  (falling)
```

`min_chars`/`max_chars` then compare their threshold against `m_n` instead of
the raw last-turn length. `attack = release = 1.0` degenerates exactly to
today's behavior (`m_n == c_n`).

Worked example: turns of 100, 1000, then 50 bytes, with `attack: 0.5` /
`release: 0.25`: `m_1 = 100`, `m_2 = 100 + 0.5*(1000-100) = 550`,
`m_3 = 550 + 0.25*(50-550) = 425`. A `min_chars: 400` rule still matches on the
425-byte effective length even though the raw last turn (50 bytes) alone would
miss it.

**Trace.** When momentum is active, the `min_chars`/`max_chars` trace entry
keeps its usual `condition` name and additionally carries a `rationale` string,
e.g. `"effective_chars=425.0 raw_chars=50"` — so `route_trace: true` explains
why a short turn still matched a long-input rule (see
[The decision on the response](#the-decision-on-the-response)).

**Non-goals.** Momentum is stateless and request-scoped by design: it does not
add a server-side session store, decision cache, or client headers. It does not
extend to classifier-scored conditions (a classifier-momentum variant is future
work), and it does not add a separate `threshold`/`switch_margin` knob — in
Lemonade's rule engine the thresholds already live in each rule's
`min_chars`/`max_chars`. Token-based length (`min_tokens`/`max_tokens`) remains
a deferred, separately named future addition — never a redefinition of
`min_chars`/`max_chars` or of momentum's byte-based filter.

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

## Testing a policy before registering it

[`POST /v1/routing/validate`](../api/lemonade.md#post-v1routingvalidate) evaluates a
policy document against a prompt and returns the decision plus its full trace,
without registering the policy or dispatching the user request to the selected
candidate. Component names are accepted without resolving against the model
registry, so a policy can be iterated on before its candidates are downloaded.
The endpoint performs structural policy validation, but it does not run the
registry-backed model-capability checks that registration performs. Model-backed
routing conditions can still invoke classifier, embedding, or LLM helper models
while the decision is evaluated.

```bash
curl -X POST http://localhost:13305/api/v1/routing/validate \
     -H "Content-Type: application/json" \
     -d '{"policy": {...}, "prompt": "please write a def to reverse a list"}'
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
