# PII router benchmarking: methodology, findings, and known limits

How the PII detection models in this directory are evaluated against
`nvidia/Nemotron-PII`, what the numbers do and do not mean, and the traps that
produced wrong conclusions before they were caught.

Read §1 and §8 before quoting any number from these runs to anyone.

## 1. The one thing to know first

**The headline benchmark measures a binary: "did the model emit any entity
anywhere in this document".** It does not compare label to label. Every
scorer — `pii_ner_eval.py`, `pii_gliner_eval.py`, `pii_pplx_eval.py`, and the
router policies — collapses to that single question, then compares it against
`pii_category != "none"`.

That is the right question for routing (local vs cloud), and it is why BIO vs
BIOES vs span-decoder differences never enter the comparison: the tagging
scheme is discarded before scoring. `pii_ner_eval.py` strips the prefix with
`label.split("-", 1)[-1]`, collects the types, then scores only `bool(detected)`.
The detected types are **logged but never compared**.

The consequence: a model earns a true positive by firing *any* label, even one
unrelated to the PII actually present. A model detecting `PERSON` on a document
whose gold labels are `{religious_belief, political_view}` scores a hit while
missing everything that mattered. Document-level recall therefore **overstates
category coverage**, and the "missed categories" it produces are only the cases
where a model found *nothing at all*.

### 1.1 `detected=` is a set of TYPES, and carries no entity count

An easy and consequential misread. The log field is a **deduplicated, sorted set
of label names** — it says nothing about how many entities were found, where they
were, or what text they covered.

```
[PASS][TP] nemotron-pii-15485: detected=account_number sensitivity=0.067
```

That line does **not** mean "found one thing". On that document pplx found
**two** spans (`BH-00028745` and `AET-5577-3489-12`), both typed
`account_number`, which collapse to one entry in the set. A document with eight
email addresses logs `detected=private_email` once. The number of labels in
`detected=` is the size of the *type* set and has no relationship to the number
of entities detected.

So a gold/predicted count mismatch — 4 gold categories vs 1 predicted label — is
usually **two independent collapses stacked**, not under-detection:

1. **many entities to one type**: 2 spans to 1 label
2. **many gold types to one model type**: `medical_record_number` *and*
   `health_plan_beneficiary_number` both to pplx's single `account_number`

Only a third cause is a real failure, and on that document it was a coverage
hole rather than a miss: `employment_status` and `occupation` are categories
pplx's 9-label taxonomy cannot express at all. §5.5 works this document through
end to end.

`pii_category_recall.py` (§5) exists to re-score the same runs per category and
recover what the binary throws away.

## 2. The dataset

`nvidia/Nemotron-PII`, test split, sampled by `build_nemotron_corpus.py`.

- **55 distinct gold labels**, finer-grained than any model's output space.
- Roughly 35 direct identifiers (`first_name`, `ssn`, `credit_debit_card`,
  `ipv4`, `api_key`, …) plus ~12 quasi-identifiers and sensitive attributes
  (`gender`, `race_ethnicity`, `sexuality`, `religious_belief`,
  `political_view`, `education_level`, `employment_status`, `age`, `language`,
  `blood_type`, `occupation`, `biometric_identifier`). Most detector models
  have no class for that second group at all.
- Raw rows carry `spans` with `start` / `end` / `label` / `text`. The builder
  **used to discard the offsets**, keeping only the comma-joined type list per
  document, which is why everything before §5.6 is document-level. It now
  preserves them (`normalize_text_with_map`), and `annotate_gold_spans.py`
  back-fills them onto corpora built before the change — the 20k corpus carries
  `pii_spans` today (§5.5).
- **Re-fetching the dataset alone would not have recovered usable gold
  offsets**, which is why the fix was a builder change. The raw
  `start`/`end` index into the *original* text, and the builder edits that text
  twice before any model sees it: `normalize_text()` collapses every whitespace
  run to a single space, then `wrap_in_message()` prepends one of 8 random
  `PROMPT_PREFIXES` (up to 46 chars). A raw offset is therefore wrong by an
  amount that depends on how much whitespace preceded it, plus a per-document
  prefix length. Nor can offsets be recovered by searching for the span text:
  gold strings repeat within a document (one case has the gold name `Richard`
  at 5+ positions), and the builder's own `surviving_spans` check is exactly
  that substring search — sound for "is it present", useless for "where". The
  fix is an index map through the whitespace collapse plus `len(prefix)`, i.e. a
  builder change, not a re-download. That is what
  `normalize_text_with_map()` + `remap_span()` do, and `annotate_gold_spans.py`
  applies them to an already-sampled corpus by matching each case back to its
  source row on normalized text (§5.5).
- Documents are short: p50 ≈ 744 chars, p99 ≈ 3,259, max 7,191 (**1,737 tokens**
  measured with the pplx tokenizer). See §7 for why this settles the
  context-length question.

### 2.1 There is no benign arm, and there cannot be one from this dataset

**A scan of 30,000 test rows found ZERO documents with no PII.** Nemotron-PII is
a pure-positive generation dataset — `has_pii = len(spans) > 0` is true for
essentially every row. That is why every corpus here reports `n_benign: 1`
despite the builder being asked for thousands.

Everything downstream follows from this:

- **Precision, FP-rate and F-beta are statistically empty** across every
  *document-level* run in this series. The negative arm is n=1. Character-level
  precision is measurable on positive documents alone and is now measured
  (§5.6), but it is bounded by PII density rather than being a true FP-rate: on
  this corpus PII is **14.58%** of non-whitespace prompt characters, so a model
  flagging every character scores char precision 0.1458 and char F1 0.2544
  rather than 0. It narrows the hole; it does not close it, and it does not
  retire the benign-arm work (§9.1).
- **A model that flagged every single document would score identically to a
  precise one** on every metric reported.
- Only **recall / leak rate** carries information.

A real negative arm has to be *constructed*, not sampled. The two viable
sources, neither of which is built yet:

1. **Nemotron hard negatives** — take Nemotron documents and substitute every
   PII span with generic non-identifying text. Ground truth is PII-free *by
   construction*, so no second detector is needed and there is no circularity.
   Domain- and style-matched to the positive arm, which isolates the identifier
   signal from topic confounds. Attribute spans woven into prose (gender,
   religion, occupation) substitute awkwardly and would need care.
2. **Real prompts** (dolly / ultrachat) — operationally realistic, but they have
   no gold negative labels. **Filtering them PII-free with a detector is
   circular**: it deletes exactly the documents models flag, driving measured FP
   toward zero artificially.

Until one exists, do not quote a precision number from these runs.

## 3. Corpora and where runs live

| Corpus | Cases | Used by |
|---|---|---|
| `l2_pii_nemotron` | 2,500 PII + 1 benign | LLM-as-router runs |
| `l2_pii_nemotron_20k` | 20,000 PII + 1 benign | detector model runs |

Logs land in `<corpus>/runs/`. The two corpora are different samples, so
**absolute rates are not comparable across them**; the normalized per-category
profiles in §5.3 are.

### 3.1 Two log dialects that disagree about the same brackets

This bites anyone parsing these logs:

- **NER scripts** put the *outcome* in the second bracket:
  `[PASS][TP]` = detected, `[FAIL][FN]` = missed.
- **`pii_routing_eval.py`** puts the *case class* there and carries the outcome
  in the verdict: `[PASS][TP]` = routed local, **`[FAIL][TP]` = leaked to
  cloud**. There are no `[FN]` lines at all.

Grepping `\[FAIL\]\[FN\]` against a routing log returns zero and looks like a
perfect score. The verdict (`PASS`/`FAIL`) is the only field meaning the same
thing in both dialects — read that alone. Routing FAIL lines also insert
`(pii=a,b,c)` between the case name and the colon, which breaks a naive
`(\S+?):` name capture.

## 4. Running a model

Detector models (no Lemonade server needed):

```bash
python test/eval/pii_ner_eval.py    --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose
python test/eval/pii_gliner_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose
python test/eval/pii_pplx_eval.py   --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose

# ONNX variant: export once, then score it and diff against the safetensors run
python test/eval/pplx_pii_masking2onnx.py
python test/eval/pii_pplx_onnx_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose     --parity-against test/conformance/routing/1/l2_pii_nemotron_20k/runs/pplx_f1f90a53823f5df0a1344c1e137d9fffdaab54d6_20260908-173941.log
```

**Always pass `--verbose`.** A non-verbose log records only failures, which
makes it impossible to distinguish "passed" from "not yet run" — it breaks both
`--resume-from` and all of the re-scoring in §5.

Character-level scoring (§5.6), which needs a corpus carrying `pii_spans`:

