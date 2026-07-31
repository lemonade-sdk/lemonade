# Router back-compat conformance corpus

A corpus of golden `policy → Decision` conformance cases for the
back-compat rule: a future server must never break a policy authored against an
earlier schema major. A runner replays each case through the routing engine and
compares the produced `Decision` with the recorded one field for field. The
comparison is on parsed JSON, so key order and formatting do not matter — only
the values. Every field is matched exactly, with one exception: a trace `score`
is compared within a tolerance (`1e-12`). A score is computed (dot product,
square roots, a division), so its last bits can differ between CPU architectures; the tolerance absorbs that noise. Any other drift is a
back-compat violation.

## Layout

```
routing/<schema_major>/<case>/
  policy.json    # a collection.router policy authored at that schema major
  cases.jsonl    # one request → expected Decision per line
```

A `<schema_major>` directory holds case directories and nothing else, and each case
directory is a leaf. The runner fails on anything else, so a stray file or a broken
symlink cannot quietly drop cases.

`<schema_major>` is the policy's root `version`. Cases are
grouped by the **engine behavior they lock**, so the
corpus reads as a checklist against the v1 semantics table in
`src/cpp/resources/schemas/README.md`.

```
1/
  l0a_desugaring_core_form/   # llm router in core form (llm classifier + identity rules): reply picks a candidate
  l0a_desugaring_sugar_form/  # llm router in routing.router sugar form; desugars to an llm classifier + identity rules: reply picks a candidate
  l0a_llm_on_error/           # llm router on_error: match_true fires only on a failed call, not a normal miss
  l0a_router_reply_contract/  # what counts as a valid router reply: strict {model, rationale}, code-fence stripping
  l0a_router_trace/           # router trace: only the picked candidate carries a rationale; fail-open scores 0
  l1_conditions_char_bounds/  # min_chars / max_chars (own policy: length rules are greedy)
  l1_conditions_features/     # boolean request-feature ops: has_tools / has_images
  l1_conditions_features_negated/  # authored has_tools:false — equality, matches when absent
  l1_conditions_metadata/     # metadata equals / any / exists / token-set semantics
  l1_conditions_regex_dialect/  # regex dialect lock: ECMAScript \d digit class (not a literal d)
  l1_conditions_vocab/        # keyword / regex ops + any / all / not / implicit-all
  l1_input_forms/             # routing-input extraction from the `prompt` and `input` request fields
  l1_outputs/                 # matched rule's nested outputs bag copied verbatim into Decision
  l1_resolution/              # rule-list resolution: first-match-wins, fail-open default, explicit route to default
  l1_trace/                   # route_trace=true: per-leaf trace, accumulation, short-circuit, default
  l2_semantic_concepts/       # semantic_similarity: each label reads its own concept's score; first-match over rules
  l2_semantic_on_error/       # undefined cosine (empty / zero / mismatched-length input) => classifier fails
  l2_semantic_on_error_match_true/  # on_error match_true fires the band only on a failed embed, not a real miss
  l2_semantic_scoring/        # max cosine over a concept's phrases, floored at 0, inclusive band boundary
  l3_classifier_all_combinator/     # all combinator over classifier leaves, with on_error children
  l3_classifier_band/         # min_score/max_score band: two-sided, default, and point (min==max)
  l3_classifier_combinator_match_true/  # match_true leaves inside any / not
  l3_classifier_implicit_label/     # omitted label => default_label; label-less classifier => primary() lone score
  l3_classifier_label_selection/    # a rule reads its own label's score; an absent label resolves to 0
  l3_classifier_nested_combinator/  # not-of-any and any-of-not nesting
  l3_classifier_on_error/     # classifier failure vs empty map; match_false default, match_true fires
  l3_classifier_on_error_in_combinator/  # a failed classifier leaf inside not / any
2/ # coming in a later version
  ...
```

