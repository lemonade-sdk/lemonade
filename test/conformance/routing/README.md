# Router conformance corpus

The routing engine takes a `collection.router` policy and a request and returns a
`Decision`: which model to use and why. This corpus is a set of recorded
policy + request → `Decision` examples. A test runner
(`test/cpp/test_routing_conformance_corpus.cpp`, CTest target
`RoutingConformanceCorpusTest`) replays every example through the real engine and
fails if the engine now answers differently.

The point is backward compatibility. A policy written for schema version 1 must
keep routing the same way on every future server. The schema files under
`src/cpp/resources/schemas/` guarantee that such a policy still *parses*; this
corpus guarantees that it still *routes* the same way.

The corpus is not yet frozen. Today it holds the engine to the expectations
committed next to it. Making version-1 cases immutable (so that editing one is a
CI failure by itself) is follow-up work.

## What is compared

The runner serializes the engine's `Decision` with the production
`route_decision_to_json` and compares it with the recorded one as parsed JSON, so
key order and formatting do not matter. It checks these fields:

| Field | Meaning |
|-------|---------|
| `version` | The decision format version, always `"1"` today. |
| `route_to` | The chosen model. |
| `matched_rule` | The id of the rule that matched, or `""` when no rule matched. |
| `default_used` | `true` when no rule matched and `default_model` was used. |
| `outputs` | The matched rule's `outputs` object, copied as is (`{}` otherwise). |
| `trace` | Only when the request sets `route_trace: true`: one entry per evaluated condition, with `condition`, `result`, and optionally `score`, `label`, `rationale`. |

Every value must match exactly, with one exception: a trace `score` may differ by
up to `1e-12`. Only the semantic-similarity score is actually computed (a cosine:
dot product, square roots, a division), and its last bits can differ between the
x86 and ARM CI runners. Classifier and LLM-router scores are copied from the stub
answers and are exact in practice, but a trace entry does not say which
classifier type produced it, so the same small margin applies to every score.
Applying it everywhere loses no coverage: a difference that small cannot move a
score across a threshold without also flipping `result`, which is compared
exactly.

A field that neither side has, or that the comparison does not know about, is a
failure too. This catches drift between the serializer and the runner.

## Layout

```
test/conformance/routing/
  README.md
  1/                  # schema major of the policies inside
    l0a/
      policies.json   # { "<policy_name>": <collection.router policy>, ... }
      cases.jsonl     # one case per line, each naming the policy it runs against
    l1/
    l2/
    l3/
```

One directory per schema major, one directory per tier inside it, exactly two
files per tier. The runner enforces this layout strictly. Any stray file, extra
directory, missing file, policy name declared twice in the same `policies.json`,
policy whose `version` does not match the directory name, policy no case uses, or
case naming an unknown policy is a hard failure.
Strictness is deliberate: a corpus that silently loses cases is worse than one
that fails.

The `version` of a policy is the schema major of the *policy* format. The
`version` inside a recorded `Decision` is the major of the *decision* format.
Both are `1` today, but they are versioned separately.

### Tiers