```bash
# one-time: back-fill gold offsets onto an existing corpus
python test/eval/annotate_gold_spans.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k

# score the detectors - SEQUENTIALLY, see trap 19
for m in mmbert pplx privacy-filter; do
    python test/eval/pii_char_f1_eval.py --model $m --verbose --intra-op-threads 16
done

# re-score offline, no inference: by label, by span length, by decision rule
python test/eval/pii_char_f1_report.py --by-label
python test/eval/pii_char_f1_report.py --by-length
python test/eval/pii_char_f1_report.py --rule all
```

`pii_char_f1_eval.py` has `--resume-from <run>.spans.jsonl`, and unlike the
resume path that produced trap 13 it **replays the recorded spans rather than
reconstructing tallies**, so every metric in a resumed summary sits on the same
denominator. Verified: a resumed run reproduces a clean one to 4 decimals.

### 4.1 perplexity-ai/pplx-pii-masking needs its own script

`pii_pplx_eval.py` exists rather than a `--model` flag on `pii_ner_eval.py`
because the checkpoint is not an `AutoModelForTokenClassification`:

- Custom architecture (`PiiMaskingModel`, `auto_map` → `modeling_pii_masking.py`)
  requiring `trust_remote_code=True`, exposing `model.predict(text)` →
  `(spans, sensitivity)` rather than a `.logits` tensor over `id2label`.
- Span decoding is a **constrained BIOES Viterbi over 37 tags inside the
  model**. An external argmax would bypass the decoder it was trained to be read
  through.
- `config.json`'s `id2label` is the vestigial backbone field (`LABEL_0`/
  `LABEL_1`), not the 37 real tags.

**It has two heads, and they disagree. Do not route on the sensitivity head.**
The token head yields spans over 9 categories; a separate document-level
sensitivity head yields one scalar. Measured over the full 20k corpus:

| Head | Recall |
|---|---|
| Span head (the routing signal) | **99.20%** |
| Sensitivity head @ 0.5 | **9.16%** |

The model card's own example shows the same thing — three correct spans at
`sensitivity=0.027`. Routing on the sensitivity head would leak ~91% of PII.
`pii_pplx_eval.py` scores the span head as primary and tallies sensitivity
separately, never mixing them.

### 4.2 The pplx ONNX export is decision-identical, and that took work to be true

`pii_pplx_onnx_eval.py` scores the ONNX export from `pplx_pii_masking2onnx.py`.
It is the only ONNX row in this table whose equivalence to its safetensors
counterpart was actually *measured* rather than assumed (§6.2).

**The graph deliberately stops at raw per-token logits — the constrained BIOES
Viterbi is not baked in.** Scoring ONNX by argmax while the safetensors run was
scored by Viterbi would confound a backend difference with a decoder difference
and make any delta uninterpretable. So the ONNX scorer imports the
**checkpoint's own `ViterbiDecoder`** out of its `trust_remote_code` module and
feeds it the ONNX logits, constructing it from `config.viterbi_b_bias` /
`viterbi_e_bias` exactly as `PiiMaskingModel.__init__` does — with **no weights
loaded**, since that decoder's entire state is the 37-label list plus two bias
scalars. Tokenization, truncation, chunking and the log format are byte-equivalent
to `pii_pplx_eval.py`, so the two runs differ in exactly one variable.

Two things that cost time here and will cost it again:

- **The export dies on Windows *after* succeeding.** `torch.onnx`'s own progress
  printer emits `U+2705` on a successful graph capture; a cp1252 stdout can't
  encode it, so the export raises `UnicodeEncodeError` with the graph already
  built. It reads like an export bug and is a console-encoding one. Both
  `pplx_pii_masking2onnx.py` and the eval scripts now reconfigure stdout to
  UTF-8 — do not remove that.
