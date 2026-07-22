# Lemonade Router demo

Route one client request to different candidate models by policy, and see *why*
each request went where it did. The policy is a `collection.router` collection;
naming it in the OpenAI `model` field triggers the routing engine (no `/v1/route`,
no `"auto"`). Rules are evaluated top-to-bottom, first match wins, and anything
that matches nothing falls open to `default_model`.

## Demo A — both candidates local

Two local llama.cpp models: `Qwen3-0.6B-GGUF` (default / general) and
`Qwen3-4B-GGUF` (capable / coding + long-context). Model choice is arbitrary —
the point is the routing, not answer quality.

```bash
# 1. start lemond (or use your running server)
# 2. download the two candidate models
lemonade pull Qwen3-0.6B-GGUF
lemonade pull Qwen3-4B-GGUF

# 3. register the router collection
curl -X POST http://localhost:13305/api/v1/pull \
     -H "Content-Type: application/json" \
     --data-binary @examples/router/policy_local.json

# 4. run the demo
python examples/router/demo.py --model user.Demo-Router-Local
```

Four prompts exercise the policy:

| prompt | matches | routes to |
|--------|---------|-----------|
| casual question | nothing → default | `Qwen3-0.6B-GGUF` |
| "write a Python **function** …" | `keywords_any` | `Qwen3-4B-GGUF` |
| ~4500-char prompt | `min_chars >= 4000` | `Qwen3-4B-GGUF` |
| coding prompt **+ `metadata.consent = denied`** | privacy rule (first match) | `Qwen3-0.6B-GGUF` |

The last one is the interesting case: the coding rule *would* send it to the big
model, but the earlier `sensitive-stays-local` rule wins first-match, so the
request never leaves the small local model.

## Demo B — one local, one on fireworks.ai

Same policy shape, but the capable candidate is a cloud model. Install the
provider **before** registering the collection (cloud models only appear in the
catalog after install + auth, so the collection won't resolve otherwise):

```bash
export LEMONADE_FIREWORKS_API_KEY=fw-XXXX
lemonade cloud install fireworks --base-url https://api.fireworks.ai/inference/v1
lemonade list | grep fireworks          # pick a model id, e.g. fireworks.kimi-k2p5
# edit policy_cloud.json so the capable candidate is that fireworks.<id>
curl -X POST http://localhost:13305/api/v1/pull \
     -H "Content-Type: application/json" \
     --data-binary @examples/router/policy_cloud.json
python examples/router/demo.py --model user.Demo-Router-Cloud
```

Now coding / long prompts are answered by Fireworks (route category `cloud`)
while casual and `consent:denied` prompts stay local — the local/cloud split is
entirely policy-driven.

## How the decision is reported

- Response header `x-lemonade-route` → the matched rule id (or `default`).
- With `route_trace: true` in the request, the response body carries
  `x_lemonade_route`: `{ route_to, matched_rule, default_used, outputs, trace[] }`.
  `route_to` is the candidate that actually answered.
