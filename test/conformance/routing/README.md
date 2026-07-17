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

`<schema_major>` is the policy's root `version` (currently only `1`). Cases are
grouped by the fixture they derive from (`test/cpp/fixtures/routing/`).

## `cases.jsonl` line schema

Each line is one JSON object:

| Field | Meaning |
|-------|---------|
| `name` | Unique, human-readable case id within the file. |
| `request` | An OpenAI chat-completions body (`model`, `messages`, optional `metadata`). The engine input is the last user message; `min_chars`/`max_chars` count its UTF-8 bytes. |
| `decision` | The exact `Decision` the engine must emit: `version`, `route_to`, `matched_rule` (empty on fall-through), `default_used`, `outputs`. |

## Scope

This directory holds **deterministic** cases only — `keywords_any` / `regex` /
`min_chars` / `metadata` and first-match / fail-open behavior, all of which are
byte-for-byte reproducible without a model backend. Model-backed cases
(semantic_similarity / classifier / llm) are stubbed against pinned fake
classifier services and tracked separately.