- **Do not export from `perplexity-ai/pplx-pii-masking-vllm-tmp`.** That repo
  repacks the same weights as a stock `Qwen3ForTokenClassification` with a
  top-level `"is_causal": false`, which stock transformers **does not read**
  (huggingface/transformers#39554). It would load with correct tensor shapes,
  look self-consistent, and be silently **causal instead of bidirectional**. The
  original repo's vendored `modeling_pplx_qwen3.py` flips
  `layer.self_attn.is_causal` per layer *and* rebuilds the mask via
  `or_mask_function=bidirectional_mask_function(...)`. Same shape of trap as
  the gpt-oss reinterpretation in `privacy_filter_ml_v2_onnx_repro.md`.

## 5. Coverage-aware per-category scoring

`pii_category_recall.py` re-scores existing runs per category. **It needs no new
inference** — every verbose log already records its per-case predictions
(`detected=CREDIT_CARD,DATE_TIME,...`), so gold and predicted labels are both
recoverable offline.

**That holds for re-scoring labels, not for re-scoring decisions.** The logs
record which types a model emitted, never the scores behind them — no logits, no
probabilities, per-case or otherwise. So anything that changes the *decision
rule* (a threshold sweep, argmax vs `min_score`, a different aggregation) cannot
be recovered from a log and needs a fresh pass; §6.4 is that case. The pass is
cheaper than the original run suggests — hitting the ONNX export directly is
~12 cases/s, 28min for 20,001, against the 7.19hr the router row reports — so
re-run rather than approximating from `detected=`.

`pii_taxonomy.py` maps every taxonomy into **one canonical interlingua** (24
categories) rather than pairwise: N mappings instead of N², and adding a model
later is one table.

Three rules matter more than the mapping itself:

1. **Coverage.** Each model declares which canonical categories it can express.
   Per-category recall is computed only over that set; gold categories outside
   it are reported as `no class` — a declared coverage gap, **not a miss**.
   Without this a model is punished for lacking a class it never claimed.
2. **Coarse-label credit** (default, lenient). A model label may be coarser than
   the canonical class, so mappings are one-to-many: pplx's `account_number`
   expands to `{FINANCIAL_ACCOUNT, GOV_ID, INTERNAL_ID, MEDICAL}`. A prediction
   counts for any gold category in its expansion, because flagging an SSN as
   "account number" genuinely is a catch for routing. `--strict` instead solves
   a maximum bipartite matching so one emitted label satisfies at most one gold
   category. **Report both** — see §5.2.
3. **Catch-alls don't attribute.** pplx's `other_pii` and OpenMed's
   `MASKEDNUMBER` would otherwise match everything. They count toward the
   document-level binary but are excluded from per-category credit and tallied
   separately.

```bash
# label-attributed recall (detector models)
python test/eval/pii_category_recall.py --corpus-dir <corpus> <corpus>/runs/*.log
python test/eval/pii_category_recall.py --strict --corpus-dir <corpus> <corpus>/runs/*.log

# document-level miss enrichment (works for LLM routing runs too)
python test/eval/pii_category_recall.py --doc-level --corpus-dir <corpus> <log> ...
```

### 5.1 Per-category recall (lenient), 20k corpus

`no class` = absent from that model's taxonomy. `n/a` = covered but no gold
document exercises it.

| Category | support | mmbert | openmed | openmed-v2 | pplx | openai-pf | gliner\* |
|---|---:|---:|---:|---:|---:|---:|---:|
| PERSON_NAME | 9621 | 98.8% | 95.7% | 99.3% | 99.5% | 95.6% | 99.8% |
| CONTACT_EMAIL | 8535 | 99.8% | 98.9% | 99.8% | 99.7% | 98.1% | 78.9% |
| CONTACT_PHONE | 4914 | 100.0% | 94.4% | 100.0% | 99.0% | 84.7% | 99.1% |
| ADDRESS_LOCATION | 7438 | 91.3% | 90.9% | 98.8% | 74.7% | 50.3% | 99.3% |
| DATE_TIME | 11214 | 99.2% | 80.4% | 97.4% | 84.8% | 77.9% | 98.9% |
| DATE_OF_BIRTH | 3230 | 99.8% | 99.9% | 99.7% | 99.8% | 99.3% | 99.9% |
| AGE | 1390 | 92.4% | 83.6% | 95.1% | no class | no class | 92.9% |
| GOV_ID | 2412 | 86.9% | 69.1% | 76.3% | 98.0% | 93.0% | 98.2% |
| FINANCIAL_ACCOUNT | 5408 | 98.1% | 92.8% | 99.3% | 98.7% | 86.8% | 99.4% |
| CREDENTIAL_SECRET | 3567 | no class | 79.0% | 98.0% | 63.4% | 47.3% | 98.4% |
| ORG_COMPANY | 5439 | 96.7% | 13.4% | 93.1% | no class | no class | 98.1% |
| OCCUPATION_EMPLOYMENT | 5403 | no class | 19.9% | 52.5% | no class | no class | 97.9% |
| WEB_URL | 6652 | 99.0% | 99.0% | 99.6% | 82.6% | 29.9% | 52.5% |
| NETWORK_ID | 2536 | 39.9% | 71.0% | 66.1% | no class | no class | 82.5% |
| ACCOUNT_HANDLE | 2214 | no class | 88.5% | 96.6% | no class | no class | 94.6% |
| MEDICAL | 3763 | no class | no class | no class | 93.0% | 85.9% | 99.0% |
| **BIOMETRIC** | 1958 | no class | no class | no class | no class | no class | 99.2% |
| PHYSICAL_ATTRIBUTE | 0 | no class | n/a | n/a | no class | no class | no class |
| GENDER_SEXUALITY | 1575 | no class | 53.1% | 95.7% | no class | no class | 99.8% |
| RACE_ETHNICITY_LANGUAGE | 2208 | **5.0%** | no class | no class | no class | no class | 98.1% |
| BELIEF_POLITICAL | 1721 | **5.2%** | no class | no class | no class | no class | 96.7% |
| **EDUCATION** | 1416 | no class | no class | no class | no class | no class | 98.2% |
| VEHICLE | 941 | no class | 89.7% | 99.5% | no class | no class | 99.5% |
| INTERNAL_ID | 5076 | no class | no class | no class | 96.5% | 87.2% | 99.5% |

\* **GLiNER is not comparable to the rest.** `pii_gliner_eval.py` extracts the
label vocabulary from the corpus itself and passes those 55 gold label names to
the model at inference — it is told exactly what to look for, in the dataset's
own words. Its numbers deserve an asterisk everywhere they appear.

Key readings:

- **mmBERT's demographic failure is genuine, not a taxonomy gap.** It *has*
  Presidio's `NRP` class (nationality / religious / political group) and fires
  it on **5.0% / 5.2%** of the documents that need it. pplx and OpenMed score
  zero there because they have no such class at all. The old "missed categories"
  column conflated those two very different situations.
- **OpenMed v1 → v2 was a real jump:** ORG_COMPANY 13.4 → 93.1%, GENDER
  53.1 → 95.7%, CREDENTIAL_SECRET 79 → 98%, DATE_TIME 80.4 → 97.4%. Its
  remaining weak spot is OCCUPATION_EMPLOYMENT at 52.5%.
- **BIOMETRIC (1,958 docs) and EDUCATION (1,416) are total blind spots across
  all four production detectors** — a taxonomy hole, not a score. Only GLiNER
  covers them, and only because it was handed the label names.
- **pplx** leads on GOV_ID (98.0%) and is the only non-GLiNER model covering
  MEDICAL (93.0%), but is soft on ADDRESS_LOCATION (74.7%), DATE_TIME (84.8%),
  WEB_URL (82.6%) and CREDENTIAL_SECRET (63.4%), and cannot express 12 of the 24
  canonical categories — 11 of which this corpus actually exercises, the most
  of any model.

**The pplx ONNX export is omitted from the table above because it is identical
to the `pplx` column in every cell** — verified, not assumed: both scorings were
regenerated from the two runs' logs and the per-category result objects compare
equal under lenient *and* strict credit. Adding a duplicate column would widen
the table for no information. The ONNX run does appear in
`category_recall_{lenient,strict}.{txt,json}` as `pplx#2`, so the raw reports
still carry it.

### 5.1.1 pplx per-category profile, lenient vs strict

Both modes for the one model where the difference is most severe. **These figures
hold for the safetensors and ONNX runs alike.** Support is the number of gold
documents exercising that canonical category; the categories pplx cannot express
are listed below the table rather than carried as `no class` rows.

| Category | support | lenient | strict | Δ |
|---|---:|---:|---:|---:|
| PERSON_NAME | 9621 | 99.5% | 99.5% | — |
| CONTACT_EMAIL | 8535 | 99.7% | 99.7% | — |
| CONTACT_PHONE | 4914 | 99.0% | 99.0% | — |
| FINANCIAL_ACCOUNT | 5408 | 98.7% | 98.7% | — |
| DATE_TIME | 11214 | 84.8% | 84.8% | — |
| WEB_URL | 6652 | 82.6% | 82.6% | — |
| ADDRESS_LOCATION | 7438 | 74.7% | 74.7% | — |
| CREDENTIAL_SECRET | 3567 | 63.4% | 63.4% | — |
| DATE_OF_BIRTH | 3230 | 99.8% | **63.9%** | −35.9 |
| GOV_ID | 2412 | 98.0% | **71.1%** | −26.9 |
| MEDICAL | 3763 | 93.0% | **60.1%** | −32.9 |
| INTERNAL_ID | 5076 | 96.5% | **55.1%** | −41.4 |

Not expressible by pplx's 9 categories (coverage holes, not scores):
`ACCOUNT_HANDLE`, `AGE`, `BELIEF_POLITICAL`, `BIOMETRIC`, `EDUCATION`,
`GENDER_SEXUALITY`, `NETWORK_ID`, `OCCUPATION_EMPLOYMENT`, `ORG_COMPANY`,
`RACE_ETHNICITY_LANGUAGE`, `VEHICLE` — 11 of the 24 canonical categories, every
one exercised by this corpus, the widest gap of any model measured. (A 12th,
`PHYSICAL_ATTRIBUTE`, is also outside pplx's taxonomy but has zero support here,
so it costs nothing on this corpus.)

**The four categories that collapse are exactly the four sharing one emitted
label.** pplx's single `account_number` expands to
`{FINANCIAL_ACCOUNT, GOV_ID, INTERNAL_ID, MEDICAL}`, and `private_date` covers
both `DATE_TIME` and `DATE_OF_BIRTH`. Under lenient credit one emitted
`account_number` satisfies an SSN, an MRN and a customer ID in the same
document; under strict matching it can satisfy only one, and the other two
become misses. The eight unchanged rows are categories pplx maps
one-to-one — their numbers are credit-scheme independent and are the ones to
quote when the scheme is not stated.

This is a **granularity** result, not an accuracy one: pplx genuinely found
*something* at those positions in ~98% of cases. Whether that counts depends on
whether your downstream use needs to know *which* identifier it was. For
routing (local vs cloud) lenient is the right lens. For masking or redaction
policy that treats an SSN differently from a customer ID, strict is.

### 5.2 Strict mode changes who looks good

With coarse-label credit disabled:

| | pplx GOV_ID | pplx MEDICAL | pplx INTERNAL_ID | mmbert DOB | openmed-v2 GOV_ID |
|---|---:|---:|---:|---:|---:|
| lenient | 98.0% | 93.0% | 96.5% | 99.8% | 76.3% |
| strict | **71.1%** | **60.1%** | **55.1%** | **63.8%** | 76.3% |

**OpenMed v2 is essentially unchanged under strict scoring; pplx and openai-pf
collapse.** That is a direct consequence of taxonomy granularity — pplx's single
`account_number` was standing in for SSNs, MRNs and customer IDs simultaneously,
and mmBERT's `DATE_TIME` was covering both dates and dates-of-birth. Neither
mode is "the" right answer without span offsets to disambiguate against, so
report both.

### 5.3 Document-level miss enrichment (`--doc-level`)

LLM-as-router runs emit a routing decision and a free-text rationale, **never
entity labels**, so there is nothing to normalize on the prediction side. And
clean per-category attribution is impossible for a document-level decision:
**only 8 of 2,500 Nemotron documents contain a single canonical category**, so a
missed document is missed for all ~7 of its categories at once.

What survives is **enrichment**: `P(missed | doc contains category) ÷ overall
miss rate`. `>1.0` means the category is over-represented among the documents a
model let through. It is robust to the co-occurrence confound and is the
blind-spot question the "missed categories" column was always really asking.

LLMs, 2.5k corpus (baselines 6.12% / 3.28% / 2.84% — 153, 82, 71 misses):

| Category | support | Qwen3.5-9B | Qwen3.5-2B | Qwen3.5-0.8B |
|---|---:|---:|---:|---:|
| WEB_URL | 893 | **1.92x** | **1.81x** | **1.97x** |
| ORG_COMPANY | 672 | **1.65x** | 1.22x | **1.68x** |
| RACE_ETHNICITY_LANGUAGE | 224 | **1.60x** | 1.09x | 1.41x |
| ADDRESS_LOCATION | 991 | **1.52x** | 1.29x | 0.82x |
| EDUCATION | 188 | 1.48x | 0.81x | 0.56x |
| NETWORK_ID | 310 | 0.47x | 0.98x | **2.04x** |
| CREDENTIAL_SECRET | 469 | 0.21x | 0.59x | 1.05x |
| PERSON_NAME | 1213 | 0.11x | 0.33x | 0.35x |
| MEDICAL | 491 | 0.07x | 0.43x | 0.36x |
| DATE_OF_BIRTH | 312 | **0.00x** | **0.00x** | 0.11x |

**The LLMs' blind spots are judgment categories, not identifiers.** Anything
that unambiguously screams PII — date of birth, person name, medical record,
card number — is essentially never missed. What leaks is what requires a call on
whether it *counts*: URLs, company names, addresses, demographics. The logged
rationales say so verbatim: `"The request contains no personal information"` on
a document labeled `company_name, education_level, occupation, sexuality, url`.

**This explains the size paradox.** The 9B leaks *more* than the 0.8B (6.12% vs
2.84%). The profiles say why: the 9B's misses concentrate in judgment categories
(URL 1.92x, ORG 1.65x, ADDRESS 1.52x) — it reasons its way *out* of flagging.
The 0.8B's concentrate in technical identifiers (NETWORK_ID 2.04x,
CREDENTIAL_SECRET 1.05x, INTERNAL_ID 1.22x) — it fails to recognize them.
**Judgment errors vs capability errors**, and they want opposite fixes: policy
and prompt tightening for the 9B, a larger model for the 0.8B.