The tiers follow the four ways a policy can decide, from the cheapest to the most
model-dependent. `l1` runs with no model at all; the other three run against a
fake backend whose answers each case declares (see
[Stub answers](#stub-answers-for-model-backed-cases)).

| Tier | Engine part | Needs stubs |
|------|-------------|-------------|
| `l0a` | The `llm` router: a chat model picks a candidate. Both the `routing.router` shorthand and the explicit classifier + rules form. | yes (`chat`) |
| `l1` | Deterministic conditions (`keywords_*`, `regex`, `*_chars`, `has_tools`, `has_images`, `metadata`), `any` / `all` / `not`, rule order, request parsing, trace shape. | no |
| `l2` | The `semantic_similarity` classifier: embeddings and cosine scores. | yes (`embed`) |
| `l3` | The `classifier` type: label scores from a classification model, score bands, `on_error`. | yes (`run_classifier`) |

A tier holds several policies. A new policy is added only when a behavior cannot
be shown with an existing one (for example, `conditions_features` and
`conditions_features_negated` must route the same request to opposite models).
Anything the request can vary (`route_trace`, the text, `metadata`, the stub
answers) is a new case against an existing policy, not a new policy.

## Case format

Each non-blank line of `cases.jsonl` is one JSON object:

| Field | Meaning |
|-------|---------|
| `case_name` | Case id, unique within its policy. No `/`, `:` or whitespace. |
| `policy_name` | Key in this tier's `policies.json`. |
| `request` | The request body, in any form `build_route_context` accepts: chat (`messages`), legacy completions (`prompt`) or Responses (`input`), plus optional `model`, `metadata`, `tools`, `route_trace`. |
| `decision` | The exact `Decision` the engine must produce. |
| `services` | Model-backed cases only: the fake backend's answers (below). |
| `note` | Optional. Why the decision is what it is. Ignored by the runner. |

Any other key fails the case. The runner reports a case as
`<major>/<tier>::<policy_name>::<case_name>`, for example
`1/l3::classifier_band::band-inside`.

A `note` is strongly encouraged. The best notes say which alternative behavior
would have produced a different decision, so a reader knows what the case
protects against.

## Stub answers for model-backed cases

No real model runs. The runner binds each engine to a `FakeClassifierServices`
and fills it from the case's `services` object:

```
"services": {
  "embed":          { "<model>": { "<text>": [numbers] | null } },
  "run_classifier": { "<model>": { "<label>": number } | null },
  "chat":           { "<model>": "<reply>" | null }
}
```

- `embed` (used by `semantic_similarity`) is keyed by the exact text embedded:
  every reference phrase of the classifier and the routing input itself.
- `run_classifier` (used by `classifier`) returns a label → score map. An empty
  map `{}` is a *successful* answer where every label scores 0.
- `chat` (used by the `llm` router) returns the model's raw reply text.
- `null` makes that call fail. The classifier then reports failure and its rule
  resolves through `on_error` (`match_false` by default, `match_true` if set).

The fake is strict in both directions. Every backend call a case triggers must
have a stub answer; an unstubbed call fails the case even if the decision
happened to match. If a classifier should score low, write the low score
explicitly (`{"code": 0.0}`). A stub the case declares but no call uses fails it
too, so a typo'd model or phrase, or an answer left over from an earlier version
of the case, cannot sit there looking like coverage. Declare exactly the answers
the case needs.

Two details for `embed` authors:

- **Compute expected cosines from float32 values.** Vectors are stored as 32-bit
  floats, so `0.1` is rounded to float32 before the cosine (accumulated in
  double). A cosine computed from the JSON decimals in full double precision can
  miss the `1e-12` margin.
- **Each case declares all of its phrases.** The engine is rebuilt per case, so a
  `semantic_similarity` classifier re-embeds its reference phrases every time.
  Cases sharing a policy may use different vectors for the same phrase.

Model names in the corpus (`RouterLLM`, `EmbedModel`, `TopicModel`, ...) are
placeholders. Nothing is loaded; a name is only the key the stub answers are filed
under.

## Adding a case

1. Pick the tier and an existing policy. Add a policy only if no existing one can
   show the behavior.
2. Append one line to `cases.jsonl`. Give it a `note`.
3. Check that the case would fail if the behavior changed: temporarily change the
   engine, run the corpus, watch the case go red, revert.
4. Add a row to the coverage tables below.

```
cmake --build build --target test_routing_conformance_corpus
./build/test_routing_conformance_corpus
```

The row-level checks of the runner (allowed keys, names, decision comparison)
have their own unit test, `test/cpp/test_conformance_row_checks.cpp`
(CTest target `ConformanceRowChecksTest`).

## Coverage

One behavior per row, one case per behavior. Combinators and rule resolution are
op-agnostic, so they are checked once, not for every condition type. Each table
is one policy; its heading says how the policy is set up, and the case names are
relative to it. Unless a row says otherwise, "default" means no rule matched and
the decision is `default_model` with `matched_rule: ""` and `default_used: true`.

### l0a — `llm` router

The router model is asked for a JSON reply `{"model": "<candidate>", "rationale":
"<text>"}`. A valid reply scores the named candidate 1 and every other candidate
0; anything invalid scores every candidate 0. Each candidate has a rule
`min_score: 1.0` on its own label. Candidates are `TinyLLM`, `BigLLM` and
`DefaultLLM` (the `default_model`).

**`desugaring_core_form`** — the router written out by hand: one `llm`
classifier and one rule per candidate (`route-TinyLLM`, `route-BigLLM`,
`route-DefaultLLM`).

| Behavior | Case |
|----------|------|
| The reply names `TinyLLM`. Its rule fires, `route_to` is `TinyLLM`, `matched_rule` is `route-TinyLLM`. | `picks-first-candidate` |
| The reply names `BigLLM`. The `TinyLLM` rule reads 0 and misses, the `BigLLM` rule fires. | `picks-second-candidate` |
| The reply names `DefaultLLM` on purpose. Its own rule fires, so `matched_rule` is set and `default_used` is `false`. An explicit pick and a fallback to the same model are told apart. | `picks-default-candidate` |
| The reply is well formed but names `huge`, which is not a candidate. Every candidate scores 0, no rule matches, the request falls back to `DefaultLLM`. | `unknown-model-falls-open` |
| The chat call fails. With no `on_error`, a failed classifier counts as "no match" (`match_false`), so the request falls back. | `chat-failure-falls-open` |

**`router_sugar`** — the `routing.router` shorthand. At load it is expanded into
the form above, with rule ids `__route_0`, `__route_1`, `__route_2` in candidate
order.

| Behavior | Case |
|----------|------|
| The shorthand routes exactly like the written-out form. A reply naming `TinyLLM` fires `__route_0`. | `picks-first-candidate` |
| A reply naming `BigLLM` fires `__route_1`. | `picks-second-candidate` |
| A reply naming `DefaultLLM` fires `__route_2` with `default_used: false`. | `picks-default-candidate` |
| The reply is plain prose ("I think you should use BigLLM."). It does not parse as JSON, no candidate is scored, the request falls back. | `non-json-reply-falls-open` |
| The reply is `["BigLLM"]`: valid JSON but an array, not an object. Rejected, fallback. | `json-array-reply-falls-open` |
| The reply has `model`, `rationale` and a third key `confidence`. Exactly two keys are required, so it is rejected. | `extra-key-falls-open` |
| The reply has only `model`. `rationale` is required, so it is rejected. | `missing-rationale-falls-open` |
| The reply has two keys, but `choice` instead of `model`. Rejected. | `missing-model-falls-open` |
| `rationale` is spaces only. It is trimmed to empty and rejected. | `blank-rationale-falls-open` |
| `model` is the number 1, not a string. Rejected. | `model-not-string-falls-open` |
| `rationale` is the number 5, not a string. Rejected. | `rationale-not-string-falls-open` |
| The reply names `BigLLM-model`. It contains a candidate name but is not equal to one, so no candidate is scored. Matching is exact, not substring. | `superstring-name-no-exact-match` |
| The reply names `bigllm`. It differs from `BigLLM` only in case and does not match. | `case-mismatched-name-no-match` |
| The reply is wrapped in a ``` fence with a newline after the opening fence. The fence is stripped, the object is parsed, and the request routes to `BigLLM`. | `fenced-reply-routes` |
| A valid fenced object is followed by "Hope that helps!". Text after the closing fence breaks the "no other text" rule, so it is rejected. | `fenced-trailing-prose-falls-open` |
| The reply opens a fence and has a newline, but never closes the fence. Rejected. | `no-closing-fence-falls-open` |
| The fence is followed by the object on the same line, with no newline. The body cannot be delimited, so it is rejected. | `no-newline-fence-falls-open` |
| With `route_trace`, the reply picks `BigLLM`. The trace has two entries: `TinyLLM` with score 0 and no rationale, then `BigLLM` with score 1 and the reply's rationale. `__route_2` is never reached. Only the picked candidate's entry carries the rationale. | `winner-carries-rationale-loser-does-not` |
| With `route_trace`, the reply is invalid. The trace has one entry per candidate, each with score 0, `result: false` and no rationale. The fallback decision still carries the trace. | `fail-open-trace-scores-zero` |

**`llm_on_error`** — one rule for `BigLLM`; the classifier has
`on_error: match_true`.

| Behavior | Case |
|----------|------|
| The chat call fails. Because of `match_true`, the `BigLLM` rule counts as matched and the request routes to `BigLLM` with `default_used: false`. | `failure-fires-via-match-true` |
| The chat call succeeds and picks `BigLLM`. The rule matches normally, not through `on_error`. | `success-match-routes` |
| The chat call succeeds and picks `TinyLLM`, which has no rule. `BigLLM` reads 0 and its rule misses, so the request falls back. `match_true` acts only on a failed call, never on a normal miss. | `success-miss-falls-open` |

**`llm_implicit_label`** — one rule that omits `label`; the classifier declares
`default_label: TinyLLM`.

| Behavior | Case |
|----------|------|
| The reply picks `TinyLLM`. The rule reads the `default_label` score, 1, and fires. Its trace entry has no `label` but carries the rationale. | `default-label-used-when-label-omitted` |
| The reply picks `BigLLM`, so `BigLLM` is the only scored label. The rule still reads `default_label` `TinyLLM`, which is absent and reads 0, so it misses and the request falls back. The omitted label is not "whatever label was scored". The trace entry still carries the rationale. | `default-label-read-not-primary` |

An `llm` classifier always has labels (its candidates), so the label-less
lone-score path is reachable only for the `classifier` type; see
`classifier_implicit_label` in l3.

### l1 — deterministic conditions

Unless stated otherwise, candidates are `local` (the `default_model`) and
`cloud`, and every rule routes to `cloud`.

**`conditions_vocab`** — one rule per op: `keywords_any: ["alpha"]`,
`keywords_all: ["charlie", "delta"]`, `regex: "echo-[0-9]+"`, an `any` over
`foxtrot` / `golf`, an `all` over `hotel` / `india`, an `all` of `juliett` and
`not(kilo)`, an implicit-all leaf `{keywords_any: ["lima"], max_chars: 1000}`,
`keywords_any: ["café"]`, and `regex: "id-\d"`.

| Behavior | Case |
|----------|------|
| "alphabet soup" contains `alpha` inside a longer word and matches. Keywords are substrings, not whole words. | `keywords_any-substring` |
| "ALPHA release notes" matches the lowercase keyword `alpha`. | `keywords_any-case-fold` |
| Lowercase "café" matches the keyword `café`. Non-ASCII bytes compare fine when the case already agrees. | `keywords_any-non-ascii-match` |
| "CAFÉ AU LAIT" does not match `café`. The ASCII letters are folded but `É` is left as is, so the fold is ASCII-only. Falls back to `local`. | `keywords_any-non-ascii-no-case-fold` |
| The input has both `charlie` and `delta`, so `keywords_all` matches. | `keywords_all-both-present` |
| The input has `charlie` but not `delta`, so `keywords_all` fails and the request falls back. | `keywords_all-one-missing-no-match` |
| "deploy echo-42 now" matches `echo-[0-9]+`. The pattern is searched anywhere in the input, not matched against the whole of it. | `regex-matches-substring` |
| "deploy echo-x now" has no digit after `echo-` and does not match. | `regex-no-match` |
| "deploy ECHO-42 now" does not match the lowercase pattern. The regex is case-sensitive. | `regex-case-sensitive-no-match` |
| `id-\d` matches "id-7": `\d` is the ECMAScript digit class. | `ecmascript-digit-class-matches` |
| `id-\d` does not match "id-draft". A grammar that reads `\d` as a literal `d` would match here. | `ecmascript-digit-class-not-literal-d` |
| "golf" satisfies one child of the `any`, which is enough. | `any-one-child-matches` |
| Neither `foxtrot` nor `golf` is present, so the `any` is false and the request falls back. | `any-no-child-matches` |
| `hotel` and `india` are both present, so the `all` matches. | `all-both-children-match` |
| `hotel` without `india` fails the `all`. | `all-one-child-no-match` |
| "juliett is here" has no `kilo`, so `not(kilo)` is true and the rule matches. | `not-child-absent-matches` |
| "juliett and kilo" makes `not(kilo)` false, so the rule fails. | `not-child-present-no-match` |
| A short input with `lima` satisfies both keys of the two-key leaf, which is read as an implicit `all`. | `implicit-all-both-keys` |
| `lima` plus more than 1000 bytes of padding fails `max_chars`, so the whole leaf is false even though `keywords_any` holds. | `implicit-all-one-key-no-match` |
| With `route_trace`, a `keywords_any` hit is recorded as `{"condition": "keywords_any", "result": true}`. | `keywords_any-trace` |
| The trace lists `keywords_any` false, then `keywords_all` true. | `keywords_all-trace` |
| The trace lists `keywords_any` false, `keywords_all` false, then `regex` true. | `regex-trace` |

**`leaf_order`** — one rule whose leaf is authored as `regex: "delta-[0-9]"`,
`keywords_any: ["delta"]`, `min_chars: 4`, in that order.

| Behavior | Case |
|----------|------|
| "delta-7 ready" satisfies all three keys. The trace reads `keywords_any`, `min_chars`, `regex`: children run in alphabetical op-name order, not the authored order. | `all-ops-match-order-is-by-op-name` |
| "gamma-7 ready" fails `keywords_any`, the first child in that order. The implicit `all` stops there, so the trace has one entry although `min_chars` would have passed. | `first-op-false-short-circuits-rest` |
| "delta" passes `keywords_any` and `min_chars` (5 ≥ 4) and fails `regex`. The trace has three entries ending in `regex`, which shows `min_chars` was evaluated before `regex`. | `min_chars-runs-before-regex` |

**`conditions_char_bounds`** — `min_chars: 10` then `max_chars: 6`, both on the
routing input.

| Behavior | Case |
|----------|------|
| A 10-byte input satisfies `min_chars: 10`. The bound is inclusive. | `min_chars-inclusive-boundary` |
| A 6-byte input fails `min_chars` and satisfies `max_chars: 6`. The upper bound is inclusive too. | `max_chars-inclusive-boundary` |
| A 7-byte input is above 6 and below 10, so neither rule matches and the request falls back. | `between-bounds-default` |
| "éééé" is 4 characters but 8 UTF-8 bytes. 8 is above `max_chars: 6`, so the request falls back. Counting characters would have matched. | `max_chars-utf8-byte-count` |
| The request has only an assistant message. The routing input is empty and its length is 0, so `min_chars` misses and `max_chars` matches. The empty input is observable rather than skipped. | `no-user-turn-empty-input` |
| With `route_trace`, a `min_chars` hit is recorded under the condition name `min_chars`. | `min_chars-trace` |
| The trace lists `min_chars` false, then `max_chars` true. | `max_chars-trace` |

**`conditions_total_char_bounds`** — `min_total_chars: 40`, then
`min_chars: 40`, then `max_total_chars: 6`. The total counts every text part in
the request; `min_chars` sees only the latest user turn.

| Behavior | Case |
|----------|------|
| A 25-byte assistant turn plus a 15-byte user turn total exactly 40, so `min_total_chars: 40` matches although the routing input is only 15 bytes. | `min_total_chars-inclusive-boundary` |
| 24 + 15 = 39 bytes is one short. `min_total_chars` misses, `min_chars: 40` misses on the 15-byte turn, `max_total_chars: 6` misses, and the request falls back. | `min_total_chars-below-boundary` |
| A single 6-byte message totals 6 bytes. The two earlier rules miss and `max_total_chars: 6` matches. | `max_total_chars-inclusive-boundary` |
| "éééé" totals 8 bytes, above `max_total_chars: 6`, so the request falls back. Counting characters would have matched. | `total_chars-utf8-byte-count` |
| 102 bytes of system, user and assistant history end in a 5-byte "go on". `min_total_chars` sees the history and matches; `min_chars: 40` cannot. Had the total been measured on the last turn alone, `max_total_chars: 6` would have matched instead, so `matched_rule` tells the two apart. | `long-history-short-last-turn` |
| System 14 + user 15 + assistant 11 + tool 11 = 51 bytes, with no single message reaching 40. `min_total_chars` matches only because every role is counted. | `total_chars-counts-every-role` |
| A 40-byte legacy `prompt` has no history, so its total equals its length and `min_total_chars-rule` matches (it comes before `min_chars-rule`, which would match too). | `prompt-form-total-equals-chars` |
| A Responses `input` array with a 25-byte assistant item and a 15-byte user item totals 40 and matches. Counting only the user item would give 15. | `responses-input-array-sums-all-items` |
| With `route_trace`, the hit is recorded under `min_total_chars`, not under `min_chars`, which shares its implementation. | `min_total_chars-trace` |
| The trace lists `min_total_chars` false, `min_chars` false, then `max_total_chars` true. | `max_total_chars-trace` |

**`conditions_features`** — `has_tools: true` then `has_images: true`.

| Behavior | Case |
|----------|------|
| `tools` holds one function definition, so `has_tools` is true and its rule matches. | `has_tools-present-matches` |
| No `tools` field and no image, so both rules miss and the request falls back. | `has_tools-absent-no-match` |
| `tools: []` counts as no tools. | `has_tools-empty-array-no-match` |
| `tools` is an object, not an array. It counts as no tools. | `has_tools-non-array-no-match` |
| The user content is an array with a text part and an `image_url` part. `has_tools` misses, `has_images` matches. | `has_images-present-matches` |
| Plain text content: no image, the request falls back. | `has_images-absent-no-match` |
| A Responses `input` array holds a role-tagged item whose content has an `input_image` part. The image is found. | `has_images-input-image-part` |
| A Responses `input` array holds bare content parts with no role wrapper. A top-level `input_image` part is found. | `has_images-input-bare-image-part` |
| The image is in an earlier user turn; the last user turn is text only. `has_images` is still true because every message is scanned, not just the routing turn. | `has_images-earlier-turn-still-counts` |
| With `route_trace`, a `has_tools` hit is recorded under the condition name `has_tools`. | `has_tools-trace` |
| The trace lists `has_tools` false, then `has_images` true. | `has_images-trace` |

**`conditions_features_negated`** — one rule `has_tools: false`.

| Behavior | Case |
|----------|------|
| A request with no tools makes `has_tools` false, which equals the authored `false`, so the rule matches. | `has_tools-false-matches-absent` |
| A request with tools makes `has_tools` true, which does not equal `false`, so the rule misses. `has_tools: false` is a comparison, not an always-true rule. | `has_tools-false-no-match-when-present` |

**`conditions_metadata`** — here `default_model` is `cloud` and every rule routes
to `local`. Rules in order: an `any` of `task_class any: ["payment", "checkout"]`
and `consent equals: "denied"`; `consent exists: false`; `region exists: true`;
`note equals: ""`.

| Behavior | Case |
|----------|------|
| `task_class: "payment"` is one of the listed values, so `any` matches. | `metadata-any` |
| `task_class: "billing, payment"` is split on commas and trimmed. The token `payment` is listed, so `any` matches. | `metadata-any-comma-separated` |
| `consent: "denied"` equals the rule value exactly. | `metadata-equals` |
| `consent: "denieddd"` is not equal to `denied`. `equals` is exact, not prefix or substring, so the request falls back. | `metadata-equals-no-match` |
| `consent: "DENIED"` does not equal `denied`. Comparison is case-sensitive. | `metadata-equals-case-sensitive` |
| `note: ""` against the rule `equals: ""`. A blank value counts as absent, so the rule never matches even though the strings are equal. | `metadata-equals-blank-never-matches` |
| No `consent` key is sent, so `exists: false` matches. | `metadata-exists-false` |
| `region: "eu-west"` is present and non-blank, so `exists: true` matches. | `metadata-exists-true` |
| `consent: "   "` is whitespace only and counts as absent, so `exists: false` matches. | `metadata-whitespace-counts-absent` |
| `region: 5` is a number. Only string values are kept when the request is parsed, so the key counts as absent and `exists: true` misses. The request schema allows strings only; this pins how the engine handles a request that breaks that. | `metadata-non-string-value-dropped` |
| With `route_trace`, the first rule's `any` stops at its first matching child, so the trace has one entry `{"condition": "metadata", "result": true}`. | `metadata-trace` |

**`input_forms`** — `keywords_any: ["escalate"]` then
`keywords_all: ["alpha", "omega"]`. The two rules tell which text was used as the
routing input.

| Behavior | Case |
|----------|------|
| A legacy `prompt` string containing "escalate" is used as the routing input. | `prompt-string-form` |
| A `prompt` array `["alpha ...", "omega ..."]` is joined. The `alpha` + `omega` rule fires only because both items were included. | `prompt-array-form` |
| A `prompt` array with an object item `{"escalate": "now"}` between two strings. The object is skipped, not turned into text: the `alpha` + `omega` rule fires, not the `escalate` rule. | `prompt-array-skips-non-string-parts` |
| A Responses `input` string containing "escalate" is used as the routing input. | `input-string-form` |
| An `input` array with a user item whose content is an `input_text` part. That part's text is the routing input. | `input-array-role-tagged` |
| An `input` array of plain strings with no roles. The strings are concatenated, so the `alpha` + `omega` rule fires. | `input-roleless-fallback` |
| An earlier user turn says "escalate", the last user turn does not. Only the latest user turn is the routing input, so the request falls back. Reading the whole history would have matched. | `messages-latest-user-turn-only` |
| The request carries `messages`, `prompt` and `input`. Only `messages` is read: its last turn has no keyword and the request falls back, although `prompt` and `input` both say "escalate". | `messages-wins-over-prompt-and-input` |
| No `messages`. The `prompt` matches the `alpha` + `omega` rule while the `input` would match the earlier `escalate` rule, so `matched_rule` shows that `prompt` was read and `input` ignored. | `prompt-wins-over-input` |

**`resolution`** — candidates `local`, `cloud`, `edge`. Rules in order:
`shared` or `only1` → `cloud`; `shared` or `only2` → `edge`; `only3` → `local`;
`escalate` → `cloud` with a non-empty `outputs` object.

| Behavior | Case |
|----------|------|
| "a shared concern" matches the first and the second rule. The first wins and the request goes to `cloud`, not `edge`. | `first-match-wins` |
| "only2" matches only the second rule, so the request goes to `edge`. | `later-rule-when-earlier-misses` |
| Nothing matches. The request goes to `local` with `matched_rule: ""` and `default_used: true`. | `fail-open-to-default` |
| "only3" matches a rule whose target is the `default_model`. `route_to` is `local` as in the fallback case, but `matched_rule` is set and `default_used` is `false`. | `explicit-rule-to-default-model` |
| The matched rule's `outputs` holds a string, a number, a nested object and an array. The decision carries it unchanged. | `nested-outputs-verbatim` |

**`trace`** — an `any` of `keywords_any: ["alpha"]` and `regex: "bravo"`, then an
`all` of `keywords_any: ["charlie"]` and `not(max_chars: 5)`.

| Behavior | Case |
|----------|------|
| No `route_trace` in the request. Rules are evaluated but the decision has no `trace` key. | `trace-omitted-when-not-requested` |
| `route_trace: false` behaves exactly like leaving it out. | `trace-omitted-when-explicitly-false` |
| "alpha" makes the first child of the `any` true, so the `regex` child is never evaluated. The trace has one entry, with no `score`, `label` or `rationale` (deterministic leaves carry none). | `trace-any-short-circuits-on-first-true` |
| "charlie please": the `any` misses and contributes two false entries, then the `all` matches. Its `not(max_chars: 5)` child is recorded as `max_chars` with `result: false`, the child's own result before negation. Four entries in total. | `trace-accumulates-across-missed-rule` |
| "zulu": both rules miss. The `all` stops after its first false child, so `max_chars` is not traced. The fallback decision carries the three accumulated entries. | `trace-all-short-circuits-and-default-carries-trace` |

The classifier trace entry (`condition: "classifier:<id>"`, with `score` and
usually `label`) is covered by the model-backed tiers.

### l2 — `semantic_similarity`

The classifier embeds the input and every reference phrase. A concept's score
is the highest cosine between the input and any of the concept's phrases, clamped
to `[0, 1]`. Candidates are `CodingLLM`, `MathLLM` and `DefaultLLM` (the
`default_model`). All cases request a trace, so the recorded `score` is checked.

**`semantic_scoring`** — one concept `coding` with phrases "writes code"
`[1, 0, 0]` and "fixes bugs" `[0, 1, 0]`; one rule with `min_score: 0.6`.

| Behavior | Case |
|----------|------|
| The input `[0.1, 1, 0]` has cosine ≈ 0.995 with the second phrase and ≈ 0.1 with the first. The concept scores 0.995, the max, and the rule fires. | `max-over-phrases-last-phrase-best` |
| The input `[1, 0.1, 0]` is closest to the first phrase. The max comes from the first phrase this time. Together the two cases show the score is the max, not the first, last or average phrase. | `max-over-phrases-first-phrase-best` |
| The input `[0.2, 0.2, 1]` scores ≈ 0.19 against both phrases, below 0.6, so the request falls back. | `weak-similarity-below-threshold` |
| The input `[-1, -1, 0]` has cosine ≈ -0.71 with each phrase. The recorded score is exactly 0, never negative. | `negative-cosine-floored-to-zero` |
| The input `[3, 0, 4]` has cosine exactly 0.6 with the first phrase, equal to `min_score`. The band is inclusive, so the rule fires. | `inclusive-boundary` |
| The second phrase is stubbed with a 2-element vector while the input equals the first phrase exactly (cosine 1.0). One uncomparable phrase fails the whole classifier; there is no fallback to the usable phrase. With default `on_error` the rule misses and its trace entry has no `score`. | `phrase-dimension-mismatch-fails-whole-concept` |

**`semantic_concepts`** — concepts `coding` ("writes code" `[5, 0, 0]`) and
`math` ("proves theorems" `[3, 4, 0]`); the `coding` rule comes first, both rules
use `min_score: 0.75`.

| Behavior | Case |
|----------|------|
| An input near the coding phrase scores ≈ 0.999 on `coding` and ≈ 0.63 on `math`. The coding rule reads its own label and fires; one trace entry with `label: "coding"`. | `coding-label-selects-coding-score` |
| An input near the math phrase scores ≈ 0.62 on `coding` (miss) and ≈ 0.9995 on `math` (match). Each rule reads its own concept; two trace entries. | `math-label-selects-math-score` |
| An input scores ≈ 0.96 on `math` and ≈ 0.80 on `coding`, both above 0.75. The coding rule fires because it comes first; the math rule is never evaluated. Rule order wins over the higher score. | `best-match-loses-to-rule-order` |
| An input scores ≈ 0.195 on both concepts. Both rules miss and the request falls back; two false trace entries. | `unrelated-input-falls-open` |

**`semantic_implicit_label`** — the same two concepts, `default_label: coding`,
and one rule without `label` (`min_score: 0.5`).

| Behavior | Case |
|----------|------|
| The input equals the coding phrase. The rule reads the `default_label` concept, scores 1.0 and fires. Its trace entry has no `label`. | `default-label-used-when-label-omitted` |
| The input equals the math phrase: `math` scores 1.0, `coding` 0.0. The rule still reads `coding` and misses, so the request falls back. The omitted label is the `default_label`, not the highest-scoring concept. | `default-label-read-not-max` |

**`semantic_on_error`** — two classifiers with one phrase each: `fail_open`
(default `on_error`) and `fail_closed` (`on_error: match_true`), each with its own
rule at `min_score: 0.5`; the `fail_open` rule comes first.

| Behavior | Case |
|----------|------|
| The input embedding for `fail_open` is stubbed `null`, so the embed call fails. The classifier fails, `match_false` applies, the rule misses and its trace entry has no `score`. `fail_closed` gets a real orthogonal vector, scores 0 and misses too. | `input-embedding-failure-fails-open` |
| The input vector for `fail_open` is `[0, 0, 0]`. A zero vector has no direction, the cosine is undefined, and the classifier fails as above. | `zero-vector-input-fails-open` |
| The input vector for `fail_open` has 2 elements while the phrase has 3. They cannot be compared, so the classifier fails as above. | `dimension-mismatch-fails-open` |
| `fail_open` scores a real 0 and misses. The input embedding for `fail_closed` is `null`; with `match_true` its rule fires as if matched, routing to `MathLLM`. Its trace entry has `result: true` and no `score`. | `failure-fires-via-match-true` |
| Both classifiers succeed with a real score ≈ 0.447, below 0.5. Both miss, including the `match_true` one. `on_error` acts only on failure, never on a low score. | `success-below-threshold-misses` |

A `semantic_similarity` classifier always has labels (its concepts), so the
label-less lone-score path is not reachable here either.

### l3 — `classifier`

The classification model returns a label → score map. A rule reads one label's
score and applies its `min_score` / `max_score` band. Candidates are
`CodingLLM`, `MathLLM` and `DefaultLLM` (the `default_model`). All cases request
a trace.

**`classifier_band`** — labels `code`, `math`, `algebra`. Rules in order: `code`
in `[0.4, 0.7]` → `CodingLLM`; `math` with no bounds → `MathLLM`; `algebra` with
`min_score` = `max_score` = 0.8 → `MathLLM`.

| Behavior | Case |
|----------|------|
| `code: 0.4` sits exactly on the lower bound and matches. | `band-min-inclusive` |
| `code: 0.7` sits exactly on the upper bound and matches. | `band-max-inclusive` |
| `code: 0.55` is strictly inside the band and matches. | `band-inside` |
| `code: 0.39` is just under the lower bound. All three rules miss (`math` and `algebra` are 0) and the request falls back; three trace entries. | `band-below-min` |
| `code: 0.71` is just over the upper bound and misses. The band is a closed interval, not only a floor. | `band-above-max` |
| The `math` rule has no bounds, so the default band `min_score: 0.5` applies. `math: 0.5` matches. | `default-band-inclusive` |
| `math: 0.49` is under the default 0.5 and misses. | `default-band-below` |
| `algebra: 0.8` equals both bounds of the point band and matches. | `point-band-exact-match` |
| `algebra: 0.79` is just under the point and misses. | `point-band-just-below` |
| `algebra: 0.81` is just over the point and misses. A point band is a single value, not a floor. | `point-band-just-above` |

**`classifier_label_selection`** — labels `code` and `math`; a `code` rule then a
`math` rule, both `min_score: 0.5`.

| Behavior | Case |
|----------|------|
| `{code: 0.9, math: 0.1}`: the code rule reads `code`, not `math`, and fires. | `code-label-reads-code-score` |
| `{code: 0.1, math: 0.9}`: the code rule misses, the math rule fires. Each rule reads its own label. | `math-label-reads-math-score` |
| `{math: 0.9}` with no `code` entry: `code` reads 0 (trace shows `score: 0.0`) and misses; `math` fires. | `label-absent-from-map-scores-zero` |
| `{code: 0.9}`: the code rule fires; the missing `math` entry never matters. | `only-code-in-map-matches` |
| `{code: 0.3}`: `code` is under the threshold and `math` is absent (0), so both rules miss and the request falls back. | `only-code-in-map-below-defaults` |
| `{code: 0.2, math: 0.3}`: both labels present but under the threshold, so the request falls back. | `neither-label-matches-defaults` |

**`classifier_implicit_label`** — `categorical` has labels `code`, `math` and
`default_label: code`; `labelless` declares no labels. Each has a rule without
`label` (`min_score: 0.5`), `categorical` first.

| Behavior | Case |
|----------|------|
| `categorical` returns `{code: 0.9, math: 0.1}`. The rule reads the `default_label` `code` and fires. | `default-label-used-when-label-omitted` |
| `categorical` scores 0 on both labels and misses. `labelless` returns a single entry `{toxicity: 0.8}`; with no labels declared, that lone score is read and the rule fires. | `primary-reads-lone-score` |
| `labelless` returns two entries `{code: 0.9, math: 0.1}`. With no label to pick, the score is 0 rather than a guess, so the rule misses although one entry is 0.9. | `primary-multi-entry-scores-zero` |

**`classifier_on_error`** — `fail_open` (default `on_error`) and `fail_closed`
(`match_true`), each with a `code` rule at `min_score: 0.5`; `fail_open` first.

| Behavior | Case |
|----------|------|
| `fail_open`'s model is stubbed to fail. With `match_false` the rule misses and its trace entry has no `score`. `fail_closed` scores 0.1 and misses, so the request falls back. | `failure-match-false-misses` |
| `fail_open` scores 0.1 and misses. `fail_closed`'s model fails; with `match_true` its rule fires, routing to `MathLLM`. Its trace entry has `result: true` and no `score`. | `failure-match-true-fires` |
| `fail_closed` returns `{}`. An empty map is a successful answer where `code` reads 0, so `on_error` does not apply and the `match_true` rule misses normally (trace `score: 0.0`). | `empty-map-is-not-a-failure` |
| `fail_open` returns `{}` and misses with score 0. Evaluation continues and `fail_closed` at 0.9 fires. An empty map neither fails nor stops evaluation. | `empty-map-on-first-classifier-continues` |
| Both return `{}`. Both labels read 0, both rules miss, the request falls back. | `both-maps-empty-default` |

**`classifier_all_combinator`** — rule 1: `all` of `clsX` and `clsY` → `CodingLLM`;
rule 2: `all` of `clsZ` (`match_true`) and `clsW` → `MathLLM`. Every leaf reads
`code` at `min_score: 0.5`.

| Behavior | Case |
|----------|------|
| `clsX: 0.9`, `clsY: 0.9`: both children hold, rule 1 fires; two trace entries. | `all-both-match-fires` |
| `clsX: 0.9`, `clsY: 0.3`: the `all` becomes false at `clsY`. In rule 2 `clsZ: 0.0` is false, so the `all` stops before `clsW`. Fallback; trace `clsX`, `clsY`, `clsZ`. | `all-one-child-below-defaults` |
| `clsX` fails with `match_false`, so rule 1's `all` stops at once and `clsY` is never evaluated. `clsZ: 0.0` misses. Fallback; trace `clsX` (no score), `clsZ`. | `all-failed-match-false-child-defaults` |
| `clsX: 0.0` misses rule 1. In rule 2 `clsZ` fails but has `match_true`, so it counts as true; `clsW: 0.9` holds; the `all` is true and routes to `MathLLM`. | `all-failed-match-true-child-still-matches` |

**`classifier_combinator_match_true`** — rule 1: `any` of `clsMtAny`
(`match_true`) and `clsSib` → `CodingLLM`; rule 2: `not` of `clsMtNot`
(`match_true`) → `MathLLM`.

| Behavior | Case |
|----------|------|
| `clsMtAny` fails and counts as true. The `any` stops there without evaluating `clsSib` and rule 1 fires; one trace entry. | `any-with-failed-match-true-child-fires` |
| `clsMtAny: 0`, `clsSib: 0`: rule 1 misses. `clsMtNot` fails and counts as true, the `not` turns it false, rule 2 misses, the request falls back. | `not-of-failed-match-true-misses` |
| `clsMtNot` succeeds with 0.0: its leaf is false, the `not` turns it true and rule 2 fires. `on_error` never touches a real low score. | `not-of-succeeding-low-classifier-matches` |

**`classifier_nested_combinator`** — rule 1: `not(any(clsP, clsQ))` →
`CodingLLM`; rule 2: `any(not(clsR), clsS)` → `MathLLM`.

| Behavior | Case |
|----------|------|
| `clsP: 0`, `clsQ: 0`: the inner `any` is false, the outer `not` makes it true, rule 1 fires. | `not-of-any-inner-false-matches` |
| `clsP: 0.9`: the inner `any` is true at once (`clsQ` not evaluated), so rule 1 misses. `clsR: 1.0` makes `not(clsR)` false and `clsS: 0` misses, so the request falls back. Trace `clsP`, `clsR`, `clsS`. | `not-of-any-inner-true-misses` |
| `clsP: 0.9` misses rule 1. `clsR: 0.0` makes `not(clsR)` true, so the `any` stops and fires without evaluating `clsS`. | `any-of-nested-not-fires-via-not-child` |

**`classifier_on_error_in_combinator`** — rule 1: `not(clsA)` → `CodingLLM`;
rule 2: `any(clsB, clsC)` → `MathLLM`. All three use the default `on_error`.

| Behavior | Case |
|----------|------|
| `clsA` fails and counts as false. The `not` turns it true and rule 1 fires. The trace shows `clsA` with `result: false` and no `score` while the rule still matched. | `not-of-failed-classifier-matches` |
| `clsA: 0.9` makes rule 1 miss. `clsB` fails (false) but `clsC: 0.9` holds, so the `any` is true. One failed child does not break an `any`. | `any-with-failed-child-still-matches` |
| `clsA: 0.9`; `clsB` and `clsC` both fail. The `any` is false and the request falls back. | `any-all-children-fail-defaults` |
