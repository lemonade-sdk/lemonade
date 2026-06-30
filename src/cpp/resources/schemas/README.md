# Routing engine schemas (Lemonade Router)

Frozen JSON Schemas for the generic routing engine. The engine does **pure model
selection** (boolean rules over classifiers, first-match-wins, fail-open to
`default_model`); trust-specific concerns (verdicts, block, audit, consent) are
layered on top via the `outputs` pass-through bag, request `metadata`, and the
decision `trace` — never in the engine.

| Schema | Describes |
|--------|-----------|
| `route_policy.schema.json` | The `routing` block embedded in a `collection.router` collection JSON. Invoked like `collection.omni`: point the OpenAI `model` field at the collection name. |
| `request.schema.json` | The request-side extension fields on the OpenAI chat body: `metadata` (string-valued routing inputs) and the optional `route_trace`. Validates only those fields (`additionalProperties: true`); no `version` (rides on the stock OpenAI request). |
| `decision.schema.json` | The `x_lemonade_route` decision object attached additively to the chat response. |

## Versioning

Both schemas carry a **required** root `version` field — these files define
version `"1"`. It is required (not optional) because the format is greenfield:
declaring it from the first policy avoids a breaking "add it later" migration and
lets a server branch cleanly on shape instead of guessing when the field is
absent. A server validates a document against the schema matching its major
version and rejects an unknown major with a clear message rather than a confusing
per-key error. The policy `version` is author-declared; the decision `version` is
always emitted by the engine.

Evolution rule:
- **Additive, backward-compatible data** (new keys in the open `outputs` bag or
  request `metadata`) needs no version bump — those seams are unconstrained.
- **New vocabulary** (a new condition op, classifier `type`, or decision field)
  is a deliberate schema edit; a compatible addition stays within major `1`, a
  breaking change ships a new major and a new schema file.

## Compatibility & frozen semantics

**Hard rule: a future server must never break a policy authored against an
earlier major.** This holds structurally — old policies keep validating against a
newer (superset) schema, and `additionalProperties: false` only constrains the
forward direction (an old server rejecting a *newer* policy), which `version`
handles cleanly.

Two operating principles keep it true:

1. **Never redefine a shipped field — only add.** A v1 field's meaning is
   immutable. New behavior arrives as a *new* field / op / classifier `type`
   (which old policies don't use), never as a reinterpretation of an existing
   one. The `classifier.type` enum already reserves future preset names for this
   reason.
2. **Never delete or edit a shipped major's schema + parser.** Evolution is
   additive: ship `vN+1` schemas and per-major load-time shims that upgrade older
   documents to the latest internal form; retain every prior major's schema so
   its policies still validate and route.

These are enforced mechanically, not just by convention:

- `schema-lock.json` + `test/test_schema_lock.py` pin a canonical hash of each
  schema. A **released** major is immutable — any change fails CI (ship a new
  major instead). An **unreleased** major may change, but only with a refreshed
  lock in the same diff (`python test/test_schema_lock.py --update`), so every
  schema edit is a visible, reviewed change rather than silent drift. (v1 stays
  `released: false` through implementation — the design is signed off, but the
  schema can still be refined with a reviewed lock refresh until the engine
  ships. Flip to `true` at product release, when real policies exist in the wild
  and immutability actually protects users.)
- `test/test_routing_fixtures.py` keeps the schemas self-valid and the example
  fixtures conformant.
- A frozen conformance corpus (golden policy → expected `Decision`) will enforce
  *behavioral* stability across versions; schema↔parser key parity is checked
  where the parser is built. (Both are tracked separately in the milestone.)

Lemonade executes a policy identically regardless of version — the only behavioral
drift for model-backed classifiers (semantic_similarity / classifier / llm) comes
from the **backend engine or the model**, not from lemonade. To keep that drift
from silently flipping a route, the engine-owned semantics below are **frozen for
v1** (pinned in the schema field descriptions):

| Semantic | Frozen v1 definition |
|----------|----------------------|
| `keywords_any` / `keywords_all` | case-insensitive **substring** over input text |
| `regex` | **ECMAScript** dialect (`std::regex`) |
| `min_score` / `max_score` | **inclusive** band (`>=` / `<=`); default `min_score: 0.5` when neither bound is given |
| `min_chars` / `max_chars` | input length in **UTF-8 bytes** (not code points) |
| `metadata` | reads a request `metadata` key; **case-sensitive** comparison, value decoded into a comma-split, trimmed **token set** (`equals` exact / `any` set-intersection / `exists` presence) |
| multi-key leaf object | interpreted as implicit **`all`**; e.g. `{"keywords_any":[...],"max_chars":1000}` means both leaves must match |
| `on_error` (omitted) | default **`match_false`** (fail-open) |
| `routing.router` desugaring | expansion to one `llm` classifier + identity rules is deterministic and behavior-equivalent across versions |

Anything fancier (token/BM25 keyword matching, a different regex engine,
token-based length) ships as a new, separately named op — never by changing one
of the above.

## Authoring levels (example fixtures)

Lean local-form examples — `components` reference already-registered models by
name; the full `models[]` manifest is only needed for Hugging Face
redistribution. Fixtures live in `test/cpp/fixtures/routing/`:

| Fixture | Level | Mechanism |
|---------|-------|-----------|
| `l0a_llm_router.json` | L0(a) | `routing.router` LLM-as-router (desugars to one `llm` classifier + identity rules) |
| `l1_keywords.json` | L1 | Deterministic `keywords_any` / `regex` / `min_chars` |
| `l1_metadata.json` | L1 | Deterministic `metadata` match on caller-supplied routing inputs (`task_class` / `consent`) |
| `l2_semantic.json` | L2 | `semantic_similarity` classifier (embeddings + cosine) |
| `l3_classifier.json` | L3 | Model-backed `classifier` (PII / jailbreak) |
| `decision_example.json` | — | A `Decision` + `trace` |

Levels **compose** — one policy may mix a router, classifiers, and deterministic
conditions across its rules.

## Contract surface

The C++ types/interfaces these schemas back live in
`src/cpp/include/lemon/routing_policy.h`. Validation:

- **Schema validation** — `python test/test_routing_fixtures.py` (uses `jsonschema`).
- **Contract / cross-field invariants** — `test/cpp/test_routing_policy_contract.cpp`
  (CTest target `RoutingPolicyContractTest`): default_model and every
  `route_to` must be a candidate; classifier condition refs must resolve.