Detectors, 20k corpus:

| Category | mmbert | openmed | pplx | openai-pf |
|---|---:|---:|---:|---:|
| BELIEF_POLITICAL | **6.97x** | no class | no class | no class |
| RACE_ETHNICITY_LANGUAGE | **6.16x** | no class | no class | no class |
| WEB_URL | 0.00x | 0.00x | **2.12x** | **1.68x** |
| ADDRESS_LOCATION | 1.18x | 0.58x | **1.71x** | **1.53x** |
| OCCUPATION_EMPLOYMENT | no class | **1.59x** | no class | no class |
| VEHICLE | no class | **1.52x** | no class | no class |

mmBERT's misses are almost entirely demographic-attribute documents —
**independent corroboration** of the 5.0% / 5.2% NRP recall in §5.1, reached by
a completely different route. Note the small base: mmBERT's 6.97x rests on 25
total misses, so detector enrichments are directionally right but statistically
thin. The LLM figures (71–153 misses) are firmer.

### 5.4 Coverage gaps barely affect routing

| Model | in-coverage docs missed | gap-only docs missed |
|---|---|---|
| mmbert | 24 / 19,921 (0.12%) | 1 / 79 |
| openmed | 18 / 19,977 (0.09%) | 1 / 23 |
| openmed-v2 | 0 / 19,977 (0.00%) | 0 / 23 |
| pplx | 153 / 19,894 (0.77%) | 6 / 106 |
| openai-pf | 1,014 / 19,894 (5.10%) | 48 / 106 |

Because Nemotron documents average ~7 categories, only 23–106 documents per
model have gold sets lying *entirely* in a coverage hole. **Taxonomy gaps hurt
category coverage, not leak rate** — the document-level leak rates remain the
right routing metric; §5.1 explains *why* they differ.

### 5.5 Character-level F1: the design, and why the old logs could not support it

**Status: implemented.** The corpus now carries gold offsets and the three ONNX
detectors are character-scored; **§5.6 holds the results**. This section keeps
the design rationale, because every choice in it is still load-bearing and the
next person adding a model needs to know why the metric is shaped this way.

What changed, in three pieces:

| Piece | What it does |
|---|---|
| `build_nemotron_corpus.py` | `normalize_text_with_map()` / `remap_span()` — new corpora carry `pii_spans` |
| `annotate_gold_spans.py` | back-fills `pii_spans` onto corpora **already built and already benchmarked**, so the 20k corpus keeps its case names and every document-level row stays comparable |
| `pii_char_f1_eval.py` | runs a detector, scores characters, and **writes every predicted span to disk** |
| `pii_char_f1_report.py` | re-scores those span files offline — by label, by span length, by decision rule |

#### Why the logs cannot support it

Character F1 needs two lists of *character positions* per document: which chars
are PII (gold) and which the model said are PII (prediction). **Neither side has
positions.** Predictions are a type set (§1.1); gold is a comma-joined type list
(§2). `grep -cE '\[[0-9]+:[0-9]+\]'` returns 0 across every log dialect. Both
sides know *what kinds*; character F1 needs *where*. No re-parsing recovers it —
it needs a corpus rebuild (§2) plus re-inference.

#### The move that dissolves the taxonomy problem

Stop asking what the model called a span; ask which characters it covered. A
span becomes a set of character indices:

```
pplx: account_number        [107:118]  ->  {107..117}
gold: medical_record_number [107:118]  ->  {107..117}
```

Identical sets. **`account_number` vs `medical_record_number` vs `BANKACCOUNT`
vs `CREDIT_CARD` never enter the arithmetic**, so no interlingua is needed and
`pii_taxonomy.py`'s editorial judgment (§8.7) stops being load-bearing. Then
ordinary set F1: precision = correct chars / flagged chars, recall = correct
chars / gold chars.

#### Three tiers, only the last needing the taxonomy

1. **Label-agnostic character F1** — union all predicted spans vs all gold
   spans. Zero taxonomy. The headline number, comparable across all detectors.
