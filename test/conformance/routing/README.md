# Router back-compat conformance corpus

Frozen golden `policy → Decision` cases that enforce the hard back-compat rule:
a future server must never break a policy authored against an earlier schema
major. A runner replays each case through the routing engine and asserts the
produced `Decision` matches the frozen expectation exactly; any drift is a
back-compat violation.

## Layout

```
routing/<schema_major>/<case>/
  policy.json    # a collection.router policy authored at that schema major
  cases.jsonl    # one request → expected Decision per line
```

`<schema_major>` is the policy's root `version`. Cases are
grouped by the **engine behavior they lock**, so the
corpus reads as a checklist against the frozen-v1 semantics table in
`src/cpp/resources/schemas/README.md`.

```
1/
  l1_conditions_char_bounds/  # min_chars / max_chars (own policy: length rules are greedy)
  l1_conditions_metadata/     # metadata equals / any / exists / token-set semantics
  l1_conditions_vocab/        # keyword / regex ops + any / all / not / implicit-all
  l1_resolution/              # rule-list resolution: first-match-wins, fail-open default
  # l2_semantic/              # stubbed semantic_similarity — to be added
  # l3_classifier/            # stubbed classifier             — to be added
  # l0a_router/               # stubbed llm router + desugaring — to be added
2/ # coming with a following version
  ...
```

The `l1_conditions_*` and `l1_resolution` groups are **deterministic** — they replay with an
empty `ClassifierServices` (no model backend). The `l2` / `l3` / `l0a` groups are
**stubbed model-backed** cases (pinned fake `ClassifierServices`); they are *to be
added* and will carry per-case stub outputs.

## `cases.jsonl` line schema

Each line is one JSON object:

| Field | Meaning |
|-------|---------|
| `name` | Unique, human-readable case id within the file. One case locks one behavior. |
| `request` | An OpenAI chat-completions body (`model`, `messages`, optional `metadata`). The engine input is the last user message; `min_chars`/`max_chars` count its UTF-8 bytes. |
| `decision` | The exact `Decision` the engine must emit: `version`, `route_to`, `matched_rule` (empty on fall-through), `default_used`, `outputs`. |
| `note` | Optional. Free-text annotation for a non-obvious case; ignored by the runner. |

## Coverage matrix

One frozen deterministic semantic per row → the single case that locks it. The
matrix is the sufficiency argument: every deterministic behavior the engine
freezes for v1 has exactly one lock, and combinators/resolution are tested once
(they are op-agnostic) rather than across every leaf.

| Frozen semantic | Case |
|-----------------|------|
| `keywords_any` — substring match | `l1_conditions_vocab/keywords_any-substring` |
| `keywords_any` — ASCII case-fold | `l1_conditions_vocab/keywords_any-case-fold` |
| `keywords_all` — all tokens present | `l1_conditions_vocab/keywords_all-both-present` |
| `keywords_all` — one token missing ⇒ no match | `l1_conditions_vocab/keywords_all-one-missing-no-match` |
| `regex` — ECMAScript dialect | `l1_conditions_vocab/regex-ecmascript` |
| `regex` — non-matching input ⇒ no match | `l1_conditions_vocab/regex-no-match` |
| `any` — matches if at least one child matches | `l1_conditions_vocab/any-one-child-matches` |
| `all` — matches only if every child matches | `l1_conditions_vocab/all-both-children-match` |
| `all` — one child fails ⇒ no match | `l1_conditions_vocab/all-one-child-no-match` |
| `not` — matches when the child does not match | `l1_conditions_vocab/not-child-absent-matches` |
| `not` — child matches ⇒ no match | `l1_conditions_vocab/not-child-present-no-match` |
| multi-key leaf ⇒ implicit `all` | `l1_conditions_vocab/implicit-all-both-keys` |
| `min_chars` — inclusive (`>=`), UTF-8 bytes | `l1_conditions_char_bounds/min_chars-inclusive-boundary` |
| `max_chars` — inclusive (`<=`), UTF-8 bytes | `l1_conditions_char_bounds/max_chars-inclusive-boundary` |
| `min_chars`/`max_chars` count bytes, not code points | `l1_conditions_char_bounds/max_chars-utf8-byte-count` |
| `metadata` `any` — value equals one of the listed | `l1_conditions_metadata/metadata-any` |
| `metadata` `equals` — value matches exactly | `l1_conditions_metadata/metadata-equals` |
| `metadata` `equals` — near-miss value fails (exact, not substring) | `l1_conditions_metadata/metadata-equals-no-match` |
| `metadata` `exists: false` — key absent | `l1_conditions_metadata/metadata-exists-false` |
| `metadata` — whitespace-only value counts as absent | `l1_conditions_metadata/metadata-whitespace-counts-absent` |
| `metadata` `any` — comma-separated value, one token listed | `l1_conditions_metadata/metadata-any-comma-separated` |
| first-match-wins (earlier rule beats a later match) | `l1_resolution/first-match-wins` |
| later rule fires when earlier misses | `l1_resolution/later-rule-when-earlier-misses` |
| fail-open to `default_model` | `l1_resolution/fail-open-to-default` |

Stubbed model-backed semantics (`min_score`/`max_score` band, `semantic_similarity`
max-cosine, `classifier` label resolution, `llm` router desugaring, `on_error`
fail-open) — *to be added* with the `l2` / `l3` / `l0a` groups.
