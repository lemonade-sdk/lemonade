# Catching PII before it leaves the machine: detectors, routing, and the ONNX path

A prompt lands on a local gateway on its way to a frontier cloud model:

> "Can you help me word an appeal letter for MRN 4471-2298, DOB 03/14/1979? I was
> denied coverage last month..."

Somewhere in that sentence is a medical record number and a date of birth. If the
gateway doesn't notice, both are now sitting in a cloud provider's request logs.
There's no way to un-send them.

That asymmetry is the whole design problem. **A leak is unrecoverable** — the
data has left the machine and no later action retracts it. **An over-route is
recoverable** — it costs some money and maybe some answer quality, and it can be
fixed on the next request. So the metric that matters is recall on the sensitive
class, not precision, and that's a deliberate choice, not a limitation of how we
measured things. (We'll come back to what that choice costs — precision doesn't
disappear, it just becomes something the corpus we used can't measure. More on
that in a few sections.)

The obvious first move is regex: match SSN patterns, email patterns, credit card
prefixes, and route anything that matches. We ran that baseline on 2,500 sampled
prompts from `nvidia/Nemotron-PII`. It leaks **18.7% (467/2,500)** — nearly one
in five sensitive prompts sails through untouched. That's the bar everything else
in this post has to clear.

This post is about building and evaluating a better gate: an inline PII
classifier wired into [Lemonade's](https://github.com/lemonade-sdk/lemonade) model
router, so a prompt gets scored before it's sent anywhere. We'll cover what's
been published in this space, which open models we benchmarked and how, how we
took two of them from safetensors to ONNX without changing their decisions, what
the router actually measures versus what a detector paper usually reports, and
the two decision rules — `min_score` versus argmax versus a constrained decoder —
that turn out to matter more than which model you pick.

---

## Related work

Three families of PII detector show up repeatedly, and it's worth knowing what
each one is actually good at before comparing numbers.

**Rules and pattern matching.** [Microsoft Presidio](https://github.com/microsoft/presidio)
is the reference implementation here — a combination of regex recognizers and a
spaCy NER pass, open-sourced in 2019 and still the first thing most teams reach
for. It's cheap, deterministic, and auditable: you can read exactly why it fired.
It's also brittle. A 2025 study on financial-document PII
([*Scientific Reports*](https://www.nature.com/articles/s41598-025-04971-9))
found Presidio's fixed recognizers struggled with the format variety in real
financial text, and only recovered acceptable recall (89.4%) by bolting a
custom NER model on top of the rules. Our own regex baseline shows the same
failure mode on conversational text: 18.7% leak rate, because a phone number
written `+1 (415) 555 0132` or a date written `14 March 1979` just doesn't match
a fixed pattern.

**Encoder NER models.** This is the family the bulk of this post is about:
BERT-style token classifiers fine-tuned to tag PII spans, running in
milliseconds on CPU with a fixed label vocabulary. The architecture goes back to
standard sequence-labeling NER, but the interesting recent line of work is
**GLiNER** ([Zaratiana et al., NAACL 2024](https://arxiv.org/abs/2311.08526)),
which treats label names as *input* to the model rather than baked into the
output head — so the same network can be pointed at whatever entity types a
deployment needs. That flexibility spawned a cluster of PII-specific fine-tunes:
`urchade/gliner_multi_pii-v1`, Knowledgator's `gliner-pii-base/large-v1.0`, and
NVIDIA's `gliner-PII`, several of which are now wired into Presidio itself via a
built-in `GLiNERRecognizer`. On the clinical side, tools like
[CliniDeID](https://academic.oup.com/jamiaopen) (JAMIA Open, Jan 2024) took the
domain-specific route, reporting 95.9% macro-recall on names against HIPAA
Safe-Harbor and Expert-Determination standards — a reminder that this family
has been in production, quietly, in healthcare de-identification pipelines for
years before the current wave of general-purpose privacy filters.

**LLM-as-judge / LLM-as-router.** The newest and most expensive family: prompt a
chat model to decide whether a piece of text contains PII, either as a
standalone judge or as the routing decision itself. It's flexible — no fixed
taxonomy, handles context the other two families can't — and, as we found in our
own baselines below, **not monotone in model size**. A [2025 NEJM AI
paper](https://ai.nejm.org/) used Llama-3 70B as a PHI redactor over real
clinical letters and reported 99.24% removal, which is a strong number, but at a
cost and latency that rules it out as an inline gate for every request.

**What's missing from all three.** Every one of the above is published as a
*detector* result — entity-level F1 or recall on the model's own held-out set.
Nobody publishes **leak rate at an actual routing decision**, measured
identically across model families on one shared corpus, which is the number
that matters if you're the person deciding whether to gate traffic with this
thing. That's the gap this post tries to fill, and it's also exactly the
question Perplexity's own recent work is aimed at.

**The most directly comparable prior work** is Perplexity's
[**PII-TRACE**](https://www.perplexity.ai/hub/blog/pii-trace-detecting-personal-data-before-it-leaves-the-device),
published as part of their Hybrid Compute feature for the Mac app. It introduces
a 0.6B model, PII-Tracer, that acts as a local privacy gate deciding whether a
turn of a conversation should stay on-device, get masked, or escalate to the
cloud — the same three-way decision this post's router policies encode. Their
headline problem is different from ours (tracing the *same* identifier
consistently across a long, multi-turn, possibly multilingual conversation,
where a single missed mention re-exposes something already redacted elsewhere),
and their benchmark construction is worth reading on its own: they template
real conversations, insert consistent synthetic identifiers, and recompute
character offsets after paraphrasing so gold spans survive rewriting. Two
results are directly relevant to what follows here. First, they hit a context
wall — recall drops from 0.955 to 0.687 once a conversation crosses 10,000
characters, purely because their 4,096-token window can't see the earlier
mention — and recover most of it (0.830 → 0.965 character recall) with a
50%-overlap sliding window at inference time, no retraining. Our own corpus
never hits that wall (documents top out at 1,737 tokens — see the dataset
section below), so we can't reproduce or refute that finding, but it's the
right thing to check before deploying any of the detectors below on long
conversations. Second, they report PII-Tracer beating OpenAI's privacy filter on
character F1 across five external benchmarks — including, notably,
**Nemotron-PII, the same corpus this post uses**. That's the one number from
their post that overlaps with ours, and it's consistent with what we find below
for the OpenAI/privacy-filter model: it's the weakest detector in every table
here (5.31% leak rate, the highest of any encoder model tested).

Two more recent entries round out the encoder family: the
**[SPY benchmark](https://aclanthology.org/2025.naacl-srw.23.pdf)**
(NAACL-SRW 2025) built a synthetic PII dataset specifically to expose failure
modes existing detectors miss — the same motivation behind our own
character-level scoring below — and **GLiNER2-PII**, a 2025/2026 fine-tune
covering 42 entity types across seven categories, which reports the best recall
among GLiNER-family models on SPY (0.718 average) but a comparatively modest
span-F1 (0.477), a reminder that wider taxonomies and higher recall don't
automatically come free.

---

## The Lemonade router: what "solved" looks like here

[Lemonade](https://github.com/lemonade-sdk/lemonade) is a local LLM server that
runs models across CPU, GPU, and NPU backends and exposes OpenAI-, Ollama-, and
Anthropic-compatible APIs. One of its recipe types, `collection.router`, lets
you describe a routing policy as JSON: a set of candidate models, a default,
and a set of classifiers and rules that decide which candidate handles a given
request. The deployment picture for this post is one instance of that recipe:

```
prompt --> [PII classifier] --> PII found?  --yes--> small local model
                                     |
                                     no
                                     v
                              default model (local or cloud)
```

A classifier in this schema can be a `classifier` (a token-classification ONNX
model scored per-label against a `min_score`/`max_score` band), a
`semantic_similarity` embedding comparison against reference phrases, or an
`llm` acting as a chat-based judge. The rule that consumes it is a `match`
expression — `any`/`all` of leaf conditions like `keywords_any`, `regex`,
`min_chars`, or a classifier band test — evaluated first-match-wins, with a
configurable fail-open or fail-closed behavior on classifier errors.

Here's a trimmed version of the actual policy backing the pplx-based results in
this post (the full file, with all 36 label clauses, is committed at
`test/conformance/routing/1/l2_pii_onnx_pplx_masking/policy.json`):

```jsonc
{
  "version": "1",
  "model_name": "user.PII-ONNX-PplxMasking-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",
    "classifiers": [
      {
        "id": "pii-onnx-pplx",
        "type": "classifier",
        "model": "user.pplx-pii-masking-onnx",
        "labels": ["O", "B-private_person", "I-private_person", /* ...33 more BIOES labels */ "S-other_pii"],
        "on_error": "match_false"
      }
    ],
    "rules": [
      {
        "id": "pii-detected",
        "match": {
          "any": [
            { "classifier": "pii-onnx-pplx", "label": "B-private_person", "min_score": 0.5 },
            { "classifier": "pii-onnx-pplx", "label": "I-private_email",  "min_score": 0.5 },
            { "classifier": "pii-onnx-pplx", "label": "S-secret",         "min_score": 0.5 }
            /* ...33 more identical clauses, one per BIOES label */
          ]
        },
        "route_to": "Qwen3.5-0.8B-GGUF"
      }
    ]
  }
}
```

Two definitions we'll use for the rest of the post:

- **Leak rate** = sensitive prompts routed to the cloud model / all sensitive
  prompts. The number that matters most.
- **Over-route** = benign prompts routed local instead of to the (presumably
  better) cloud default. Costs quality and money. **We don't have this number
  yet** — the dataset we used has no benign arm, which we'll explain in a
  moment and won't gloss over later.

---

## Candidate models

Here's what's publicly available, and the axis that actually separates them.

| Model | Params | Label space | Context | Architecture |
|---|---|---|---|---|
| mmBERT32K-PII | ~300M | 35 BIOES labels (Presidio-style) | 32k tokens | encoder, `ModernBertForTokenClassification` |
| OpenMed privacy-filter (v1) | — | coarse categories | — | `openai_privacy_filter` |
| OpenMed privacy-filter-multilingual (v2) | — | coarse categories, expanded | 128k tokens | `openai_privacy_filter`, MoE |
| perplexity `pplx-pii-masking` | — | 9 categories, 37 BIOES labels + dual sensitivity head | 4k tokens | custom `PiiMaskingModel`, span head + sensitivity head, constrained Viterbi decoder |
| GLiNER (`nvidia/gliner-PII`) | — | zero-shot, label names supplied at inference | — | span extractor |
| OpenAI/privacy-filter | — | 8 coarse categories | — | encoder, BIOES |

The axis worth paying attention to isn't parameter count or raw accuracy — it's
**label-space coverage**. `pplx-pii-masking` expresses 9 categories total and
can't represent 11 of the 24 canonical categories our corpus exercises, the
widest coverage gap of anything we tested — and yet it leaks only 0.80% of
documents. Coverage and document-level leak rate are close to independent here:
a narrow taxonomy costs you *category attribution*, not necessarily *leak rate*,
because most documents that need flagging carry multiple PII types and only one
needs to fire. Keep that in mind for the results below — it's the reason the
document-level leak table looks so flat across very differently-shaped models,
and it's why we eventually turn to character-level scoring to actually
discriminate between them.

One more honest caveat: there's no language field in this corpus, and it's
English-only. mmBERT and OpenMed-multilingual are both multilingual-capable
models, but **nothing in this post measures a multilingual advantage** — we
simply didn't test one.

---

## The dataset

All numbers in this post come from `nvidia/Nemotron-PII` (test split), sampled
into two corpora that are **not comparable to each other**:

- `l2_pii_nemotron` — 2,500 cases, used for the LLM-as-router baselines.
- `l2_pii_nemotron_20k` — 20,000 cases, used for the encoder-detector runs.

Wherever a number from one corpus sits next to a number from the other in this
post, treat them as two independent samples, not a controlled comparison.

The corpus carries 55 distinct gold labels, roughly 35 direct identifiers (SSNs,
emails, phone numbers, account numbers) plus about 12 quasi-identifiers and
sensitive attributes — gender, race, sexuality, religion, political affiliation,
education, employment, age, language, blood type, occupation, biometric
descriptors. **Most of the detector models below have no output class at all
for that second group**, which matters a lot for the per-category results
further down.

Documents are short: p50 ~744 characters, p99 ~3,259, max 7,191 characters
(1,737 tokens). Zero documents truncate for any model in this comparison, so the
32k/128k/4k context-window spread between mmBERT, OpenMed, and pplx could not
have been what separated their results.

**Here's the concession we want to make plainly, up front, rather than bury in
a limitations section at the end:** a scan of all 30,000 test rows across both
corpora found **zero** documents with no PII at all. Nemotron-PII is a
pure-positive generation dataset. The consequences are blunt:

- Document-level precision, false-positive rate, and F-beta are **statistically
  empty** for every model in this post — there's no denominator.
- A model that flagged every single document, with zero discrimination, would
  score identically to a genuinely precise one on every document-level metric
  reported here.
- Only recall / leak rate carries real information at the document level.

We'll partially repair this with character-level precision later (an
over-tagging model does get penalized there, because it starts marking
characters inside documents that *are* positive but where it's wrong about
*which* characters). But that's bounded by how PII-dense the corpus is, not a
true measure of false-positive behavior on benign traffic — and building an
actual benign arm remains the single biggest gap in what's measured here.

---

## Baselines: regex, embeddings, and the LLM-as-router size paradox

All on the 2,500-case corpus:

| Baseline | Leak rate | Recall |
|---|---|---|
| Regex | 18.7% (467/2,500) | 81.30% |
| embeddinggemma-300m (semantic similarity) | 100% (20/20)\* | 0.00%\* |
| Qwen3-Embedding-0.6B | 90% (18/20)\* | 10.00%\* |
| Qwen3-Embedding-4B | 90% (18/20)\* | 10.00%\* |
| LLM Qwen3.5-9B | 6.16% (154/2,500) | 93.84% |
| LLM Qwen3.5-2B | 3.28% (82/2,500) | 96.72% |
| LLM Qwen3.5-0.8B | 2.84% (71/2,500) | 97.16% |

\* These three rows are n=20, zero benign cases, and should be read as a
directional pilot rather than a settled result — worth re-running at scale
before leaning on them.

Two things stand out.

**Semantic similarity is the wrong tool for this job**, at least as measured
here — the embedding classifiers were the worst-performing configurations in
the whole comparison, at any scale we tried them at.

**The size paradox is the more interesting finding.** The 9B model leaks *more
than twice* what the 0.8B model leaks. Looking at which categories each model's
misses concentrate in explains why:

| Category | support | Qwen3.5-9B | Qwen3.5-2B | Qwen3.5-0.8B |
|---|---:|---:|---:|---:|
| WEB_URL | 893 | **1.92x** | 1.81x | **1.97x** |
| ORG_COMPANY | 672 | **1.65x** | 1.22x | 1.68x |
| ADDRESS_LOCATION | 991 | **1.52x** | 1.29x | 0.82x |
| NETWORK_ID | 310 | 0.47x | 0.98x | **2.04x** |
| CREDENTIAL_SECRET | 469 | 0.21x | 0.59x | 1.05x |
| DATE_OF_BIRTH | 312 | **0.00x** | **0.00x** | 0.11x |
| PERSON_NAME | 1,213 | 0.11x | 0.33x | 0.35x |

(Enrichment = `P(missed | doc has category) / overall miss rate`; above 1.0
means that category is over-represented among the documents a model let
through. Because only 8 of 2,500 documents contain a single canonical
category, a missed document is usually missed for around seven categories at
once, so per-category attribution here is about which categories co-occur with
a miss, not a clean per-entity recall number.)

The 9B's misses concentrate in **judgment** categories — URLs, company names,
addresses, demographic attributes. It reasons its way *out* of flagging them.
Here's the actual logged rationale on a document labeled
`company_name, education_level, occupation, sexuality, url`:

> "The request contains no personal information"

The 0.8B's misses concentrate instead in **technical identifiers** — it simply
doesn't recognize a network ID or credential string as sensitive at all.
Judgment errors and capability errors want opposite fixes: tighter policy and
prompt constraints for the 9B, a strictly bigger model (or a dedicated encoder,
see below) for the 0.8B. Neither problem goes away by scaling in the direction
that seems intuitive.

---

## Encoder detectors: the leak-rate table, and the turn to character F1

The main results, all on the full 20,000-case corpus, CPU only:

| Model | Leak rate | Recall | E2E runtime |
|---|---|---|---|
| GLiNER (`nvidia/gliner-PII`)\* | 0.005% (1/20,000) | 99.995% | partial run |
| OpenMed privacy-filter-ml-v2 | 0% (0/20,000) | 100% | 8.12hr (router) |
| OpenMed privacy-filter-multilingual | 0.07% (14/20,000) | 99.93% | 2.15hr |
| mmBERT32K-PII (safetensors) | 0.12% (25/20,000) | 99.87% | 1.01hr |
| mmBERT32K-PII (ONNX via router) | 0.24% (49/20,000) | 99.76% | 7.19hr |
| pplx-pii-masking (safetensors) | 0.80% (159/20,000) | 99.20% | 2.55hr |
| pplx-pii-masking (ONNX) | 0.80% (159/20,000) | 99.20% | 2.17hr (not comparable) |
| OpenAI/privacy-filter | 5.31% (1,062/20,000) | 94.69% | partial run |

\* GLiNER needs an asterisk everywhere it appears: our eval script extracts the
55 gold label names straight out of the corpus and hands them to the model at
inference time. It's told exactly what to look for, in the dataset's own words.
It's a calibration reference here, not a fair competitor.

Also worth flagging: the 7.19hr / 8.12hr figures are **full router round-trips**
(HTTP overhead plus the routed model actually answering), not raw
classification. Direct classification over the same 20,001 cases is roughly 28
minutes. The gate itself is not what costs hours here.

**Now for the turn.** Every model in that table is "essentially perfect," the
top six sit inside one percentage point of each other, and document-level leak
rate has run out of ability to rank them. Scoring the same runs at the
character level (treating every PII span as a set of character indices,
independent of any taxonomy) changes the picture completely:

| Model | char P | char R | **char F1** | doc leak |
|---|---:|---:|---:|---:|
| OpenMed privacy-filter-ml-v2 | 0.9769 | 0.9365 | **0.9563** | 0.00% |
| pplx-pii-masking | 0.9725 | 0.7330 | **0.8360** | 0.795% |
| mmBERT32K-PII | 0.9202 | 0.6842 | **0.7849** | 0.125% |
| *flag everything (strawman)* | *0.1458* | *1.0000* | *0.2544* | *0.00%* |

**The ordering inverts.** mmBERT leaks six times fewer *documents* than pplx,
and is the *worse* detector by characters — it tends to fire somewhere on
nearly every document while covering a much smaller fraction of what's actually
in it. That single table is the whole argument for why document-level scoring
alone isn't enough: three models a leak-rate column says are all "basically
done" turn out to be 0.16 of character F1 apart. It's also a real, measured
version of the "flag everything" strawman — 0.2544 char F1 is what you get for
zero discrimination, and every real model clears it comfortably.

Character scoring also gives a genuine precision number for the first time in
this post: **0.92–0.98 across the three detectors**, against that 0.1458 floor.
That doesn't fix the missing benign arm (it's still bounded by how PII-dense
the positive documents are, not a true false-positive rate), but it's a partial
answer where before there was none.

**Per-label character recall needs no taxonomy mapping at all**, so it retires
the lenient-vs-strict ambiguity below for these three models specifically. The
most important correction it produces: pplx has no dedicated class for gender,
sexuality, or religion — and yet covers **73.7%, 69.0%, and 65.8%** of those
categories' characters respectively, entirely through its `other_pii`
catch-all label. "The model has no class for this" and "the model doesn't find
this" are genuinely different claims, and only character-level scoring can tell
them apart — which, for a routing decision where the label name is irrelevant
and only coverage matters, is exactly the question that matters. pplx's *real*
holes read near zero instead: `company_name` at 1.2%, `occupation` at 1.4%.
Shared blind spots across all three models: `time` (60.6% / 46.7% / 74.5%) and,
for the two smaller models, `country` (23.3% / 28.4%).

Two more things worth carrying from the per-category (not per-character) view:

- **mmBERT's demographic gap is a genuine model failure, not a taxonomy gap.**
  It *has* Presidio's `NRP` class and still only fires it on 5.0%
  (race/ethnicity/language) and 5.2% (belief/political) of documents that need
  it. pplx and OpenMed score zero on the same categories for the opposite
  reason — they have no such class at all. Those are two completely different
  situations that a bare "missed category" column would conflate.
- **BIOMETRIC (1,958 docs) and EDUCATION (1,416 docs) are total blind spots**
  across all four production detectors — only GLiNER covers them, and only
  because it was told to look for them by name.
- **OpenMed v1 → v2 was a real jump**: ORG_COMPANY 13.4% → 93.1%, GENDER
  53.1% → 95.7%, CREDENTIAL_SECRET 79% → 98%, DATE_TIME 80.4% → 97.4%. The
  remaining weak spot in v2 is OCCUPATION_EMPLOYMENT at 52.5%.

Finally, lenient versus strict credit matters for anything that isn't a
1-to-1 category mapping. pplx's single `account_number` label, for example,
expands under our taxonomy to four canonical categories at once:

| Category | lenient | strict | Δ |
|---|---:|---:|---:|
| DATE_OF_BIRTH | 99.8% | 63.9% | −35.9 |
| GOV_ID | 98.0% | 71.1% | −26.9 |
| MEDICAL | 93.0% | 60.1% | −32.9 |
| INTERNAL_ID | 96.5% | 55.1% | −41.4 |

OpenMed v2 is essentially unchanged under strict scoring; pplx and the OpenAI
privacy filter collapse. This is a **granularity** result, not an accuracy one
— pplx genuinely found something at those character positions roughly 98% of
the time, it just can't tell you which of four related categories it was. For
a routing decision, where the only question is "is there PII here at all,"
lenient is the right lens. For a masking or redaction policy that has to treat
an SSN differently from a customer account ID, strict is. That distinction
connects straight back to the routing decision at the top of this post.

---

## From safetensors to ONNX

The router's classifier path runs on `onnxruntime` — no torch dependency in the
serving process, CPU-friendly, single artifact. Getting there for two of the
models above surfaced two traps worth publishing so another team doesn't lose a
day to them.

**Trap 1: the export can die on Windows *after* it already succeeded.**
`torch.onnx`'s progress printer emits a Unicode checkmark (U+2705); a cp1252
console can't encode it, so the export raises `UnicodeEncodeError` with the
graph already fully built and correct. It reads like an export bug. It's a
console-encoding bug.

**Trap 2: don't export pplx from a repackaged checkpoint that quietly changes
its causality.** One community repack of the pplx checkpoint carries the same
weights repacked as a stock `Qwen3ForTokenClassification`, with a top-level
`"is_causal": false` field — a key stock `transformers` doesn't actually read
([huggingface/transformers#39554](https://github.com/huggingface/transformers/issues/39554)).
It loads with correct tensor shapes, produces plausible-looking output, passes
a self-consistency check, and is **silently causal instead of bidirectional**.
The original repository's vendored `modeling_pplx_qwen3.py` explicitly flips
`is_causal` per layer and rebuilds the attention mask bidirectionally — logic
the repack drops entirely. Full detail on the analogous failure for
OpenMed's model (a similar "wrong architecture class silently loads" trap) is
in `privacy_filter_ml_v2_onnx_repro.md`.

The generalizable lesson from trap 2: **a conversion can be self-consistent and
still wrong.** Equivalence has to be measured, not assumed from a clean load
and sane-looking numbers.

**A third point, arguably the most useful one.** The pplx ONNX graph
deliberately stops at raw per-token logits — the model's own constrained BIOES
Viterbi decoder is not baked into the graph. Scoring the ONNX run by plain
argmax while the safetensors run was scored by Viterbi would silently confound
a *backend* difference with a *decoder* difference, and make any gap between
the two runs uninterpretable. So the ONNX eval script imports the checkpoint's
own `ViterbiDecoder` and feeds it the ONNX logits directly. That leaves exactly
one variable different between the two runs — the backend — and none of the
comparison below is contaminated by a decoding-rule change.

The parity result, over the full 20,001-case corpus:

| Check | Result |
|---|---|
| Confusion matrix | 19,841 / 159 / 1 / 0 — identical, both runs |
| `has_pii` decision agreement | **20,001 / 20,001 (100.0000%)** |
| Routing decision flips | **0** |
| Exact label-set agreement | 20,001 / 20,001 (100.0000%) |
| The 159 missed documents | the same 159 case names, empty set difference both ways |
| Per-category recall, lenient + strict | identical in every cell |
| Logit max-abs-diff (a real 806-char document) | 1.0e-5 |

The row that actually proves this rather than just suggesting it is the
same-case-names check. Identical *counts* alone wouldn't rule out compensating
errors — a model could miss a different 159 documents in each run and still
report the same aggregate numbers. Matching the exact set of missed case names
closes that gap. **We verified this decision-identical for pplx specifically —
this has not been repeated for mmBERT-ONNX or OpenMed-v2-ONNX**, which still
rest on router logs rather than a direct parity diff. We're not claiming a
blanket "conversion never loses anything" here; we're claiming one specific,
measured case, and flagging the other two as open work.

---

## Router B: how the routing rule actually decides, versus argmax

Here's a discrepancy that started this whole investigation: mmBERT reads
**0.12%** leak rate in one table and **0.24%** in another, on the exact same
weights. Same model, two different numbers.

The explanation is two different decision rules, not two different backends:

| Path | Rule |
|---|---|
| offline eval script | argmax per token; document flagged if any non-special token's argmax label isn't `O` |
| the router | softmax per token → take the max score over all tokens, per label → fire if any non-`O` label clears `min_score` |

Here's the mathematical fact that makes this a proof rather than a hunch: a
per-token softmax over 35 labels sums to exactly 1. So a label scoring above
0.5 at a given token *is, by definition*, that token's argmax — there's no room
for two labels to both clear 0.5. That means **`min_score >= 0.5` is strictly
stricter than argmax; it's a containment, not an independently tunable rule.**

We measured that containment rather than just arguing it: the router's 49
leaks are a strict superset of the 25 argmax leaks (25/25 contained, 0
violations), the extra 24 are threshold-only, and re-running both rules
against a shared argmax threshold across all 20,001 cases found 0 entity-set
mismatches and 0 `has_pii` flips. The shipped `min_score` of 0.5 brackets to
`(0.4963, 0.5043]` from the run itself. One more detail worth a sentence:
special tokens (like `<bos>`) are excluded from the router's max-over-tokens —
this model's `<bos>` token always fires a label on its own, and including it
would push all 49 known leaks above 0.5 for the wrong reason.

**What that rule actually costs, measured in characters, reverses the
takeaway.** At the document level the two rules differ by 0.12 percentage
points, and the honest one-line summary would be "the threshold barely
matters." Scoring the same two rules on the same graph, in the same process,
over characters instead:

| mmBERT rule | char P | char R | char F1 | doc leak |
|---|---:|---:|---:|---:|
| argmax | 0.9202 | 0.6842 | **0.7849** | 0.125% |
| `min_score` 0.5 | 0.9494 | 0.4849 | **0.6419** | 0.245% |

**The shipped threshold throws away 29% of the PII characters mmBERT can
find.** A router only needs to notice *one* entity anywhere in a document to
make the right routing call, so it never pays that bill — which is exactly why
the document-level number makes the threshold look nearly free. A masking or
redaction pipeline pays all of it, on every document.

The same shape shows up on pplx from the opposite direction: its own
constrained BIOES Viterbi decoder nearly *doubles* its document leak rate
against plain argmax (159 vs. 84 leaks) while buying back precision
(0.9627 → 0.9725). A constrained decoder is a trade, not a free upgrade.

And the size of that bill is wildly model-dependent — a 30x spread: char recall
costs privacy-filter (OpenMed v2) about −0.7 percentage points, pplx about
−1.4pp, and mmBERT a full **−19.9pp**. The tuning curve two paragraphs down was
measured on mmBERT — the model where the threshold matters the most. **A
threshold doesn't port between checkpoints.**

Put together, the closing thesis for this section is: **route with the loose
rule, mask with the strict one.** Same weights, same graph, two different jobs,
two different thresholds — and it's a cleaner proof than the containment result
above, because here both rules ran over one graph in one process, with nothing
held constant "by argument."

**The schema, briefly** (full reference at `docs/dev/router-policy.md`): policy
is data, evaluated first-match-wins; the default candidate is the fail-open
path; classifier errors default to `match_false` unless configured otherwise;
band tests (`min_score`/`max_score`) gate on a classifier's per-label score; and
cheap conditions (keyword matches, character-length bounds, metadata) are meant
to be checked before anything model-backed, for latency reasons.

**The `min_score` curve** (mmBERT ONNX):

| `min_score` | leaks | recall | vs. argmax |
|---|---|---|---|
| 0.10 | 9 | 99.955% | −16 |
| 0.30 | 25 | 99.875% | +0 |
| 0.50 (shipped) | 49 | 99.755% | +24 |
| 0.70 | 119 | 99.405% | +94 |

**This curve only means something with its caveat attached, in the same
breath.** With exactly one benign case in the whole corpus, lowering
`min_score` shows no measurable cost *here* — the curve is one-sided and is
**not tuning advice** on its own. It's also not as much free headroom as it
looks: of the 24 threshold-only leaks recovered by lowering the bar, the
sub-threshold signal sits on a category the document doesn't actually contain
in 13 of the 24 cases. Those are accidental catches. For a binary route, an
accidental catch still routes correctly — but that same kind of spurious firing
is precisely what would cost precision on benign traffic, which is the one
thing this corpus can't measure at all.

### The same gate helps pplx — in the opposite direction from mmBERT

Reading the mmBERT result alone invites the wrong conclusion — "the router
costs recall." At the same `min_score: 0.5`, pplx shows the opposite, by more
than mmBERT lost:

| Path | Decision rule | Leaks | Recall |
|---|---|---|---|
| offline | ONNX logits → the checkpoint's own BIOES **Viterbi** decoder | 159 / 20,000 | 99.205% |
| the router | ONNX logits → softmax → max over tokens → any non-`O` ≥ 0.5 | **89 / 20,000** | **99.555%** |

The nesting is strict and one-directional here: 89 documents flagged by both,
0 flagged only by the router, **70 flagged only by Viterbi's stricter offline
decoding**. The router recovered 70 documents the model's own decoder missed,
and regressed none, across 20,001 cases with 0 HTTP/parse errors.

The mechanism is the whole point. mmBERT's offline baseline is argmax, which
fires on any single winning token regardless of how weak that win is — so a 0.5
gate can only ever subtract from it. pplx's offline baseline is Viterbi, which
demands a *coherent* BIOES span (a proper `B → I → E` sequence) and therefore
suppresses a confident but isolated token by design. Viterbi is the right rule
for masking, where you need well-formed spans to redact cleanly — and the
wrong rule for a binary gate, where the only question is whether evidence
exists anywhere in the document. The router's max-over-tokens keeps exactly
what Viterbi is designed to throw away.

So the defensible claim isn't "the router is stricter" or "the router is
better" — it's model-dependent, and the honest version is:

> The router's rule is stricter than argmax and looser than a constrained
> Viterbi decoder. Which direction that moves recall depends entirely on what
> the model's own native decoder does — for a routing gate, the model's own
> decoder is not automatically the right baseline to compare against.

Same caveat as everywhere else in this post applies here too: the 70 recovered
documents are free only because this corpus can't charge for false positives —
the router's extra sensitivity also flagged the single benign case in the
whole run that the Viterbi-decoded offline path had left alone. One case
doesn't decide anything on its own, but it points the right way, and we're not
publishing "70 leaks recovered" without that caveat sitting right next to it.

(Provenance, for reproducibility: serving pplx through the router required
adding `"pii_masking"` to the backend's `supported_model_types()` allowlist —
a claim purely about input convention, since pplx's graph only declares
`input_ids`/`attention_mask` and its tokenizer adds no BOS/EOS. This run used
`policy_local_default.json`, identical to the committed `policy.json` except
the non-PII path routes to a local `Qwen3.5-9B-GGUF` instead of a cloud model —
matching how the mmBERT and OpenMed router rows were produced, and meaning no
document in this run was ever sent to a cloud provider.)

---

## Example policies for real deployments

Putting the models above into policies you could actually run. All three are
committed under `test/conformance/routing/1/`.

**1. SaaS deployment with a cloud tier (`l2_pii_onnx_pplx_masking/policy.json`).**
Most requests should get the better cloud model; anything carrying PII should
stay on a local model instead.

```jsonc
{
  "model_name": "user.PII-ONNX-PplxMasking-Router",
  "recipe": "collection.router",
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",           // no PII -> cloud
    "classifiers": [{ "id": "pii-onnx-pplx", "type": "classifier",
                       "model": "user.pplx-pii-masking-onnx", "on_error": "match_false" }],
    "rules": [{ "id": "pii-detected",
                "match": { "any": [ /* one min_score:0.5 clause per BIOES label */ ] },
                "route_to": "Qwen3.5-0.8B-GGUF" }]     // PII found -> local
  }
}
```

**2. Fully local / air-gapped deployment
(`l2_pii_onnx_pplx_masking/policy_local_default.json`).** Identical schema, but
`default_model` is a local `Qwen3.5-9B-GGUF` instead of a cloud model — no
network dependency at all, no API key, nothing ever leaves the machine
regardless of the PII decision. This is the policy behind the 89-leak /
99.555%-recall result above, and it's the one to reach for if "no cloud calls,
period" is itself the requirement rather than PII specifically.

**3. The naive baseline, for comparison (`l2_pii_regex/policy.json`).** No
model at all — 11 regex leaves (SSN, generic 9-digit, email, phone, four
credit-card-brand prefixes, IPv4, two date formats) `any`-matched against a
cloud/local split identical in shape to policy 1:

```jsonc
{
  "model_name": "user.PII-Regex-Router",
  "routing": {
    "candidates": ["Qwen3.5-0.8B-GGUF", "fireworks.kimi-k2p6"],
    "default_model": "fireworks.kimi-k2p6",
    "rules": [{ "id": "pii-detected",
                "match": { "any": [
                  { "regex": "\\b\\d{3}-\\d{2}-\\d{4}\\b" },
                  { "regex": "[a-zA-Z0-9._%+\\-]+@[a-zA-Z0-9.\\-]+\\.[a-zA-Z]{2,}" }
                  /* ...9 more regex leaves */
                ] } },
                "route_to": "Qwen3.5-0.8B-GGUF" }]
  }
}
```

Zero model dependency, sub-millisecond, and the 18.7% leak rate that opened
this post. It's the right choice only when a model-backed classifier genuinely
isn't available — everything above it in this post exists because that leak
rate is too high for anything that has to hold up under real traffic.

A policy this shape generalizes past "local vs. cloud LLM," too — the same
classifier could gate access to a `responses`/tool-use path, an image
generation request, or a document upload, anywhere Lemonade's router sits in
front of more than one destination and one of them shouldn't see sensitive
input.

---

## What the gate costs

This is the one section we can't fill in from any run we have. The runtime
numbers earlier in this post mix classification time with the routed model
actually generating an answer (7.19hr end-to-end vs. roughly 28 minutes for
classification alone, over the same 20,001 cases) — that gap tells you the
classifier itself is cheap relative to inference, but it doesn't answer the
question a systems audience actually asks: what does this add to p50/p95
latency on a single prompt? We don't have a clean per-prompt latency
measurement yet, broken out by model and by backend (CPU/GPU/NPU). Rather than
estimate it, we're flagging it as open work.

---

## Limitations

Most of what belongs here has already been conceded in the body of the post
rather than saved for the end, but to have it all in one place:

- **The missing benign arm is still the load-bearing gap.** Character
  precision narrows it a little — an over-tagging model is now penalized on
  positive documents — but it's bounded by PII density in the corpus, not a
  true false-positive rate, and it says nothing about how any of these models
  behave on genuinely benign traffic. Building a real negative arm (Nemotron
  documents with every PII span swapped for generic non-identifying text, so
  ground truth is PII-free by construction) is the highest-value thing left to
  do here.
- **Character F1 covers three models, not eight.** The LLM routers and
  embedding classifiers emit a decision, never a span — they're
  document-level *by construction*, not by an oversight. GLiNER, OpenMed v1,
  OpenAI/privacy-filter, and mmBERT-safetensors simply haven't had this run
  yet.
- **The category-mapping taxonomy behind the lenient/strict tables is
  editorial judgment, and it should be reviewed rather than assumed.** Two
  mappings were wrong on a first pass during this work, and one produced a
  fabricated "0.2% biometric failure" for OpenMed that turned out to be a bad
  mapping, not a model failure. Character-level per-label recall needs none of
  this mapping, which is part of why it's the more trustworthy diagnostic.
- **The GLiNER numbers throughout this post are a calibration reference, not a
  competing result** — it was handed the gold label vocabulary at inference
  time.
- **No language field exists in this corpus.** Every claim in this post is
  English-only; nothing here measures a multilingual advantage for the
  multilingual-capable models.

---

## What we'd ship

**OpenMed privacy-filter-ml-v2 is the recommendation**, and the reason goes
past its 0% document leak rate, which three other models effectively tie —
it wins character F1 by 0.12 over the next best (0.9563 vs. 0.8360), never
drops below roughly 92% recall in any span-length bucket, and has no
catastrophic per-label hole the way the other two do — its weakest category is
`time` at 74.5%, where the other two sit at 60.6% and 46.7%. The cost is size:
a 5.6 GB fp32 graph, 3.29 hours of runtime against mmBERT's 0.40 hours over the
same 20,001 documents — roughly **8x slower per document**. Whether that
trade is worth it depends on your latency budget.

**Match the decision rule to the job, not just the model to the job.** Both
decoder findings above point the same direction: `min_score` 0.5 costs mmBERT
0.68 → 0.48 character recall while barely moving its document leak rate, and
pplx's own Viterbi decoder nearly doubles its leak rate against plain argmax
while buying back precision. **Route with the loose rule, mask with the strict
one** — most write-ups about this kind of gate never separate those two jobs,
and the gap between them is where most of the surprising results in this post
came from.

And say plainly what's still missing rather than imply it's solved: an
over-routing number, and a per-prompt latency figure for the gate itself. Both
are next.