2. **Within-schema per-label character F1** — models sharing a label vocabulary
   compare label-for-label with no mapping: pplx + openai/privacy-filter (same
   9-category schema, per `pii_taxonomy.py`'s own note); OpenMed v1 + v2;
   mmBERT alone on Presidio's 17. **GLiNER is the special case** — it is handed
   the corpus's own 55 gold label names at inference, so it compares to gold
   *directly*, making it the only calibration reference needing no mapping.
3. **Cross-schema** — still via `pii_taxonomy.py`, but the mapping becomes a
   hypothesis that tiers 1-2 can falsify rather than an axiom.

#### Why it matters more than another model row

**It measures precision without a benign arm.** §2.1's limitation — precision is
empty at n_benign=1, and an indiscriminate detector scores like a precise one —
holds only for *document-level* scoring. At character level an over-tagging model
is penalized on *positive* documents. This does not retire the benign-arm work
(§9.1): the ceiling is the document's PII density, so flagging every character
scores precision ~0.24 here, not 0. But it stops precision being *completely*
unmeasured, and it is the only way to rank the models currently tied at
0.00-0.24% leak rate.

**Prefer character F1 over span-exact F1.** Models fragment: mmBERT emits one ID
number as 6 pieces (`' BH'`, `'-'`, `'0'`, `'0'`, `'02874'`, `'5'`), pplx emits
one email as `' d'`,`'aniel'`,`'@'`,... Span-exact F1 scores near-zero for
correct detections; character F1 measures coverage and handles fragmentation
correctly.

#### Worked example: case `nemotron-pii-15485`

317 chars, 76 of them gold PII (24%). Document-level, **every** model scores a
clean `[PASS][TP]`.

```
txt  My medical record number is BH-00028745. Are you currently employed? Yes, I a
gld .............................GGGGGGGGGGG......................................
ppl .............................ppppppppppp......................................
mmb ............................mmmmmmmmmmmm......................................
txt m employed full-time. What is your occupation? My occupation is medical health
gld ..GGGGGGGGGGGGGGGGGG............................................GGGGGGGGGGGGGG
ppl ..............................................................................
mmb ..............................................................................
```

| | spans | chars flagged | correct | precision | recall | **char F1** |
|---|---:|---:|---:|---:|---:|---:|
| pplx | 2 | 27 | 27 | 1.000 | 0.355 | **0.524** |
| mmBERT | 18 | 36 | 35 | 0.972 | 0.461 | **0.625** |

mmBERT here is scored under the **argmax** rule (`pii_ner_eval.py`'s), not the
router's `min_score` rule — per §6.4 those are different decision rules, and any
character-level work has to state which one it scored, since the stricter rule
would drop spans and move both precision and recall.

Both are blind to 49 of 76 gold PII chars (`employed full-time`,
`medical health services manager`) while scoring a perfect document-level result.
That gap is the whole argument.

**And it corrects a conclusion the label-set view invites.** mmBERT emits nine
labels on this disability questionnaire — `CREDIT_CARD`, `IBAN_CODE`, `US_SSN`,
`US_DRIVER_LICENSE`, `PHONE_NUMBER`, `STREET_ADDRESS`... none of which the
document contains. Read as labels, that is nine errors. Read as characters,
**35 of its 36 flagged chars are genuinely PII (precision 0.972)**: it found both
ID numbers and shredded them into 17 mislabeled fragments. The labels are wrong;
the *detection* is not. Label-set scoring cannot separate "wrong label" from
"wrong location" — character F1 is the metric that can, and mmBERT's §5.1
per-category numbers should be read with that distinction in mind.

Note mmBERT edges pplx here (0.625 vs 0.524) only by grabbing a leading space
and `' services'`. Whether its 0.12%-vs-0.80% document-level advantage survives
character scoring, or was partly a reward for spraying, is exactly the open
question.

**Caveats on the numbers above.** Single document, and the gold spans are
*reconstructed* by locating the four gold entity strings (the builder discarded
the real offsets), with `employed full-time` / `medical health services manager`
boundaries inferred. These are illustrative of the method, **not results of
record** — no character-level number belongs in §6 until the builder emits real
offsets.

#### Cost, and what it is not

Re-inference is the whole cost: span outputs were never written down, so getting
them means re-running each model over all 20,000 documents. From §6's own
column that is ~1-2.5 hr per model on this CPU, so **≈10-15 hr sequential** for
six detectors. Only OpenMed-v2 and the ONNX variants overlap with the re-runs
already queued in §9.6 — the rest is new compute, not work already planned.

**Character F1 is undefined for the LLM routers and the embedding classifiers.**
They emit a routing decision and never a span. Those rows stay document-level
permanently; that is a property of the method, not a gap to fill.

That cost is now paid once and only once, because `pii_char_f1_eval.py` writes
every predicted span to `<run>.spans.jsonl`. **Recording the spans is the fix
for the thing that made this expensive** — no earlier run in the series wrote
down *where* a model fired, only which label names it emitted, so every new
question meant a new pass over 20,001 documents. Per-label scoring, a different
threshold, a span-length breakdown and span-exact as a cross-check now all read
that file. `pii_char_f1_report.py` is that reader, and it runs against a live
run's partial file too.

#### Recovering the gold offsets without resampling the corpus

The builder change alone would have forced a rebuild, and a rebuild resamples:
a fresh corpus is a different document set, so every document-level number ever
measured against `l2_pii_nemotron_20k` would stop being comparable to the
character numbers sitting beside it. `annotate_gold_spans.py` avoids that. It
matches each existing case back to its source row **by normalized text**, then
applies the index map and the resolved prefix length to that row's raw offsets.

Result on the 20k corpus: **20,001 / 20,001 cases matched, 170,974 gold spans
written, 0 dropped, 0 failing the offset check.** Gold PII is 2,376,455 of
16,304,802 non-whitespace prompt characters — a **PII density of 0.1458**,
which is the number §5.5 needed and had only estimated (§2.1's "~0.24" came
from one document).

Density is quoted on the **non-whitespace** denominator because that is what
the metric uses on both sides. Dividing whitespace-excluded gold by
whitespace-inclusive prompt length gives 0.1252 and understates the
flag-everything floor; the first version of these scripts did exactly that.

Two things that check the recovery rather than assume it:

- **Every remapped span is verified against the prompt it now indexes into**,
  not just produced. `content[start:end]` must equal the span's own text after
  the same whitespace collapse.
- **That comparison is case-insensitive, and that is a correction, not a
  loosened check.** The dataset's `text` field is a canonicalized copy of the
  entity while `start`/`end` point at the document's true casing —
  `'compliance officer'` vs `Compliance Officer`, `'male'` vs `Male`,
  `'ekaterina.ivanov@kreditexpress.ru'` vs `...@kreditExpress.ru`. 1,418 of
  170,974 spans differ that way and **zero differ in any other way**. A
  case-sensitive check silently discards 1,418 correctly located spans; the
  first run of the annotator did exactly that before the mismatches were
  classified rather than counted.

### 5.6 Character-level results

**The pipeline reproduces every known document-level number before any
character number is quoted from it.** That gate matters more than the character
figures themselves, because the whole apparatus is new: a new corpus
annotation, a new inference path, a new scorer. If it had disagreed with §6 by
a single case, the character numbers would be measuring the harness.

| Check | §6 / §6.4 says | This pipeline |
|---|---|---|
| mmBERT leaks, argmax rule | 25 / 20000 | **25** |
| mmBERT leaks, `min_score` 0.5 | 49 / 20000 | **49** |
| argmax leaks ⊆ `min_score` leaks | 25/25 contained | **0 violations** |
| threshold-only leaks | 24 | **24** |
| mmBERT on the one benign case | flagged (FP=1) | **FP=1** |
| documents truncated | none (§7) | **0** |

Reached independently: this scorer shares no code with `pii_ner_eval.py` or the
router, and it runs **both** rules over the same ONNX graph in the same
process. §6.4 argued the 0.12% → 0.24% gap was the decision rule and not the
backend by a containment check across two different runs; here the backend is
held literally constant and the same gap appears, which is as direct as that
claim can be made.

#### The headline table

Label-agnostic character F1 on `l2_pii_nemotron_20k`, whitespace excluded from
both sides, each model under its own primary rule.

| Model | Rule | char P | char R | **char F1** | macro F1 | doc leak | Runtime |
|---|---|---:|---:|---:|---:|---:|---:|
| **privacy-filter** (OpenMed ml-v2) | argmax | 0.9769 | 0.9359 | **0.9559** | 0.9568 | 0.00% | *in flight* |
| **pplx-pii-masking** | viterbi | 0.9725 | 0.7330 | **0.8360** | 0.8379 | 0.795% (159) | 2.27 hr |
| **mmBERT32K-PII** | argmax | 0.9202 | 0.6842 | **0.7849** | 0.8045 | 0.125% (25) | 0.40 hr |
| *flag-everything strawman* | — | *0.1458* | *1.0000* | *0.2544* | — | *0.00%* | — |

privacy-filter's row is from **15,838 of 20,001 cases** and is marked *in
flight*; its figures moved by <0.002 between 1% and 79% coverage, but it is not
a number of record until the run finishes. The corpus is shuffled, so a prefix
is a random sample rather than a biased slice.

**This is the ranking document-level scoring could not produce.** Those three
models sit at 0.00%, 0.125% and 0.795% document leak — a spread of under one
percentage point, all of it inside the noise of "essentially perfect" — and
they spread across **0.16 of character F1**. The ordering is not even the same:
mmBERT leaks 6x fewer documents than pplx and is the *worse* model by
characters, because it fires *somewhere* on almost every document while
covering far less of what is actually there.

Every char precision is 0.92-0.98, well clear of the 0.1458 strawman. **That
is the first precision measurement in this series that means anything** (§2.1),
and it says none of the three over-tags.

#### The other decision-rule finding: pplx's own decoder costs it 75 documents

| pplx rule | char P | char R | char F1 | doc leak |
|---|---:|---:|---:|---:|
| argmax | 0.9627 | 0.7328 | 0.8322 | **0.42% (84)** |
| `min_score` 0.5 | 0.9666 | 0.7190 | 0.8246 | 0.445% (89) |
| viterbi (shipped) | 0.9725 | 0.7330 | **0.8360** | **0.795% (159)** |

**The constrained BIOES Viterbi is stricter than argmax, not merely better.**
It nearly doubles the leak rate — 84 documents to 159 — because a valid BIOES
path suppresses isolated `I-` / `E-` predictions that argmax happily emits, and
those isolated firings are exactly the marginal detections a router still wants
to act on. In exchange it buys precision (0.9627 → 0.9725) and a slightly
better char F1.

This is new and it is not what §4.2 or §6.2 measured. Those held the decoder
fixed on both sides and varied the backend, correctly, to prove the ONNX export
was decision-identical to safetensors. Nobody had varied the decoder. **For
routing, pplx is better served by argmax than by its own decoder; for masking,
the reverse.** Do not assume a constrained decoder is a free improvement.

#### Per-label character recall, and what §5.1 could not see

`pii_char_f1_report.py --by-label` scores each gold label over the characters
carrying it. Unlike §5.1 this needs **no interlingua** — a prediction either
covers those characters or it does not — so `pii_taxonomy.py`'s editorial
judgment (§8.7) is not in the loop, and there is no lenient-vs-strict question
to settle (§5.2). Selected rows, sorted by gold character support:

| Gold label | gold chars | mmbert | pplx | privacy-filter\* |
|---|---:|---:|---:|---:|
| url | 401,181 | 47.6% | 78.6% | 99.1% |
| email | 261,331 | 91.7% | 99.7% | 99.5% |
| company_name | 189,208 | 58.3% | **1.2%** | 84.0% |
| date | 147,817 | 89.7% | 76.0% | 92.8% |
| occupation | 135,450 | **3.7%** | **1.4%** | 57.0% |
| first_name | 99,871 | 86.0% | 99.1% | 98.4% |
| http_cookie | 80,175 | 36.6% | 78.3% | 98.5% |
| last_name | 78,681 | 95.3% | 98.9% | 98.6% |
| phone_number | 58,429 | 98.9% | 99.9% | 99.9% |
| api_key | 42,602 | 70.1% | 99.8% | 97.2% |
| credit_debit_card | 39,246 | 99.7% | 99.8% | 100.0% |
| **time** | 38,949 | **60.6%** | **46.7%** | **75.2%** |
| city | 28,884 | 73.3% | 63.2% | 95.1% |
| education_level | 26,415 | **0.5%** | **2.7%** | 75.6% |
| county | 25,617 | 43.0% | 45.2% | 94.1% |
| state | 24,575 | 38.9% | 48.5% | 92.9% |
| country | 24,298 | **23.3%** | **28.4%** | 97.5% |

\* privacy-filter at 79% coverage; see the note above.

Three readings that the document-level view could not reach:

- **A coverage hole is not a clean zero, and that matters.** §5.1 reports
  `no class` for pplx on ORG_COMPANY and OCCUPATION_EMPLOYMENT — a declared
  gap, scored as neither hit nor miss. In characters those come out at **1.2%**
  and **1.4%**, not 0%: pplx does occasionally cover a company name or a job
  title, incidentally, via `other_pii` or by swallowing it inside an adjacent
  span. The gap is real and the honest number is "essentially nothing", but
  `no class` and 1.2% are different claims and only one of them is measured.

- **Geographic granularity is where the two smaller models actually break.**
  `country` 23.3% / 28.4%, `state` 38.9% / 48.5%, `county` 43.0% / 45.2% for
  mmBERT and pplx, against 92.9-97.5% for privacy-filter. §5.1 folds all of
  these into one `ADDRESS_LOCATION` row (91.3% / 74.7%) where the collapse is
  invisible, because a model that finds the street address scores the category
  while missing the country entirely.

- **`time` is a shared blind spot nobody had flagged**: 60.6% / 46.7% / 75.2%,
  the weakest row on which *all three* models are simultaneously poor, on
  38,949 gold characters. §5.1's `DATE_TIME` row reads 99.2% / 84.8% / 97.4%,
  because `date` is easy and dominates the category. Splitting them is a
  character-level result and a real finding: **times are much harder than
  dates for every detector measured.**

`--by-length` adds one more: recall falls off at both ends for mmBERT and pplx
(short entities to subword fragmentation, 40+ char entities to partial
coverage) while privacy-filter stays above 90% across every bucket.

#### What the document-level table could not see

**`min_score` is nearly free for routing and expensive for anything else.**
On mmBERT the shipped 0.5 threshold moves the document leak rate 0.12% → 0.24%
— two negligible numbers, and §6.4's tuning curve reads as though the choice
barely matters. In characters the same threshold costs **char recall 0.6842 →
0.4849 and char F1 0.7849 → 0.6419**. Half the missing PII characters are
sub-threshold. A router only has to notice one entity, so it never pays that
bill; a redaction or masking path pays all of it. The two use cases want
different thresholds and the document-level table cannot show that.

Note the direction of the precision/recall trade, too: `min_score` *raises*
char precision (0.9202 → 0.9494) while collapsing recall. It is a strictly
more conservative rule in exactly the way the containment predicts.

## 6. Consolidated results

These are the numbers of record. Every cell was verified against the run logs
on 2026-09-08; where a value disagreed with the earlier spreadsheet, the log
wins and the log's value is what appears here. §6.1 records what changed and
why — one of those changes inverts a conclusion, so it is worth reading before
reusing any older copy of this table.

Transposed relative to the original spreadsheet (models as rows) because 15
columns do not survive a markdown table.

| Model | Corpus | Leak rate (PII → cloud) | Recall | E2E runtime | Source log |
|---|---|---|---|---|---|
| Regex | 2.5k | 18.7% (467/2500) | 81.30% | — | `policy_20260809-155705` |
| LLM Qwen3.5-9B | 2.5k | 6.16% (154/2500) | 93.84% | ~6hr (unverified) | `policy_llm_20260808-170329` |
| LLM Qwen3.5-2B | 2.5k | 3.28% (82/2500) | 96.72% | 2.90hr | `policy_llm_qwen3.5-2B_20260812-192954` |
| LLM Qwen3.5-0.8B | 2.5k | 2.84% (71/2500) | 97.16% | 2.21hr | `policy_llm_20260812-230020` |
| embeddinggemma-300m | 20 | 100% (20/20) | 0.00% | 29s | `policy_20260810-192605` |
| Qwen3-Embedding-0.6B | 20 | 90% (18/20) | 10.00% | 59s | `policy_20260811-094251` |
| Qwen3-Embedding-4B | 20 | 90% (18/20) | 10.00% | 6.8min | `policy_20260811-094735` |
| mmBERT32K-PII (safetensors) † | 20k | 0.12% (25/20000) | 99.87% | 1.01hr | `ner_mmbert32k-...-194327` |
| mmBERT32K-PII (ONNX/Lemonade) † | 20k | 0.24% (49/20000) | 99.76% | 7.19hr | `policy_smoke_20260819-100935` |
| OpenMed/privacy-filter-multilingual | 20k | 0.07% (14/20000) | 99.93% | 2.15hr | `ner_privacy-filter-multilingual_...-194332` |
| OpenMed/privacy-filter-ml-v2 (safetensors) | 20k | 0% (0/20000) | 100% | resumed; partial | `ner_privacy-filter-multilingual-v2_...-224148` |
| OpenMed/privacy-filter-ml-v2 (ONNX/Lemonade) | 20k | 0% (0/20000) | 100% | 8.12hr | `pf_router_full20k_stdout_20260819` |
| GLiNER (nvidia/gliner-PII) | 20k | 0.005% (1/20000) | 99.995% | resumed; partial | `gliner_gliner-PII_...-211156` |
| OpenAI/privacy-filter | 20k | 5.31% (1062/20000) | 94.69% | resumed; partial | `ner_privacy-filter_...-224152` |
| **perplexity-ai/pplx-pii-masking** | 20k | **0.80% (159/20000)** | **99.20%** | **2.55hr** | `pplx_f1f90a53..._20260908-173941` |
| **pplx-pii-masking (ONNX/onnxruntime)** | 20k | **0.80% (159/20000)** | **99.20%** | 2.17hr (split run — see §6.2) | `pplx_onnx_20260908-214405` |

Routing / model-eval split, where the router logs report one:

| Model | E2E | Routing | Routed-model eval |
|---|---|---|---|
| LLM Qwen3.5-2B | 2.90hr | 2.47hr | 21min |
| LLM Qwen3.5-0.8B | 2.21hr | 1.86hr | 18min |
| mmBERT32K (ONNX) | 7.19hr | 4.60hr | 2.59hr |
| OpenMed v2 (ONNX) | 8.12hr | 7.05hr | 1.06hr |

All runs are CPU. pplx safetensors is fp32 on 16 threads, single clean pass over
all 20,001 cases with no resume. **The pplx ONNX runtime is not a usable
comparison** — that run was interrupted by a machine restart at 4,433 cases and
resumed, and onnxruntime ran at its default thread count while torch used all
16. Quote the 2.55hr safetensors figure; re-run uninterrupted with
`--intra-op-threads` pinned if a real backend-vs-backend timing is wanted.

`Rationale behind routing` (99.5% / 59.4% / 18.30% for the three LLMs) and the
`Prompt misses` / `Genuine misses` split are **not recoverable from the logs** —
no log field records them, and PASS lines carry no rationale. They are
internally consistent (129+24=153, 61+21=82, 40+31=71) and are reproduced here
on trust, not verified. Note the 9B's misses sum to 153 while its leak count is
154: the eval counts one HTTP error as a leak.

pplx's analogue of that split, which *is* derivable, is a coverage split:
**153/159 (96%) of its misses were on documents its taxonomy can express**
(real detection failures), and only 6/159 (4%) were documents whose entire gold
set lies outside its 9 categories. This is overwhelmingly a detection problem,
not a coverage problem.

pplx missed categories, ranked by enrichment (miss rate ÷ 0.795% baseline)
rather than raw count: education_level 3.9x, language 3.7x, political_view
3.5x, religious_belief 3.1x, country 3.0x, race_ethnicity 2.8x, sexuality 2.6x,
url 2.1x. By document domain the concentration is sharper — **Social Science
8.8x** (53/754 missed), Environmental 5.0x, Elections 4.2x, Sports 3.6x.

### 6.1 What changed from the earlier spreadsheet

**The embedding rows were inverted, and this reverses their conclusion.** The
earlier spreadsheet recorded 0/20, 2/20, 2/20 as leak counts, which read as the best
results in the table. Those are the `PII -> local (TP, correct)` values. The
actual `PII -> cloud (LEAK, FN error)` values are **20/20, 18/20, 18/20** —
leak rates of 100%, 90%, 90% and recalls of 0.000, 0.100, 0.100. The
`semantic_similarity` embedding classifiers were the **worst** configurations
tested, not the best. From `policy_20260810-192605.log`:

```
  PII -> local   (TP, correct)    :   0 / 20
  PII -> cloud   (LEAK, FN error) :  20 / 20
  Leak rate (FN / sensitive)     : 100.0%
  Recall                         : 0.000
```

Also note n=20 with zero benign cases, so these rows were never load-bearing
regardless of sign.

**GLiNER's denominator was a case name.** `1/18787` should be `1/20000`;
`nemotron-pii-18787` is the ID of a passing case in the log, not a total. The
run's own matrix reads `19999 / 20000` with `errors: 0`. Recall is 99.995%.

**Runtime drift.** Qwen3.5-2B E2E is 2h54m (2.90hr), not 2.5hr. The mmBERT ONNX
routing/model split of 4.3 + 2.3hr sums to 6.6hr against a 7.19hr E2E; the
logged percentages give 4.60 + 2.59 = 7.19hr. Qwen3-Embedding-4B took 407s
(6.8min), not 4min.

**One row rests on an incomplete run — but not the one it appears to.** The
direct ONNX eval `onnx_privacy-filter-ml-v2_20260819-215329.log` stalled at
1,786 / 20,001 cases with no summary. The OpenMed-v2 ONNX row is nonetheless
sound: it comes from the completed *router* run
(`pf_router_full20k_stdout_20260819`, 0/20000, `errors: 0`).

### 6.2 The ONNX export is decision-identical to safetensors (measured)

The only ONNX row here whose equivalence was verified rather than assumed.
`pii_pplx_onnx_eval.py --parity-against <safetensors log>` diffs the two runs
case by case; the block lands in the run's JSON summary.

| Check | Result |
|---|---|
| Confusion matrix | **19841 / 159 / 1 / 0 — identical, both runs** |
| has_pii decision agreement | **20,001 / 20,001 (100.0000%)** |
| Routing decision flips | **0** |
| Exact label-set agreement | **20,001 / 20,001 (100.0000%)** |
| The 159 missed documents | **the same 159 case names**, set difference empty both ways |
| Per-category recall, lenient + strict | identical in every cell (§5.1.1) |
| max \|Δsensitivity\| | 0.0010 — *is* the log's 3dp rounding floor, i.e. below resolution |
| Logit max-abs-diff, canned sample | 1.5e-5 (logits), 2e-6 (sensitivity) |
| Logit max-abs-diff, real 806-char corpus doc | 1.0e-5 (logits), 0.0 (sensitivity) |

Identical counts alone would not prove this — they can hide compensating errors.
The same-159-case-names check is what closes that gap.

Note `RESULT: FAIL` in both logs: it is the scripts' convention for `FN != 0`,
not a failed run.

**One caveat on how this number was produced.** The ONNX run was interrupted and
resumed, and `--resume-from` initially restored only the *primary* counts — so
the run first reported a sensitivity-head recall of 0.0929 scoped to the 15,568
post-resume cases while every primary metric covered all 20,001. Two
denominators in one summary. Recomputed over the full corpus both backends give
**0.09160** with 1,832 documents flagged, and the resume path now rebuilds that
tally. If you resume any run in this series, check that the secondary
denominators match the primary ones before quoting.

### 6.3 What the leak-rate column cannot tell you

**Since §5.6 exists, the first answer is "read §5.6".** Character F1 ranks the
three ONNX detectors that this column cannot separate, and it supplies the
precision number this column structurally cannot.


Repeating §2.1 because this table is what circulates: **with one benign case,
the precision side is unmeasured.** A model that flagged every document would
show a perfect leak rate here. For the record, on that single benign case
mmBERT, OpenMed-v2 and GLiNER all flagged it (FP=1, over-route 100%), while
OpenMed-v1, OpenAI/privacy-filter and pplx did not. At n=1 that separates
nothing — it is listed only to make the gap concrete.

### 6.4 † The two mmBERT rows measure different decision rules, not backends

The 0.12% → 0.24% gap between the mmBERT safetensors and ONNX/Lemonade rows is
**entirely the decision rule**. The backend contributes nothing. Both halves of
that were measured, not argued.

The rules differ:

| Path | Rule |
|---|---|
| `pii_ner_eval.py` (safetensors) | argmax per token; fire if any non-special token's argmax is not `O` |
| the router (ort-server) | softmax per token → max over tokens per label → fire if any non-`O` label ≥ `min_score` |

A per-token softmax over 35 labels sums to 1, so a label above 0.5 at a token
*is* that token's argmax. **`min_score` ≥ 0.5 is therefore strictly stricter
than argmax** — a containment, not a tunable equivalent.

| Check | Result |
|---|---|
| Backend, same argmax rule (13,708 shared cases) | **0 entity-set mismatches, 0 has_pii flips, FN sets equal** |
| — that check's source | `onnx_mmbert32k-pii_...-194225`, an interrupted direct-ONNX argmax run covering 13,708 of 20,001. Partial coverage is fine here: it is a per-case identity check, not a rate |
| Router leaks ⊇ safetensors leaks | **25 / 25 contained, 0 violations** |
| The 49 = the 25 + threshold-only misses | **24 threshold-only** |
| `min_score` the run actually used | bracketed to **(0.4963, 0.5043]** by max(leaked) / min(non-leaked) |

`pii_min_score_sweep.py` re-scores the corpus under the router's rule directly
on the ONNX export (one forward pass per case, ~28min for 20,001 — the 7.19hr
row is HTTP round-trips plus the routed model answering, not classification).
It reproduces **both** ground-truth runs set-for-set: 49/49 at 0.5, and 25/25 on
its argmax column. `--self-check <router log>` is that gate.

**Special tokens are excluded from the max, and this was measured.** Including
them puts all 49 known leaks at ≥ 0.5 — this model's `<bos>` always fires a
label, the artifact `pii_ner_eval.py` documents. Excluding them reproduces the
router exactly. No leaked case was truncated, so `max_length` is not a factor
either.

**The tuning curve is one-sided and must not be read as advice.** Per §2.1 there
is one benign case, so lowering `min_score` has no measurable cost here:

| `min_score` | leaks | recall | vs argmax |
|---|---|---|---|
| 0.10 | 9 | 99.955% | −16 |
| 0.20 | 18 | 99.910% | −7 |
| 0.30 | 25 | 99.875% | +0 |
| 0.50 (shipped) | 49 | 99.755% | +24 |
| 0.70 | 119 | 99.405% | +94 |

0.30 matching argmax's *count* is a coincidence — the sets differ (21/25
overlap). There is no threshold at which the two rules agree; they are different
rules, not reparameterizations.

And the apparent headroom is worth about half what it looks like. Of the 24
threshold-only leaks, the sub-threshold signal sits on a **category the document
does not contain in 13 of 24 cases**. Those recover by accident. For a binary
route-or-not decision an accidental catch still routes correctly, but the same
spurious firing is exactly what would cost precision on benign traffic — the
thing this corpus cannot measure. **Do not lower `min_score` off this curve
without a benign arm.**

## 7. Context length is a non-issue on this corpus

The longest document is **1,737 tokens** — 42% of pplx's 4,096 cap. **Zero
documents truncate for any model.** The mmBERT-32k / OpenMed-128k / pplx-4k
comparison is apples-to-apples here; none of them ever gets to use context past
~1.7k, so their context specs could not have differentiated them.

Also note pplx's 4,096 is **not an architectural ceiling** — it is the
fine-tuning window on the head config. The backbone is a bidirectional Qwen3
encoder with `max_position_embeddings: 32768` and `rope_theta: 1e6`.
`pii_pplx_eval.py` chunks on token-offset boundaries with 128-token overlap
above the cap rather than truncating (the mitigation the model card recommends);
on this corpus that path stays inert.

Context length matters for *deployment* — RAG-stuffed prompts, long chat
histories, pasted documents — not for these results.

## 8. Traps, in the order they cost time

1. **Transcribing the wrong column out of a routing confusion matrix.** The
   router matrix prints `PII -> local (TP, correct)` *above*
   `PII -> cloud (LEAK, FN error)`. Reading the first as the leak count inverts
   the result. This put three embedding classifiers in the summary table as
   0/20, 2/20, 2/20 — apparently the best rows in it — when they had actually
   leaked 20/20, 18/20, 18/20 and were the worst. §6.1.
2. **Reading recall as detector quality.** With n_benign=1, recall alone cannot
   distinguish a precise detector from one that flags everything. §2.1.
3. **Reading "missed categories" as model weakness.** It conflates a genuine
   detection failure (mmBERT / NRP at 5%) with a taxonomy the model never
   claimed (pplx / religion). §5.1.
4. **Reading `detected=` as an entity count, or a count mismatch as
   under-detection.** It is a deduplicated *type* set: two spans of the same
   type appear once, eight emails appear once. "4 gold categories vs 1 predicted
   label" is normally two collapses stacked, not a miss. §1.1.
5. **Judging a model by its label names alone.** mmBERT emits nine wrong labels
   on case 15485 yet 35 of its 36 flagged characters are genuinely PII. Wrong
   label and wrong location are different failures and the label-set logs cannot
   tell them apart. §5.5.
6. **Filtering categories out of the report.** `--min-support` defaults to 0 for
   exactly this reason. An earlier `--doc-level` built the row list from
   categories *some model covered*, which silently dropped BIOMETRIC (1,958
   docs) and EDUCATION (1,416) — the two biggest shared blind spots — from the
   table entirely.
7. **Trusting a mapping because the number looks plausible.** Two mappings were
   wrong on the first pass: `ONLINE_IDENTIFIER` lumped URLs with IPs, MACs and
   usernames (penalizing pplx, which only claims URLs), and
   `EYECOLOR`/`HEIGHT` → `BIOMETRIC` produced a fake "0.2% biometric failure"
   for OpenMed that was purely a bad mapping. Both are now split
   (`WEB_URL` / `NETWORK_ID` / `ACCOUNT_HANDLE`, and `PHYSICAL_ATTRIBUTE`).
   **The mappings in `pii_taxonomy.py` are editorial judgment and should be
   reviewed, not assumed.**
8. **Routing on pplx's sensitivity head.** 9.16% recall vs 99.20%. §4.1.
9. **Grepping `[FAIL][FN]` against a routing log.** Returns zero, looks perfect,
   is wrong. §3.1.
10. **Running without `--verbose`.** Destroys resume and all re-scoring. §4.
11. **Scoring an ONNX export by argmax when its safetensors run used a span
    decoder.** The pplx graph stops at raw logits on purpose; an argmax
    comparison measures decoder-vs-decoder, not backend-vs-backend, and the
    delta means nothing. Reuse the checkpoint's own decoder. §4.2.
12. **Exporting pplx from the `-vllm-tmp` repacking.** Its top-level
    `"is_causal": false` is a key stock transformers never reads, so the export
    is silently *causal* instead of bidirectional while looking entirely
    self-consistent. §4.2.
13. **Assuming an interrupted run's resumed summary is whole.** `--resume-from`
    reconstructs tallies from the log; a metric it forgets to restore ends up
    reported over a different denominator than the rest of the summary. This
    happened to pplx-ONNX's sensitivity head (15,568 vs 20,001). §6.2.
14. **Comparing a safetensors run to a router run and calling the delta a
    backend difference.** `pii_ner_eval.py` scores by argmax; the router scores
    by `min_score` over max-aggregated softmax, which at 0.5 is *strictly
    stricter*. The mmBERT rows' 2x gap was entirely this, with the backend
    contributing exactly zero. §6.4.
15. **Concluding from a document-level delta that a decision rule "barely
    matters".** mmBERT's `min_score` 0.5 moves the leak rate 0.12% → 0.24%, two
    negligible-looking numbers. The same threshold moves **char recall 0.6842 →
    0.4849**. A router needs to notice one entity per document and never pays
    that bill; a masking path pays all of it. §5.6.
16. **Assuming a constrained decoder is a free improvement.** pplx's own BIOES
    Viterbi *nearly doubles* its document leak rate against plain argmax (159
    vs 84) because valid-path constraints suppress isolated `I-`/`E-` firings.
    It buys precision and costs recall — a trade, not an upgrade. §5.6.
17. **Verifying recovered gold spans case-sensitively.** Nemotron's `text`
    field is a canonicalized copy of the entity while `start`/`end` point at the
    document's true casing, so 1,418 of 170,974 spans differ in case and *zero*
    differ any other way. A case-sensitive check silently discards 1,418
    correctly located spans and looks like a 0.8% offset bug. Classify the
    mismatches before trusting the count. §5.5.
18. **Dividing whitespace-excluded gold by whitespace-inclusive text.** The
    character metric drops whitespace from both sides, so the PII-density floor
    has to use the same denominator: 0.1458, not the 0.1252 that the mixed
    basis gives. The strawman a model must beat gets quietly easier otherwise.
19. **Benchmarking several detectors in parallel on one box.** Three ONNX
    processes at 10 intra-op threads each on **16 physical cores** is 2x
    oversubscription, and throughput collapsed ~8x — pplx fell from 2.1 to 0.27
    cases/s, turning a 7hr sequential plan into a 20hr one. Run detectors
    sequentially at full thread count. Any runtime figure from a parallel run
    measures scheduler contention, not the model.

## 9. Open work

Roughly in order of value:

1. **Build a benign arm** (§2.1). Without it, half of every confusion matrix in
   this series is decorative, and it changes the meaning of every existing row
   retroactively. Do this before benchmarking more models.
2. ~~**Preserve span offsets, then build character-level F1.**~~ **Done** for
   the three ONNX detectors — §5.5 for the method, §5.6 for the results. The
   corpus carries `pii_spans`, every predicted span is on disk, and
   `pii_char_f1_report.py` re-scores without re-inference. What remains under
   this heading is **tier 3 of §5.5**: cross-schema per-label character F1, and
   character scoring for the models not yet covered (GLiNER, OpenMed v1,
   OpenAI/privacy-filter, mmBERT safetensors). Those are now cheap to *score*
   and still cost a re-inference pass each to *collect*.
3. **Review `pii_taxonomy.py`'s mappings** (§8.7).
4. **Commit the policy JSONs that back table rows.** `policy_smoke.json`
   produced the mmBERT-ONNX row and is not in the repo, so its `min_score` had
   to be recovered from the run itself — the threshold is bracketed by
   max(score among leaked) < T <= min(score among non-leaked), which pinned it
   to (0.4963, 0.5043]. That works only because a full re-score existed; without
   one, a headline row rests on an unrecoverable setting. Registered policies
   belong in `<corpus>/` beside the log, like
   `l2_pii_onnx_classifier/policy.json` already is.
5. **Re-run the three embedding classifiers properly.** Their rows were
   inverted (§6.1) and rested on n=20 with zero benign cases. As corrected they
   leak 90-100%, which — if it survives a real run — is a finding worth stating
   deliberately rather than a cell to quietly fix.
6. Re-run OpenMed v2 and the remaining ONNX variants with `--verbose` on the 20k
   corpus if per-category numbers are wanted for the rows currently backed only
   by routing logs. **Done for pplx** (§6.2): its ONNX row comes from a direct
   verbose eval, not a router log, which is why it is the only ONNX row with
   per-category numbers and a measured parity claim. mmBERT-ONNX and
   OpenMed-v2-ONNX still rest on router logs. **mmBERT-ONNX's 0.24% vs its
   safetensors 0.12% is now explained** (§6.4): the backend is decision-identical
   and the whole gap is argmax vs `min_score`. OpenMed-v2-ONNX's row has not had
   the same treatment and carries the same confound.
7. **Re-time the pplx ONNX row on an uninterrupted run** with
   `--intra-op-threads` pinned. The current 2.17hr is a resumed,
   default-threaded run and is not comparable to the 2.55hr torch figure (§6).

## 10. Files

| File | Purpose |
|---|---|
| `build_nemotron_corpus.py` | Samples Nemotron-PII into `cases.jsonl` + `stats.json` |
| `pii_ner_eval.py` | Standard HF token-classification models (mmBERT, OpenMed) |
| `pii_gliner_eval.py` | GLiNER zero-shot (labels supplied at inference) |
| `pii_pplx_eval.py` | `perplexity-ai/pplx-pii-masking` (custom arch, dual heads) |
| `pii_pplx_onnx_eval.py` | Same model via onnxruntime + the checkpoint's real Viterbi; `--parity-against` diffs a safetensors run case by case |
| `pplx_pii_masking2onnx.py` | Exports pplx-pii-masking to ONNX (two named outputs, no decoder baked in) |
| `compare_pii_onnx_vs_safetensors.py` | Single-text logit-level ONNX vs safetensors diff, for pplx and privacy-filter-ml-v2 |
| `setup_pplx_pii_masking_onnx.py` | Places the export beside the HF snapshot + writes `manifest.json` for Lemonade's ort-server |
| `generate_pplx_pii_masking_onnx_policy.py` | Emits the router policy for the registered ONNX model |
| `pii_routing_eval.py` | Full router replay — regex, LLM-as-router, ONNX classifier policies |
| `pii_min_score_sweep.py` | Re-scores a corpus under the router's own rule (softmax → max over tokens) on the ONNX export; `--self-check` reproduces a router run's leaks as a gate |
| `pii_min_score_curve.py` | Turns that sweep into the `min_score` tuning curve — arithmetic, no re-inference |
| `pii_taxonomy.py` | Canonical 24-category interlingua + per-model mappings |
| `pii_category_recall.py` | Re-scores existing logs per category; `--strict`, `--doc-level` |
| `annotate_gold_spans.py` | Back-fills verified gold span offsets (`pii_spans`) onto an already-built corpus, matching each case to its source row by normalized text |
| `pii_char_f1_eval.py` | Character-level scoring for the three ONNX detectors; three decision rules in one pass; **writes every predicted span to `<run>.spans.jsonl`** |
| `pii_char_f1_report.py` | Re-scores those span files offline — `--by-label`, `--by-length`, `--rule`; works on a run still in flight |

Generated reports, alongside the runs they derive from:

- `l2_pii_nemotron_20k/runs/category_recall_{lenient,strict}.{txt,json}`
- `l2_pii_nemotron_20k/runs/category_enrichment_detectors.{txt,json}`
- `l2_pii_nemotron/runs/category_enrichment_llm.{txt,json}`
- `l2_pii_nemotron_20k/runs/min_score_curve.{txt,json}` (the `min_score_sweep.jsonl` it derives from is 4.4MB and stays local)
