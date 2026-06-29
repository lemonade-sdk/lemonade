# Unifying `semantic_similarity` with `classifier`

## Problem

The first cut of the `semantic_similarity` classifier was a special case. It
encoded exactly **one** concept, scored the input against it, and reported that
single cosine under an **empty-string key** (`Score.labels[""]`), read back via a
bespoke `Score::primary()` path. The `classifier` type, by contrast, already had
a clean, general contract: a model returns a `label -> score` map, and a routing
condition selects a `label` and applies a `min_score`/`max_score` band.

Two interfaces for what is fundamentally the same idea — "score the input, then
band a named score" — meant two mental models, two JSON shapes, and a magic empty
key that existed solely to paper over the mismatch. It also imposed a hard limit:
one `semantic_similarity` classifier could express only a single concept.

## The change

`semantic_similarity` now speaks the same `label -> score` contract as
`classifier`. `reference_phrases` becomes a **map of concept -> phrases**, each
concept is embedded once, and the classifier reports the **max cosine per
concept** under that concept's label. Concept names *are* the classifier's
labels, so conditions reference them exactly like any other classifier.

### Configuration JSON

Before — one concept, addressed implicitly:

```jsonc
"classifiers": [
  { "id": "is_coding", "type": "semantic_similarity", "model": "nomic-embed-text-v1.5-GGUF",
    "reference_phrases": ["write a function", "fix this bug", "refactor this code"] }
],
"rules": [
  { "match": { "classifier": "is_coding", "min_score": 0.78 }, "route_to": "vllm.qwen3-32b" }
]
```

After — multiple concepts, addressed by label like any classifier:

```jsonc
"classifiers": [
  { "id": "topic", "type": "semantic_similarity", "model": "nomic-embed-text-v1.5-GGUF",
    "reference_phrases": {
      "coding": ["write a function", "fix this bug", "refactor this code"],
      "math":   ["integral", "prove this theorem"]
    } }
],
"rules": [
  { "match": { "classifier": "topic", "label": "coding", "min_score": 0.78 }, "route_to": "vllm.qwen3-32b" },
  { "match": { "classifier": "topic", "label": "math",   "min_score": 0.80 }, "route_to": "vllm.qwen3-32b" }
]
```

The condition syntax (`label` + band) is now identical across both classifier
types, and `default_label` works the same way too.

## Extra capability

One `semantic_similarity` classifier can now carry **many concepts** and the
input is compared against all of them in a single evaluation, reporting an
independent score per concept. Previously this required one classifier (and one
embedding pass over the reference set) per concept. Now the input is embedded
once and scored against every concept, so multi-way semantic routing —
"coding vs. math vs. chit-chat" — is a single declaration. Concept reference
embeddings are still computed once and cached, so adding concepts is cheap.

## More consistent code

- **One scoring contract.** Both classifier types fill `Score.labels` with named
  scores and are read via `score_of(label)`. The condition layer
  (`make_classifier_band_condition`, the leaf factory's `label`/`default_label`
  resolution) is now fully generic — it has no branch for "semantic vs. model"
  classifiers.
- **Shared parsing.** `make_classifier` uses common `parse_labels` /
  `parse_default_label` helpers for both types; the only type-specific step is
  `parse_reference_phrases`, which derives the labels from the concept keys.
- **No magic empty key.** The empty-string slot is gone. `Score::primary()` is
  now a small, strict helper — it returns the lone entry of a genuinely
  label-less classifier and `0.0` otherwise, so an unlabeled condition can never
  silently match an arbitrary label of a multi-label score. Nothing in the engine
  treats `""` specially anymore.

## Net effect

The same routing power as before, plus multi-concept semantic routing, expressed
through a single, consistent classifier contract. Less surface area, fewer
special cases, and a JSON schema where `semantic_similarity` and `classifier`
differ only where they genuinely must (reference phrases vs. a model).