The `l1_*` groups are **deterministic** — they replay with an empty
`ClassifierServices` (no model backend). The `l0a` / `l2` / `l3` groups are
**model-backed**, but no real model runs: the runner binds the engine to a
`FakeClassifierServices`, and each case declares the answers it should return in
a `services` field (see [Model-backed stub answers](#model-backed-stub-answers-services)).
This locks the engine's threshold, selection, and fail-open logic without
depending on a real model's floats.

All model names in the corpus — `RouterLLM`, `EmbedModel`, `MathLLM`, `TinyLLM`,
and the rest — are **placeholders for stubbed backends**. They read like real
models but nothing is loaded; a name is just the key a case's stub answers are
filed under.

## `cases.jsonl` line schema

Each line is one JSON object:

| Field | Meaning |
|-------|---------|
| `name` | Unique, human-readable case id within the file. One case locks one behavior. |
| `request` | A request body in any form `build_route_context` accepts: chat-completions (`messages`), legacy completions (`prompt`), or Responses (`input`), plus optional `model`, `metadata`, `tools`, and `route_trace`. The engine input is the last user message (or the `prompt`/`input` text); `min_chars`/`max_chars` count its UTF-8 bytes. |
| `decision` | The exact `Decision` the engine must emit: `version`, `route_to`, `matched_rule` (empty on fall-through), `default_used`, `outputs`, and `trace` when `route_trace` is set. |
| `services` | Model-backed cases only. The stub answers the fake backend returns, keyed by service (`embed` / `run_classifier` / `chat`) then model. Omitted for deterministic cases. See below. |
| `note` | Optional. Free-text annotation for a non-obvious case; ignored by the runner. |

## Model-backed stub answers (`services`)

A model-backed case adds a `services` object telling the fake backend what to
return. It maps each service to a per-model answer:

```
"services": {
  "embed":          { "<model>": { "<text>": [numbers] | null } },
  "run_classifier": { "<model>": { "<label>": number } | null },
  "chat":           { "<model>": "<reply>" | null }
}
```

- `embed` — used by `semantic_similarity`. Answers are keyed by the exact text
  embedded, which is both each reference phrase and the routing input's own text.
  A `null` or empty vector makes the embed fail.
- `run_classifier` — used by the `classifier` type. A `null` answer makes the
  classifier fail; a `{}` map is a **success** with every label resolving to 0.
- `chat` — used by the `llm` router. A `null` answer makes the router call fail.

A failed service (a `null` answer) is what drives the `on_error` cases: the
classifier reports failure and its leaf resolves by `on_error` (default
`match_false`, or `match_true` when set).

Two things an author of an `embed` case must get right, or the recorded `score`
will not match:

- **Compute cosines from float32-rounded components.** Embedding vectors are
  stored as 32-bit floats, so a component written as `0.1` is first rounded to
  its float32 value and only then fed into the cosine (which accumulates in
  double). A score computed straight from the JSON decimals in full double
  precision can differ by more than the `1e-12` tolerance and fail. Round each
  component to float32 first, then compute the dot product, norms, and division.
- **Each case embeds its own reference phrases.** The runner builds a fresh
  engine per case, so a `semantic_similarity` classifier re-embeds its reference
  phrases from that case's `services` every time. Each model-backed case must
  therefore declare embeddings for **all** of its reference phrases (and for the
  input text); cases in the same directory are independent and may use different
  phrase vectors.

## Coverage matrix

One semantic per row → the single case that locks it. The matrix is the
sufficiency argument: every behavior the engine defines for v1 has exactly one
lock, and combinators/resolution are tested once (they are op-agnostic) rather
than across every leaf. Tiers run L0a → L1 → L2 → L3; L1 is deterministic (empty
`ClassifierServices`), the others are model-backed (stubbed answers, so a real
backend's numeric drift can never change the recorded `Decision`).

### L0a — `llm` router

| Semantic | Case |
|----------|------|
| router reply naming a candidate ⇒ that candidate's identity rule fires | `l0a_desugaring_core_form/picks-first-candidate` |
| reply naming another candidate ⇒ its rule fires | `l0a_desugaring_core_form/picks-second-candidate` |
| reply picking `default_model` explicitly ⇒ `default_used: false` (not fall-through) | `l0a_desugaring_core_form/picks-default-candidate` |
| reply naming a non-candidate ⇒ no rule ⇒ default | `l0a_desugaring_core_form/unknown-model-falls-open` |
| router call fails, no `on_error` ⇒ no-match ⇒ default | `l0a_desugaring_core_form/chat-failure-falls-open` |
| `routing.router` sugar desugars to one `llm` classifier + identity rules (`__route_N`); same outcomes as the core form | `l0a_desugaring_sugar_form/` |
| router call fails + `on_error: match_true` ⇒ rule fires as matched | `l0a_llm_on_error/failure-fires-via-match-true` |
| a successful reply matches its rule normally (not via `on_error`) | `l0a_llm_on_error/success-match-routes` |
| `match_true` does not fire on a normal mismatch, only on a failed call | `l0a_llm_on_error/success-miss-falls-open` |
| reply must be JSON (non-JSON ⇒ fall open) | `l0a_router_reply_contract/non-json-reply-falls-open` |
| reply must be a JSON object, not an array | `l0a_router_reply_contract/json-array-reply-falls-open` |
| reply keys are exactly `{model, rationale}` — an extra key is rejected | `l0a_router_reply_contract/extra-key-falls-open` |
| `rationale` is required | `l0a_router_reply_contract/missing-rationale-falls-open` |
| `model` is required | `l0a_router_reply_contract/missing-model-falls-open` |
| a whitespace-only `rationale` trims to empty and is rejected | `l0a_router_reply_contract/blank-rationale-falls-open` |
| `model` must be a string | `l0a_router_reply_contract/model-not-string-falls-open` |
| `rationale` must be a string | `l0a_router_reply_contract/rationale-not-string-falls-open` |
| candidate-name match is exact — a superstring of a candidate is not the candidate | `l0a_router_reply_contract/superstring-name-no-exact-match` |
| candidate-name match is case-sensitive | `l0a_router_reply_contract/case-insensitive-name-no-match` |
| one wrapping code fence is stripped, then the object is parsed | `l0a_router_reply_contract/fenced-reply-routes` |
| text after the closing fence ⇒ rejected | `l0a_router_reply_contract/fenced-trailing-prose-falls-open` |
| an opening fence with no closing fence ⇒ rejected | `l0a_router_reply_contract/no-closing-fence-falls-open` |
| an opening fence with no newline ⇒ rejected | `l0a_router_reply_contract/no-newline-fence-falls-open` |
| trace: only the picked candidate's entry carries the rationale | `l0a_router_trace/winner-carries-rationale-loser-does-not` |
| trace: an unmatched reply leaves every candidate scored 0 with no rationale; the default carries the trace | `l0a_router_trace/fail-open-trace-scores-zero` |

### L1 — deterministic conditions

| Semantic | Case |
|-----------------|------|
| `keywords_any` — substring match | `l1_conditions_vocab/keywords_any-substring` |
| `keywords_any` — ASCII case-fold | `l1_conditions_vocab/keywords_any-case-fold` |
| `keywords_any` — non-ASCII letter matches when case already agrees | `l1_conditions_vocab/keywords_any-non-ascii-match` |
| `keywords_any` — case-fold is ASCII-only (`É` not folded to `é` ⇒ no match) | `l1_conditions_vocab/keywords_any-non-ascii-no-case-fold` |
| `keywords_all` — all tokens present | `l1_conditions_vocab/keywords_all-both-present` |
| `keywords_all` — one token missing ⇒ no match | `l1_conditions_vocab/keywords_all-one-missing-no-match` |
| `regex` — pattern searched for anywhere in the input, not matched against all of it | `l1_conditions_vocab/regex-matches-substring` |
| `regex` — non-matching input ⇒ no match | `l1_conditions_vocab/regex-no-match` |
| `regex` — case-sensitive (uppercase input misses lowercase pattern) | `l1_conditions_vocab/regex-case-sensitive-no-match` |
| `regex` — ECMAScript dialect: `\d` is the digit class (matches a digit) | `l1_conditions_regex_dialect/ecmascript-digit-class-matches` |
| `regex` — ECMAScript `\d` is not a literal `d` ⇒ `id-draft` misses | `l1_conditions_regex_dialect/ecmascript-digit-class-not-literal-d` |
| `any` — matches if at least one child matches | `l1_conditions_vocab/any-one-child-matches` |
| `any` — no child matches ⇒ no match | `l1_conditions_vocab/any-no-child-matches` |
| `all` — matches only if every child matches | `l1_conditions_vocab/all-both-children-match` |
| `all` — one child fails ⇒ no match | `l1_conditions_vocab/all-one-child-no-match` |
| `not` — matches when the child does not match | `l1_conditions_vocab/not-child-absent-matches` |
| `not` — child matches ⇒ no match | `l1_conditions_vocab/not-child-present-no-match` |
| multi-key leaf ⇒ implicit `all` | `l1_conditions_vocab/implicit-all-both-keys` |
| multi-key leaf ⇒ implicit `all` — one key fails ⇒ no match | `l1_conditions_vocab/implicit-all-one-key-no-match` |
| `has_tools` — non-empty `tools[]` ⇒ match | `l1_conditions_features/has_tools-present-matches` |
| `has_tools` — no `tools[]` ⇒ no match | `l1_conditions_features/has_tools-absent-no-match` |
| `has_tools` — empty `tools[]` counts as absent ⇒ no match | `l1_conditions_features/has_tools-empty-array-no-match` |
| `has_images` — image content part ⇒ match | `l1_conditions_features/has_images-present-matches` |
| `has_images` — no image ⇒ no match | `l1_conditions_features/has_images-absent-no-match` |
| `has_images` — Responses API `input` array, role-tagged item with an `input_image` part ⇒ match | `l1_conditions_features/has_images-input-image-part` |
| `has_images` — Responses API `input` array, bare `input_image` part (no role wrapper) ⇒ match | `l1_conditions_features/has_images-input-bare-image-part` |
| `has_images` — scans every message, not just the routing turn | `l1_conditions_features/has_images-earlier-turn-still-counts` |
| `has_tools: false` — equality, matches when tools absent | `l1_conditions_features_negated/has_tools-false-matches-absent` |
| `has_tools: false` — no match when tools present (not a catch-all) | `l1_conditions_features_negated/has_tools-false-no-match-when-present` |
| `min_chars` — inclusive (`>=`), UTF-8 bytes | `l1_conditions_char_bounds/min_chars-inclusive-boundary` |
| `max_chars` — inclusive (`<=`), UTF-8 bytes | `l1_conditions_char_bounds/max_chars-inclusive-boundary` |
| `min_chars`/`max_chars` count bytes, not code points | `l1_conditions_char_bounds/max_chars-utf8-byte-count` |
| length between the bounds satisfies neither rule ⇒ fall through to default | `l1_conditions_char_bounds/between-bounds-default` |
| `metadata` `any` — value equals one of the listed | `l1_conditions_metadata/metadata-any` |
| `metadata` `equals` — value matches exactly | `l1_conditions_metadata/metadata-equals` |
| `metadata` `equals` — near-miss value fails (exact, not substring) | `l1_conditions_metadata/metadata-equals-no-match` |
| `metadata` `equals` — case-sensitive (`DENIED` ≠ `denied`) | `l1_conditions_metadata/metadata-equals-case-sensitive` |
| `metadata` `equals: ""` — blank value counts as absent, so it can never match | `l1_conditions_metadata/metadata-equals-blank-never-matches` |
| `metadata` `exists: false` — key absent | `l1_conditions_metadata/metadata-exists-false` |
| `metadata` `exists: true` — key present ⇒ match | `l1_conditions_metadata/metadata-exists-true` |
| `metadata` — whitespace-only value counts as absent | `l1_conditions_metadata/metadata-whitespace-counts-absent` |
| `metadata` `any` — comma-separated value, one token listed | `l1_conditions_metadata/metadata-any-comma-separated` |
| matched rule's non-empty nested `outputs` copied verbatim into `Decision` | `l1_outputs/nested-outputs-verbatim` |
| first-match-wins (earlier rule beats a later match) | `l1_resolution/first-match-wins` |
| later rule fires when earlier misses | `l1_resolution/later-rule-when-earlier-misses` |
| fail-open to `default_model` | `l1_resolution/fail-open-to-default` |
| a rule routing to `default_model` keeps `default_used: false` and sets `matched_rule` (distinct from fall-through) | `l1_resolution/explicit-rule-to-default-model` |
| legacy completions string `prompt` ⇒ routing input | `l1_input_forms/prompt-string-form` |
| legacy completions array-of-strings `prompt` ⇒ parts joined into routing input | `l1_input_forms/prompt-array-form` |
| array `prompt` ⇒ non-string parts skipped, not stringified | `l1_input_forms/prompt-array-skips-non-string-parts` |
| `input` bare string ⇒ routing input | `l1_input_forms/input-string-form` |
| `input` array of role-tagged messages ⇒ last user message's text | `l1_input_forms/input-array-role-tagged` |
| `input` array with no user role ⇒ role-less parts concatenated (fallback) | `l1_input_forms/input-roleless-fallback` |
| `route_trace` unset ⇒ Decision carries no `trace` | `l1_trace/trace-omitted-when-not-requested` |
| `route_trace: false` ⇒ behaves like unset, no `trace` | `l1_trace/trace-omitted-when-explicitly-false` |
| `route_trace: true` ⇒ one trace entry per evaluated leaf; `any` short-circuits on first true | `l1_trace/trace-any-short-circuits-on-first-true` |
| trace accumulates across evaluated rules; `not` records the child leaf's raw result | `l1_trace/trace-accumulates-across-missed-rule` |
| `all` short-circuits on first false; fail-open default still carries the accumulated trace | `l1_trace/trace-all-short-circuits-and-default-carries-trace` |
| trace emits condition `keywords_any` | `l1_conditions_vocab/keywords_any-trace` |
| trace emits condition `keywords_all` | `l1_conditions_vocab/keywords_all-trace` |
| trace emits condition `regex` | `l1_conditions_vocab/regex-trace` |
| trace emits condition `min_chars` | `l1_conditions_char_bounds/min_chars-trace` |
| trace emits condition `max_chars` | `l1_conditions_char_bounds/max_chars-trace` |
| trace emits condition `has_tools` | `l1_conditions_features/has_tools-trace` |
| trace emits condition `has_images` | `l1_conditions_features/has_images-trace` |
| trace emits condition `metadata` | `l1_conditions_metadata/metadata-trace` |

The one trace-emitting family not in the table above is the classifier band
(`classifier:<id>`); it is model-backed and is locked by the `l0a` tier above
and the `l2` / `l3` tiers below.

The regex **dialect** is locked by `l1_conditions_regex_dialect` (in the table
above). Most regex cases use grammar-neutral syntax (`[0-9]`) that every dialect
shares, but that group uses the ECMAScript-only `\d` digit class: `\d` matches a
digit and not a literal `d`, so the cases would break if the engine's
`std::regex::ECMAScript` grammar changed.

### L2 — `semantic_similarity` (embeddings + cosine)

| Semantic | Case |
|----------|------|
| concept score is the max cosine over its phrases (best is a later phrase) | `l2_semantic_scoring/max-over-phrases-last-phrase-best` |
| max cosine over phrases (best is the first phrase) | `l2_semantic_scoring/max-over-phrases-first-phrase-best` |
| cosine well under the band ⇒ miss ⇒ default | `l2_semantic_scoring/weak-similarity-below-threshold` |
| negative cosine floored to 0, never negative | `l2_semantic_scoring/negative-cosine-floored-to-zero` |
| cosine exactly at `min_score` ⇒ inclusive match | `l2_semantic_scoring/inclusive-boundary` |
| a rule's label reads that concept's own score | `l2_semantic_concepts/coding-label-selects-coding-score` |
| each rule selects its own concept's score | `l2_semantic_concepts/math-label-selects-math-score` |
| first-match over rules wins, not the highest-scoring concept | `l2_semantic_concepts/best-match-loses-to-rule-order` |
| input below every concept's threshold ⇒ default | `l2_semantic_concepts/unrelated-input-falls-open` |
| empty input vector ⇒ cosine undefined ⇒ fails ⇒ `match_false` | `l2_semantic_on_error/empty-input-embedding-fails-open` |
| zero-norm input vector ⇒ undefined ⇒ fails | `l2_semantic_on_error/zero-vector-input-fails-open` |
| input vector length ≠ phrase length ⇒ undefined ⇒ fails | `l2_semantic_on_error/dimension-mismatch-fails-open` |
| embed failure + `on_error: match_true` ⇒ band counts as matched (trace score absent, result true) | `l2_semantic_on_error_match_true/failure-fires-via-match-true` |
| `on_error` acts only on failure; a real low score still misses | `l2_semantic_on_error_match_true/success-below-threshold-misses` |

### L3 — `classifier` (label scores + band)

| Semantic | Case |
|----------|------|
| two-sided band inclusive at the lower bound | `l3_classifier_band/band-min-inclusive` |
| inclusive at the upper bound | `l3_classifier_band/band-max-inclusive` |
| a score strictly inside the band matches | `l3_classifier_band/band-inside` |
| just below the lower bound ⇒ miss ⇒ default | `l3_classifier_band/band-below-min` |
| just above the upper bound ⇒ miss (closed interval, not a floor) | `l3_classifier_band/band-above-max` |
| no bounds ⇒ default `min_score` 0.5, inclusive | `l3_classifier_band/default-band-inclusive` |
| below the default 0.5 ⇒ miss ⇒ default | `l3_classifier_band/default-band-below` |
| `min_score == max_score` ⇒ point band, only the exact value matches | `l3_classifier_band/point-band-exact-match` |
| just below the point ⇒ miss | `l3_classifier_band/point-band-just-below` |
| just above the point ⇒ miss | `l3_classifier_band/point-band-just-above` |
| a rule reads its own label's score | `l3_classifier_label_selection/code-label-reads-code-score` |
| each rule selects its own label | `l3_classifier_label_selection/math-label-reads-math-score` |
| a label absent from the score map resolves to 0 | `l3_classifier_label_selection/label-absent-from-map-scores-zero` |
| only the read label present ⇒ matches | `l3_classifier_label_selection/only-code-in-map-matches` |
| read label present but low, other absent ⇒ default | `l3_classifier_label_selection/only-code-in-map-below-defaults` |
| both labels under threshold ⇒ default | `l3_classifier_label_selection/neither-label-matches-defaults` |
| omitted label ⇒ classifier's `default_label` score (not the max over labels) | `l3_classifier_implicit_label/default-label-used-when-label-omitted` |
| label-less classifier ⇒ `primary()` reads the lone score | `l3_classifier_implicit_label/primary-reads-lone-score` |
| label-less classifier with >1 score ⇒ `primary()` returns 0 (no guess) | `l3_classifier_implicit_label/primary-multi-entry-scores-zero` |
| classifier failure, no `on_error` ⇒ `match_false` (trace score absent) | `l3_classifier_on_error/failure-match-false-misses` |
| classifier failure + `on_error: match_true` ⇒ fires | `l3_classifier_on_error/failure-match-true-fires` |
| an empty score map is a success (labels ⇒ 0), so `on_error` does not apply | `l3_classifier_on_error/empty-map-is-not-a-failure` |
| an empty score map is a success ⇒ rule evaluation continues | `l3_classifier_on_error/empty-map-on-first-classifier-continues` |
| all empty maps ⇒ every label 0 ⇒ default (`match_true` does not fire) | `l3_classifier_on_error/both-maps-empty-default` |
| `all` — every child matches ⇒ fires | `l3_classifier_all_combinator/all-both-match-fires` |
| `all` short-circuits false on the first missing child | `l3_classifier_all_combinator/all-one-child-below-defaults` |
| `all` — a `match_false` failed child ⇒ false, short-circuits | `l3_classifier_all_combinator/all-failed-match-false-child-defaults` |
| `all` — a `match_true` failed child counts as true | `l3_classifier_all_combinator/all-failed-match-true-child-still-matches` |
| `any` — a `match_true` failed child ⇒ true, short-circuits | `l3_classifier_combinator_match_true/any-with-failed-match-true-child-fires` |
| `not` of a `match_true` failed child (true) ⇒ false | `l3_classifier_combinator_match_true/not-of-failed-match-true-misses` |
| `not` of a real low score (false) ⇒ true (`on_error` only on failure) | `l3_classifier_combinator_match_true/not-of-succeeding-low-classifier-matches` |
| `not(any)` with every inner child false ⇒ true | `l3_classifier_nested_combinator/not-of-any-inner-false-matches` |
| `not(any)` with one inner child true ⇒ false | `l3_classifier_nested_combinator/not-of-any-inner-true-misses` |
| `any` containing a `not` child fires via that `not` | `l3_classifier_nested_combinator/any-of-nested-not-fires-via-not-child` |
| `not` of a `match_false` failed leaf (false) ⇒ true | `l3_classifier_on_error_in_combinator/not-of-failed-classifier-matches` |
| `any` — a failed child does not break it when a sibling matches | `l3_classifier_on_error_in_combinator/any-with-failed-child-still-matches` |
| `any` — all children fail (`match_false`) ⇒ false ⇒ default | `l3_classifier_on_error_in_combinator/any-all-children-fail-defaults` |
