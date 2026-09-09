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
- Raw rows carry `spans` with `start` / `end` / `label` / `text`. **The builder
  discards the offsets** and keeps only the comma-joined type list per document,
  so only document-level multi-label scoring is possible today. Preserving the
  offsets is the prerequisite for any future span-level F1.
- Documents are short: p50 ≈ 744 chars, p99 ≈ 3,259, max 7,191 (**1,737 tokens**
  measured with the pplx tokenizer). See §7 for why this settles the
  context-length question.

### 2.1 There is no benign arm, and there cannot be one from this dataset

**A scan of 30,000 test rows found ZERO documents with no PII.** Nemotron-PII is
a pure-positive generation dataset — `has_pii = len(spans) > 0` is true for
essentially every row. That is why every corpus here reports `n_benign: 1`
despite the builder being asked for thousands.

Everything downstream follows from this:

- **Precision, FP-rate and F-beta are statistically empty** across every run in
  this series. The negative arm is n=1.
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
```

**Always pass `--verbose`.** A non-verbose log records only failures, which
makes it impossible to distinguish "passed" from "not yet run" — it breaks both
`--resume-from` and all of the re-scoring in §5.

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

## 5. Coverage-aware per-category scoring

`pii_category_recall.py` re-scores existing runs per category. **It needs no new
inference** — every verbose log already records its per-case predictions
(`detected=CREDIT_CARD,DATE_TIME,...`), so gold and predicted labels are both
recoverable offline.

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
| mmBERT32K-PII (safetensors) | 20k | 0.12% (25/20000) | 99.87% | 1.01hr | `ner_mmbert32k-...-194327` |
| mmBERT32K-PII (ONNX/Lemonade) | 20k | 0.24% (49/20000) | 99.76% | 7.19hr | `policy_smoke_20260819-100935` |
| OpenMed/privacy-filter-multilingual | 20k | 0.07% (14/20000) | 99.93% | 2.15hr | `ner_privacy-filter-multilingual_...-194332` |
| OpenMed/privacy-filter-ml-v2 (safetensors) | 20k | 0% (0/20000) | 100% | resumed; partial | `ner_privacy-filter-multilingual-v2_...-224148` |
| OpenMed/privacy-filter-ml-v2 (ONNX/Lemonade) | 20k | 0% (0/20000) | 100% | 8.12hr | `pf_router_full20k_stdout_20260819` |
| GLiNER (nvidia/gliner-PII) | 20k | 0.005% (1/20000) | 99.995% | resumed; partial | `gliner_gliner-PII_...-211156` |
| OpenAI/privacy-filter | 20k | 5.31% (1062/20000) | 94.69% | resumed; partial | `ner_privacy-filter_...-224152` |
| **perplexity-ai/pplx-pii-masking** | 20k | **0.80% (159/20000)** | **99.20%** | **2.55hr** | `pplx_f1f90a53..._20260908-173941` |

Routing / model-eval split, where the router logs report one:

| Model | E2E | Routing | Routed-model eval |
|---|---|---|---|
| LLM Qwen3.5-2B | 2.90hr | 2.47hr | 21min |
| LLM Qwen3.5-0.8B | 2.21hr | 1.86hr | 18min |
| mmBERT32K (ONNX) | 7.19hr | 4.60hr | 2.59hr |
| OpenMed v2 (ONNX) | 8.12hr | 7.05hr | 1.06hr |

All runs are CPU. pplx is fp32 on 16 threads, single clean pass over all
20,001 cases with no resume.

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

### 6.2 What the leak-rate column cannot tell you

Repeating §2.1 because this table is what circulates: **with one benign case,
the precision side is unmeasured.** A model that flagged every document would
show a perfect leak rate here. For the record, on that single benign case
mmBERT, OpenMed-v2 and GLiNER all flagged it (FP=1, over-route 100%), while
OpenMed-v1, OpenAI/privacy-filter and pplx did not. At n=1 that separates
nothing — it is listed only to make the gap concrete.

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
4. **Filtering categories out of the report.** `--min-support` defaults to 0 for
   exactly this reason. An earlier `--doc-level` built the row list from
   categories *some model covered*, which silently dropped BIOMETRIC (1,958
   docs) and EDUCATION (1,416) — the two biggest shared blind spots — from the
   table entirely.
5. **Trusting a mapping because the number looks plausible.** Two mappings were
   wrong on the first pass: `ONLINE_IDENTIFIER` lumped URLs with IPs, MACs and
   usernames (penalizing pplx, which only claims URLs), and
   `EYECOLOR`/`HEIGHT` → `BIOMETRIC` produced a fake "0.2% biometric failure"
   for OpenMed that was purely a bad mapping. Both are now split
   (`WEB_URL` / `NETWORK_ID` / `ACCOUNT_HANDLE`, and `PHYSICAL_ATTRIBUTE`).
   **The mappings in `pii_taxonomy.py` are editorial judgment and should be
   reviewed, not assumed.**
6. **Routing on pplx's sensitivity head.** 9.16% recall vs 99.20%. §4.1.
7. **Grepping `[FAIL][FN]` against a routing log.** Returns zero, looks perfect,
   is wrong. §3.1.
8. **Running without `--verbose`.** Destroys resume and all re-scoring. §4.

## 9. Open work

Roughly in order of value:

1. **Build a benign arm** (§2.1). Without it, half of every confusion matrix in
   this series is decorative, and it changes the meaning of every existing row
   retroactively. Do this before benchmarking more models.
2. **Preserve span offsets in the corpus** — free at build time, and the
   prerequisite for span-level scoring and for disambiguating lenient vs strict
   credit (§5.2) on evidence rather than convention.
3. **Review `pii_taxonomy.py`'s mappings** (§8.5).
4. **Re-run the three embedding classifiers properly.** Their rows were
   inverted (§6.1) and rested on n=20 with zero benign cases. As corrected they
   leak 90-100%, which — if it survives a real run — is a finding worth stating
   deliberately rather than a cell to quietly fix.
5. Re-run OpenMed v2 and the ONNX variants with `--verbose` on the 20k corpus if
   per-category numbers are wanted for the rows currently backed only by routing
   logs.

## 10. Files

| File | Purpose |
|---|---|
| `build_nemotron_corpus.py` | Samples Nemotron-PII into `cases.jsonl` + `stats.json` |
| `pii_ner_eval.py` | Standard HF token-classification models (mmBERT, OpenMed) |
| `pii_gliner_eval.py` | GLiNER zero-shot (labels supplied at inference) |
| `pii_pplx_eval.py` | `perplexity-ai/pplx-pii-masking` (custom arch, dual heads) |
| `pii_routing_eval.py` | Full router replay — regex, LLM-as-router, ONNX classifier policies |
| `pii_taxonomy.py` | Canonical 24-category interlingua + per-model mappings |
| `pii_category_recall.py` | Re-scores existing logs per category; `--strict`, `--doc-level` |

Generated reports, alongside the runs they derive from:

- `l2_pii_nemotron_20k/runs/category_recall_{lenient,strict}.{txt,json}`
- `l2_pii_nemotron_20k/runs/category_enrichment_detectors.{txt,json}`
- `l2_pii_nemotron/runs/category_enrichment_llm.{txt,json}`
