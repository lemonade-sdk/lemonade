# PII detection and routing: blog plan

The writing plan for a public post on PII detection/identification and routing
with the Lemonade router. Section flow, what goes in each section, which numbers
are quotable today, and the experiments still outstanding.

**All numbers here are copied from `pii_benchmarking.md` and carry its caveats.**
Nothing in this plan re-derives a result; if a number here disagrees with
`pii_benchmarking.md`, that file wins.

**Numbering.** Three schemes are in play, so they are kept visually distinct:

| Form | Means |
|---|---|
| `§6.4`, `§5.1` — two-level | a section of `pii_benchmarking.md`, the source of record |
| `§7`, `§11` — one-level, unqualified | a section of the blog post being planned (the table in part 2) |
| `§7 of the benchmarking doc` | a one-level ref into `pii_benchmarking.md`, always spelled out |
| `E1` … `E11` | an experiment in the plan (part 6) |
| `## 1.`, `## 6.` | a part of *this* file |

Scope decision already made: **no benign/general-prompt arm sourced from outside
Nemotron.** The corpus is `nvidia/Nemotron-PII` and any negative arm is
constructed from it (E1 below), not sampled from dolly/ultrachat.

---

## 1. The thesis, and the one structural decision

The post is about **routing**, not about a model bake-off. The bake-off is
evidence for a routing claim. That ordering has to be visible in the structure or
the reader spends two thirds of the post looking at detector tables without
knowing what decision consumes them.

### 1.1 Split the router into two sections

Do not put "the router" in one block at the end. It has two different jobs and
they belong in different places:

| | Where | Length | Job |
|---|---|---|---|
| **Router A — the decision** | §2, immediately after the problem | ~400 words | Establish *the metric*. Prompt → classifier → local vs cloud. One trimmed policy JSON. |
| **Router B — the mechanics** | §9, after the detector results | full section | `min_score` vs argmax, the rule engine, routing results, the tuning curve. |

**Why A must come first.** `pii_benchmarking.md` §1 says the headline benchmark
measures a binary — "did the model emit any entity anywhere in this document."
Read cold, that is an *apology* for a crude metric. Read after the routing
decision has been introduced, the identical sentence is a *justification*: the
consumer of this classifier is a binary route, so a binary is the correct thing
to measure, and character-level scoring becomes the bonus diagnostic rather than
the missing headline. Same fact, opposite valence, purely from ordering. This is
the single highest-leverage structural choice in the post.

**Why B must come late.** The router-vs-argmax result (§6.4) is only interesting
once the reader has seen the mmBERT rows differ by 2x (0.12% vs 0.24%) and wants
to know why. Explaining the rule engine in the abstract, before there is a
discrepancy to explain, is documentation rather than a finding.

### 1.2 Router B is the strongest router material available

Lead it with §6.4, not with a feature tour of the policy schema:

- The two mmBERT rows differ by 2x and **the backend contributes exactly zero**.
- A per-token softmax over 35 labels sums to 1, so a label above 0.5 at a token
  *is* that token's argmax. `min_score >= 0.5` is therefore **strictly stricter
  than argmax — a containment, not a tunable equivalent.**
- It was measured, not argued: router leaks are a superset of safetensors leaks
  (25/25 contained, 0 violations), the extra 24 are threshold-only, and the
  backend-vs-backend check under a shared argmax rule found 0 entity-set
  mismatches and 0 has_pii flips across 13,708 shared cases.
- There is **no threshold at which the two rules agree**. 0.30 matching argmax's
  *count* (25) is a coincidence — the sets overlap only 21/25.
- **And the containment has a price the document-level rows hide.** The same two
  rules scored over characters read 0.7849 vs 0.6419 char F1 — the shipped
  threshold discards 29% of the PII characters mmBERT can find. That is the
  payoff of putting Router B after the detector results *and* after §7's
  character turn: the reader arrives already holding the metric that makes the
  point land.

That is a concrete, falsifiable answer to "how is the router different from a
base argmax," and it is far better than a schema walkthrough. The schema
(first-match-wins, fail-open default, `on_error: "match_false"`, band tests,
cheap conditions before expensive ones, policy-as-data) goes in as supporting
detail *after* that, at about a third of the length.

---

## 2. Section flow

| # | Section | Core content | Status |
|---|---|---|---|
| 1 | The problem | A prompt containing an MRN. Asymmetric cost. Why not "just regex it" | write |
| 2 | **Router A — what "solved" looks like** | The routing decision; the metric follows from it | write |
| 3 | Related work | Three families + the gap | write |
| 4 | Candidate models | What exists open source; the axis that matters | have |
| 5 | The dataset | Nemotron-PII; **state the zero-benign fact here** | have |
| 6 | Baselines | Regex → embeddings → LLM-as-router; the size paradox | have |
| 7 | Encoder detectors | Leak table, then **the turn to character F1** | have |
| 8 | ONNX | Two traps + the parity proof | have (pplx only) |
| 9 | **Router B — how it decides** | `min_score` vs argmax **in characters**; routing results; the curve | have |
| 10 | Cost of the gate | ms/prompt for the classifier | **missing — E3** |
| 11 | Limitations | Benign arm, char-F1 coverage, taxonomy, GLiNER asterisk | have |
| 12 | What we'd ship | A recommendation with numbers | write |

### §1 The problem

Concrete opening: a user prompt carrying a medical record number, headed for a
frontier cloud model. The framing that sets up every metric in the post:

**The costs are asymmetric.** A leak is unrecoverable — the data has left the
machine and no later mitigation retracts it. An over-route is recoverable — it
costs money and some answer quality. So the design target is recall on the
sensitive class, and that is a deliberate choice rather than a limitation of the
measurement. (This sentence also pre-empts the "your precision is unmeasured"
objection that §5 then concedes in full.)

Kill the obvious alternative early: **regex leaks 18.7%** (467/2500) on this
corpus. That is one in five sensitive prompts, and it is the approach most teams
reach for first.

### §2 Router A — what "solved" looks like

Short. The deployment picture, one figure, one trimmed policy JSON, and the
metric definition.

**Trim the policy for print.** The real
`l2_pii_onnx_pplx_masking/policy.json` carries 36 near-identical `min_score`
clauses (4 BIOES tags x 9 categories). Show three and elide the rest with a
comment; the full file goes in the repo link. An unelided 36-clause `any` block
reads as an argument against the schema.

Define here and use consistently for the rest of the post:

- **Leak rate** = sensitive prompt routed to cloud / all sensitive prompts. The
  number that matters.
- **Over-route** = benign prompt routed local. Costs quality and money.
  **Currently unmeasured — say so here, not only in §11.**

### §3 Related work

Three families, so the reader has a map:

1. **Rules / pattern matching** — Presidio and friends. Cheap, deterministic,
   auditable; 18.7% leak here.
2. **Encoder NER models** — mmBERT-PII, OpenMed privacy-filter (v1/v2),
   perplexity `pplx-pii-masking`, GLiNER zero-shot, OpenAI privacy-filter.
   Milliseconds, fixed taxonomy.
3. **LLM-as-judge / LLM-as-router** — flexible, expensive, and (per §6) not
   monotone in model size.

**The gap to claim:** these are published as *detectors*, with entity-level F1 on
their own held-out sets. Nobody publishes **leak rate at a routing decision** on
a common corpus, which is the number a person deploying a privacy gate actually
needs. That is what this post contributes.

### §4 Candidate models

Table of what is publicly available: parameter count, label-space size, context
window, license, multilingual or not.

**Frame the axis that turns out to matter, and it is not accuracy.** It is
**label-space coverage**. pplx expresses 9 categories and cannot represent 11 of
the 24 canonical categories this corpus exercises — the widest gap of any model
measured — yet leaks only 0.80%. Coverage and leak rate are close to
independent (§5.4: taxonomy gaps hurt category coverage, not leak rate). Setting
that expectation in §4 makes §7's turn land instead of confusing.

### §5 The dataset

`nvidia/Nemotron-PII`, test split, sampled by `build_nemotron_corpus.py`.

- 55 distinct gold labels; ~35 direct identifiers plus ~12 quasi-identifiers and
  sensitive attributes (gender, race, sexuality, religion, politics, education,
  employment, age, language, blood type, occupation, biometric). **Most detectors
  have no class at all for that second group.**
- Documents are short: p50 ~744 chars, p99 ~3,259, max 7,191 chars = **1,737
  tokens**. Zero documents truncate for any model, so the mmBERT-32k /
  OpenMed-128k / pplx-4k context comparison is apples-to-apples and context
  length could not have differentiated these results (§7 of the benchmarking
  doc).
- Two corpora: 2,500 cases (LLM-as-router runs) and 20,000 (detector runs).
  **Absolute rates are not comparable across them** — different samples. Say this
  wherever a 2.5k row sits next to a 20k row, which is the main results table.

**Put the benign gap here, in full, not in §11.** A scan of 30,000 test rows
found **zero** documents with no PII — Nemotron-PII is a pure-positive generation
dataset. Consequences, stated plainly:

- Precision, FP-rate and F-beta are **statistically empty** across every run.
- **A model that flagged every single document would score identically to a
  precise one** on every metric reported.
- Only recall / leak rate carries information.

This costs nothing and buys the reader's trust for the next 3,000 words. Anyone
who knows this space will go looking for it; being told is very different from
finding it in a footnote. Pair it with the strawman in E9 so the concession is
visual rather than verbal.

### §6 Baselines

The escalation, all on the 2.5k corpus:

| Baseline | Leak rate | Recall |
|---|---|---|
| Regex | 18.7% (467/2500) | 81.30% |
| embeddinggemma-300m (semantic similarity) | 100% (20/20) | 0.00% |
| Qwen3-Embedding-0.6B | 90% (18/20) | 10.00% |
| Qwen3-Embedding-4B | 90% (18/20) | 10.00% |
| LLM Qwen3.5-9B | 6.16% (154/2500) | 93.84% |
| LLM Qwen3.5-2B | 3.28% (82/2500) | 96.72% |
| LLM Qwen3.5-0.8B | 2.84% (71/2500) | 97.16% |

Two stories here, and the second is the most quotable finding in the whole
corpus.

**(a) Semantic similarity is the wrong tool for this.** Embedding classifiers
were the *worst* configurations tested. Caveat honestly: n=20, zero benign, and
these rows were transcribed inverted in an earlier spreadsheet (§6.1) — the
0/20, 2/20, 2/20 that looked like the best rows in the table were the
`PII -> local (TP, correct)` column, not the leak column. Either re-run them
(E5) or present them explicitly as a directional pilot. **Do not publish a
20-case row without labelling it as such.**

**(b) The size paradox — the headline.** The 9B leaks *more than twice* what the
0.8B leaks. The per-category miss enrichment says why:

| Category | support | Qwen3.5-9B | Qwen3.5-2B | Qwen3.5-0.8B |
|---|---:|---:|---:|---:|
| WEB_URL | 893 | **1.92x** | 1.81x | **1.97x** |
| ORG_COMPANY | 672 | **1.65x** | 1.22x | 1.68x |
| ADDRESS_LOCATION | 991 | **1.52x** | 1.29x | 0.82x |
| NETWORK_ID | 310 | 0.47x | 0.98x | **2.04x** |
| CREDENTIAL_SECRET | 469 | 0.21x | 0.59x | 1.05x |
| DATE_OF_BIRTH | 312 | **0.00x** | **0.00x** | 0.11x |
| PERSON_NAME | 1213 | 0.11x | 0.33x | 0.35x |

The 9B's misses concentrate in **judgment** categories — URLs, company names,
addresses, demographics. It reasons its way *out* of flagging. The 0.8B's
concentrate in **technical identifiers** — it fails to recognize them.
**Judgment errors vs capability errors, and they want opposite fixes**: policy
and prompt tightening for the 9B, a bigger model for the 0.8B.

Use the verbatim logged rationale as the money quote: `"The request contains no
personal information"` on a document labeled
`company_name, education_level, occupation, sexuality, url`.

Explain enrichment once, in one line: `P(missed | doc has category) / overall
miss rate`; `>1.0` = over-represented among the documents a model let through.
It exists because only 8 of 2,500 documents contain a single canonical category,
so a missed document is missed for ~7 categories at once and raw per-category
attribution is meaningless for a document-level decision.

### §7 Encoder detectors

The main results table (20k corpus, all CPU):

| Model | Leak rate | Recall | E2E runtime |
|---|---|---|---|
| GLiNER (nvidia/gliner-PII) \* | 0.005% (1/20000) | 99.995% | partial run |
| OpenMed privacy-filter-ml-v2 | 0% (0/20000) | 100% | 8.12hr (router) |
| OpenMed privacy-filter-multilingual | 0.07% (14/20000) | 99.93% | 2.15hr |
| mmBERT32K-PII (safetensors) | 0.12% (25/20000) | 99.87% | 1.01hr |
| mmBERT32K-PII (ONNX via router) | 0.24% (49/20000) | 99.76% | 7.19hr |
| **pplx-pii-masking (safetensors)** | **0.80% (159/20000)** | **99.20%** | **2.55hr** |
| **pplx-pii-masking (ONNX)** | **0.80% (159/20000)** | **99.20%** | 2.17hr (not comparable) |
| OpenAI/privacy-filter | 5.31% (1062/20000) | 94.69% | partial run |

\* **GLiNER needs an asterisk everywhere it appears.** `pii_gliner_eval.py`
extracts the label vocabulary from the corpus itself and hands those 55 gold
label names to the model at inference — it is told exactly what to look for, in
the dataset's own words. It is a calibration reference, not a competitor.

Also flag in the table's caption: the 7.19hr / 8.12hr figures are **router E2E**
(HTTP round-trips plus the routed model answering), not classification. Direct
classification over the same 20,001 cases is ~28 min. Readers will otherwise
conclude the gate costs hours.

**Then make the turn — and make it with characters, not per-category recall.**
The table above is the setup for the post's best moment: every model in it is
"essentially perfect", the top six sit inside one percentage point, and the
column cannot rank them. Follow it immediately with E2's result (§5.6 of the
benchmarking doc):

| Model | char P | char R | **char F1** | doc leak |
|---|---:|---:|---:|---:|
| OpenMed privacy-filter-ml-v2 | 0.9769 | 0.9359 | **0.9559** | 0.00% |
| pplx-pii-masking | 0.9725 | 0.7330 | **0.8360** | 0.795% |
| mmBERT32K-PII | 0.9202 | 0.6842 | **0.7849** | 0.125% |
| *flag everything* | *0.1458* | *1.0000* | *0.2544* | *0.00%* |

**The ordering inverts.** mmBERT leaks 6x fewer documents than pplx and is the
worse detector by characters — it fires *somewhere* on nearly every document
while covering much less of what is actually in it. One table, and the reader
understands why document-level scoring was not enough without being lectured
about it.

This also **replaces E9's strawman** with a measured one: the *flag everything*
row is a real number now (0.2544), not a rhetorical device, and every model
clears it by a wide margin.

Then per-label character recall for the blind spots — `time` at 60.6 / 46.7 /
75.2% across all three, `country` at 23.3 / 28.4% for the two smaller models —
and note that this view needs **no taxonomy mapping at all**, which retires the
lenient-vs-strict caveat for these three rows.

**Then make the turn.** Four models are bunched between 0.00% and 0.24% and the
leak rate has stopped discriminating. Two moves recover the signal:

**(a) Per-category recall.** The §5.1 table. Key readings to carry over:

- **mmBERT's demographic failure is genuine, not a taxonomy gap.** It *has*
  Presidio's `NRP` class and fires it on **5.0%** (race/ethnicity/language) and
  **5.2%** (belief/political) of the documents that need it. pplx and OpenMed
  score zero there because they have no such class at all. **These are two
  completely different situations and a "missed categories" column conflates
  them.** This is the single most important methodological point in the section.
- **BIOMETRIC (1,958 docs) and EDUCATION (1,416) are total blind spots across
  all four production detectors.** Only GLiNER covers them, and only because it
  was handed the label names.
- **OpenMed v1 -> v2 was a real jump**: ORG_COMPANY 13.4 -> 93.1%, GENDER
  53.1 -> 95.7%, CREDENTIAL_SECRET 79 -> 98%, DATE_TIME 80.4 -> 97.4%. Remaining
  weak spot: OCCUPATION_EMPLOYMENT at 52.5%.

**(b) Lenient vs strict credit.** pplx's single `account_number` expands to
`{FINANCIAL_ACCOUNT, GOV_ID, INTERNAL_ID, MEDICAL}`:

| Category | lenient | strict | Δ |
|---|---:|---:|---:|
| DATE_OF_BIRTH | 99.8% | 63.9% | −35.9 |
| GOV_ID | 98.0% | 71.1% | −26.9 |
| MEDICAL | 93.0% | 60.1% | −32.9 |
| INTERNAL_ID | 96.5% | 55.1% | −41.4 |

**OpenMed v2 is essentially unchanged under strict; pplx and openai-pf
collapse.** State the reading explicitly: this is a **granularity** result, not
an accuracy one. pplx genuinely found something at those positions ~98% of the
time. **For routing, lenient is the right lens. For masking or redaction policy
that treats an SSN differently from a customer ID, strict is.** That sentence is
the whole payoff of the section and connects directly back to Router A.

Report both modes. Neither is "the" right answer without span offsets to
disambiguate against.

### §8 ONNX

Why: the router's classifier path is `onnxruntime` — no torch in the serving
process, CPU-friendly, one artifact.

**Two traps worth publishing** (both are real time sinks another team will hit):

1. **The export dies on Windows *after* succeeding.** `torch.onnx`'s progress
   printer emits U+2705; a cp1252 stdout cannot encode it, so the export raises
   `UnicodeEncodeError` with the graph already built. Reads like an export bug,
   is a console-encoding bug.
2. **Do not export pplx from `-vllm-tmp`.** That repo repacks the same weights as
   a stock `Qwen3ForTokenClassification` with a top-level `"is_causal": false` —
   a key stock transformers does not read (huggingface/transformers#39554). It
   loads with correct tensor shapes, looks entirely self-consistent, and is
   silently **causal instead of bidirectional**. The original repo's vendored
   `modeling_pplx_qwen3.py` flips `is_causal` per layer *and* rebuilds the mask
   bidirectionally.

Trap 2 generalizes into the section's thesis: **a conversion can be
self-consistent and wrong**, so equivalence has to be measured.

**And a third methodological point that is arguably the most useful:** the pplx
ONNX graph deliberately stops at raw per-token logits — the constrained BIOES
Viterbi is **not** baked in. Scoring ONNX by argmax while the safetensors run was
scored by Viterbi would confound a backend difference with a decoder difference
and make the delta uninterpretable. So the ONNX scorer imports the checkpoint's
own `ViterbiDecoder` and feeds it the ONNX logits. **The two runs then differ in
exactly one variable.**

The parity result:

| Check | Result |
|---|---|
| Confusion matrix | 19841 / 159 / 1 / 0 — identical, both runs |
| has_pii decision agreement | **20,001 / 20,001 (100.0000%)** |
| Routing decision flips | **0** |
| Exact label-set agreement | 20,001 / 20,001 (100.0000%) |
| The 159 missed documents | **the same 159 case names**, set difference empty both ways |
| Per-category recall, lenient + strict | identical in every cell |
| Logit max-abs-diff (real 806-char doc) | 1.0e-5 |

**Say why the same-case-names row is the one that matters:** identical counts
alone would not prove equivalence — compensating errors hide there. That check is
what closes the gap. It is a transferable methodology point, and it is the reason
this section is worth writing at all.

### §9 Router B — how it decides

Structure: the discrepancy, the explanation, the proof, then the schema, then the
curve.

1. **The discrepancy.** mmBERT reads 0.12% in one row and 0.24% in another. Same
   weights.
2. **The explanation.** Two decision rules, not two backends:

   | Path | Rule |
   |---|---|
   | `pii_ner_eval.py` (offline) | argmax per token; fire if any non-special token's argmax is not `O` |
   | the router | softmax per token → max over tokens per label → fire if any non-`O` label >= `min_score` |

   A per-token softmax over 35 labels sums to 1, so a label above 0.5 at a token
   *is* that token's argmax. **`min_score >= 0.5` is strictly stricter than
   argmax.**
3. **The proof.** Containment 25/25 with 0 violations; 24 threshold-only extras;
   0 mismatches under a shared argmax rule across 13,708 cases; the shipped
   threshold bracketed to (0.4963, 0.5043] from the run itself. Also worth one
   line: **special tokens are excluded from the max, and this was measured** —
   including them puts all 49 known leaks above 0.5 because this model's `<bos>`
   always fires a label.
4. **What the rule actually costs — in characters.** This is the strongest
   addition E2 makes to this section, because it reverses the reader's takeaway
   from step 3. At document level the two rules differ by 0.12 percentage
   points and the honest summary is "the threshold barely matters." Score the
   same two rules on the same ONNX graph in the same process, over characters:

   | mmBERT rule | char P | char R | char F1 | doc leak |
   |---|---:|---:|---:|---:|
   | argmax | 0.9202 | 0.6842 | **0.7849** | 0.125% |
   | `min_score` 0.5 | 0.9494 | 0.4849 | **0.6419** | 0.245% |

   **The shipped threshold throws away 29% of the PII characters mmBERT can
   find.** A router only has to notice one entity per document, so it never
   pays that bill — which is exactly why the document-level view says the
   choice is cheap. A masking or redaction path pays all of it.

   The same shape appears on pplx from the other direction: its own constrained
   BIOES Viterbi decoder **nearly doubles** its document leak rate against
   plain argmax (159 vs 84) while buying precision (0.9627 → 0.9725). A
   constrained decoder is a trade, not a free upgrade.

   **The generalizable claim, and a good candidate for the post's closing
   line: route with the loose rule, mask with the strict one.** Same weights,
   same graph, two jobs, two thresholds. Note also that this is a *cleaner*
   proof than step 3's: there the backend was held constant across two runs by
   argument, here both rules run over one graph in one process.

5. **The schema, briefly.** Policy is data, not code: first-match-wins rule
   resolution, fail-open default, `on_error: "match_false"`, band tests
   (`min_score`/`max_score`), cheap conditions (keywords, char bounds, metadata)
   evaluated before model-backed ones. Point at `docs/dev/router-policy.md`
   rather than reproducing the schema.
6. **The curve** (mmBERT ONNX):

   | `min_score` | leaks | recall | vs argmax |
   |---|---|---|---|
   | 0.10 | 9 | 99.955% | −16 |
   | 0.30 | 25 | 99.875% | +0 |
   | 0.50 (shipped) | 49 | 99.755% | +24 |
   | 0.70 | 119 | 99.405% | +94 |

   **Publish this only with its caveat, in the same breath.** With one benign
   case, lowering `min_score` has no measurable cost *here*, so the curve is
   one-sided and is **not** tuning advice. And the apparent headroom is worth
   about half what it looks like: of the 24 threshold-only leaks, the
   sub-threshold signal sits on a category the document **does not contain** in
   13 of 24 cases. Those recover by accident. For a binary route an accidental
   catch still routes correctly — but the same spurious firing is exactly what
   would cost precision on benign traffic, the thing this corpus cannot measure.

   This is the most misusable table in the post. If it cannot be published with
   the caveat visually adjacent (same figure, not a footnote), cut it and keep
   the prose.

### §10 Cost of the gate

**Not currently answerable from any table.** See E3 below. This is the first
question a systems audience asks and the post is weak without it.

### §11 Limitations

Short, because §5 already conceded the big one. Cover:

- **The benign arm.** Still the load-bearing gap. Character precision (§7)
  narrows it — an over-tagging model is now penalized on positive documents —
  but it is bounded by PII density, not a true FP-rate, and it says nothing
  about behavior on benign traffic. E1 remains outstanding.
- **Character F1 covers three models, not eight.** The LLM routers and embedding
  classifiers emit a decision and never a span, so they are document-level
  permanently — a property of the method, not a gap. GLiNER, OpenMed v1,
  OpenAI/privacy-filter and mmBERT-safetensors simply have not been run yet.
- **`pii_taxonomy.py`'s mappings are editorial judgment and should be reviewed,
  not assumed** — two were wrong on the first pass and one produced a fake "0.2%
  biometric failure" for OpenMed that was purely a bad mapping. Worth adding
  that character-level per-label recall **needs none of it**, which is part of
  why it is the better diagnostic.
- **The GLiNER asterisk.**
- **No language field in this corpus** (§4 of this plan).

### §12 What we'd ship

A recommendation with numbers, not a shrug. **E2 sharpens this considerably:**

- **OpenMed privacy-filter-ml-v2 is the recommendation**, and now for a reason
  beyond a 0% leak rate that four models tie on: it wins character F1 by 0.12
  (0.9559 vs 0.8360), holds >90% recall in every span-length bucket, and is the
  only one of the three without a catastrophic per-label hole. The cost is size
  — a 5.6 GB fp32 graph, ~3x slower per document than mmBERT.
- **Match the decision rule to the job.** Both E2 decoder findings point the
  same way: `min_score` 0.5 costs mmBERT char recall 0.6842 → 0.4849 while
  barely moving document leak, and pplx's own Viterbi decoder nearly doubles its
  leak rate against argmax while buying precision. **Route with the loose rule,
  mask with the strict one.** Most posts never separate these two jobs.
- State what the gate costs (E3) and what it does not measure (over-routing).

---

## 3. Reframings from `pii_benchmarking.md`

Four places where the source doc's framing is right for an internal record and
wrong for a post.

**(a) "The metric is a binary" — apology to justification.** Covered in §1.1.
Ordering does the work; the sentence barely changes.

**(b) Do not claim "conversion didn't lose anything."** That is measured for
**pplx only**. mmBERT-ONNX and OpenMed-v2-ONNX rest on router logs and have not
been parity-checked. The truthful version is a *better* story:

> We verified one export decision-identical — and the one gap that looked like
> conversion damage turned out not to be conversion at all.

A blanket lossless claim invites the reader to find the two unchecked rows. The
honest version turns §6.4 into supporting evidence for the ONNX section instead
of a separate topic.

**(c) The size paradox is a finding, not a table row.** In the benchmarking doc
it is a paragraph inside §5.3. In the post it is a subsection with the enrichment
table, the two-failure-modes reading, and the verbatim rationale quote.

**(d) Character F1 is a caveat internally and the post's central turn.** In the
benchmarking doc §5.6 sits where it belongs for a record: after the
document-level results, as the more careful re-measurement. In the post it is
not a footnote to §7 — it is the moment §7 exists for. The leak-rate table is
built so the reader notices that eight models are all "essentially perfect" and
the column cannot rank them; the character table then ranks them and **inverts
the order**. Deliver it as a turn, not as an appendix, and do not soften the
leak-rate table beforehand to make the turn less surprising.

---

## 4. Claims the post must not make

- **~~No character-level F1 results.~~ E2 is done — see §5.6 of the benchmarking
  doc.** Character F1 on the full 20,001-case corpus is now a number of record
  for the three ONNX detectors, and the post should lead §7 with it. Two
  standing constraints remain:
  - **The single-document `nemotron-pii-15485` figures (pplx 0.524, mmBERT
    0.625) are still not results of record.** Their gold spans were
    reconstructed by substring search with inferred boundaries. The real
    corpus-wide numbers superseded them; use those. The case-15485
    *alignment diagram* remains a good explanatory figure — label it a worked
    illustration and never mix its numbers into a results table.
  - **Character F1 is undefined for the LLM routers and the embedding
    classifiers.** They emit a decision, never a span. Those rows stay
    document-level permanently; say so once rather than leaving a blank cell.
- **No document-level *precision*, FPR or specificity number, for any model.**
  n_benign = 1. **Character precision is now measurable and measured**
  (0.92-0.98 across the three detectors, against a 0.1458 flag-everything
  floor) — but it is bounded by PII density, not a true FP-rate, and it says
  nothing about behavior on benign traffic. E1 is still the load-bearing gap.
- **No multilingual claim.** There is no language field in this corpus and
  Nemotron-PII is English. The multilingual models (mmBERT, OpenMed-multilingual)
  have **no measured multilingual advantage here.** One sentence in §11; do not
  imply otherwise in §4's model table.
- **No cross-corpus comparison** without saying so. 2.5k and 20k are different
  samples; the LLM rows and detector rows are not directly comparable. The
  normalized per-category profiles are.
- **No "ONNX is faster" claim.** The 2.17hr pplx-ONNX figure came from a resumed
  run at onnxruntime's default thread count against torch on 16 threads. E4
  fixes this; until then the runtime column compares nothing.
- **No entity counts read off `detected=`.** It is a deduplicated *type* set:
  two spans of one type appear once, eight emails appear once.
- **Nothing routed on pplx's sensitivity head.** 9.16% recall vs 99.20% for the
  span head. Worth one sentence as a deployment trap — the model card's own
  example shows three correct spans at `sensitivity=0.027` — but never as a
  measured configuration.

---

## 5. Assets to produce

| Asset | Section | Source | Status |
|---|---|---|---|
| Routing flow diagram (prompt → classifier → local/cloud) | §2 | — | to draw |
| Trimmed policy JSON (3 clauses + elision) | §2 | `l2_pii_onnx_pplx_masking/policy.json` | trim |
| Model landscape table | §4 | model cards | compile |
| Baseline escalation table | §6 | §6 of benchmarking doc | have |
| LLM miss-enrichment table | §6 | `category_enrichment_llm.json` | have |
| Main detector results table | §7 | §6 of benchmarking doc | have |
| Per-category recall heatmap/table | §7 | `category_recall_lenient.json` | have |
| Lenient-vs-strict delta table | §7 | `category_recall_strict.json` | have |
| ONNX parity table | §8 | §6.2 | have |
| argmax-vs-`min_score` rule diagram | §9 | §6.4 | to draw |
| `min_score` curve + caveat, one figure | §9 | `min_score_curve.json` | have |
| **Character-F1 headline table (3 detectors)** | **§7** | **§5.6** | **have** |
| **Per-label character-recall table** | **§7** | `pii_char_f1_report.py --by-label` | **have** |
| **pplx viterbi-vs-argmax decoder table** | **§8/§9** | **§5.6** | **have** |
| Character-alignment illustration (case 15485) | §4/§11 | §5.5 | have, label clearly |
| False-negative gallery | §7 or §11 | run logs | E10 |
| Latency table | §10 | — | **E3** |

---

## 6. Experiment plan

Ranked by blog value per hour. Tier 1 items change what the post can claim; Tier
2 shores up claims it already makes; Tier 3 is cheap polish.

### Tier 1 — the post is materially weaker without these

#### E1. Build a benign arm (Nemotron hard negatives)

**Unlocks:** §5's honesty concession becomes a measured over-route rate; every
precision/FPR cell in the post; a real answer to "does this thing just send
everything local."

Without it the post's central claim — route PII local, everything else to the
big model — has **no over-routing number at all**, and the data cannot detect a
router that routes 100% of traffic local. That is the load-bearing gap.

**Method (already decided — construct, don't sample):** take Nemotron documents
and substitute every PII span with generic non-identifying text. Ground truth is
PII-free **by construction**, so no second detector is needed and there is no
circularity. Domain- and style-matched to the positive arm, which isolates the
identifier signal from topic confounds. Filtering real prompts with a detector is
**circular** — it deletes exactly the documents models flag and drives measured
FP toward zero artificially.

Known wrinkle: attribute spans woven into prose (gender, religion, occupation)
substitute awkwardly and need care. Consider excluding those documents from the
first pass rather than producing unnatural text.

**Size:** ~2,000 hard negatives. **Run:** pplx, OpenMed-v2, mmBERT, and the
0.8B LLM router.

**Do this in the same builder change as E2** — both need spans preserved
through `normalize_text()` and the prompt prefix.

#### E2. Preserve span offsets, then character F1 — **DONE**

**Result: it separates the models far more than the leak rates do, so the
answer to the doc's own "if it separates them, spend the 15 hours" is yes.**
Full 20,001-case runs, not the 500-case Phase 1 slice — the slice was skipped
once mmBERT came in at 24 minutes.

| Model | char P | char R | **char F1** | doc leak |
|---|---:|---:|---:|---:|
| privacy-filter (OpenMed ml-v2) | 0.9769 | 0.9359 | **0.9559** | 0.00% |
| pplx-pii-masking | 0.9725 | 0.7330 | **0.8360** | 0.795% |
| mmBERT32K-PII | 0.9202 | 0.6842 | **0.7849** | 0.125% |
| *flag-everything strawman* | *0.1458* | *1.0000* | *0.2544* | *0.00%* |

Three models inside one percentage point of document leak spread across **0.16
of character F1**, and the ordering inverts: mmBERT leaks 6x fewer documents
than pplx and is the worse detector by characters. That single fact is the
strongest argument in the post for why the metric had to change, and it is a
better version of the E9 strawman than E9 was going to be.

**What the post gains, beyond the table:**

1. **A real precision number.** 0.92-0.98 against a 0.1458 floor. §5's honesty
   concession gets a partial answer instead of a pure apology.
2. **Two decision-rule findings that document scoring hides.** mmBERT's shipped
   `min_score` 0.5 costs char recall 0.6842 → 0.4849 while moving document leak
   only 0.12% → 0.24%. pplx's own Viterbi decoder *nearly doubles* its leak rate
   against plain argmax (159 vs 84) while buying precision. Both are the same
   shape: **route with the loose rule, mask with the strict one** — and the post
   can say that with numbers.
3. **Per-label recall with no taxonomy in the loop**, which kills the
   lenient-vs-strict caveat (§5.2) for these three models. New findings: `time`
   is a shared blind spot (60.6/46.7/75.2) that §5.1's `DATE_TIME` row hides
   behind easy dates; geographic granularity (`country` 23.3%/28.4%) is where
   the two smaller models actually break, invisible inside `ADDRESS_LOCATION`.
4. **A validation gate worth one sentence in §8.** The new pipeline reproduces
   every known document-level number exactly — mmBERT 25 and 49 leaks, the 24
   threshold-only cases, pplx's 159 — from code sharing nothing with the
   original scorers. It is the strongest reproducibility claim the post has.

**Cost, actual:** ~3.5 hr of CPU for the three full runs, plus the builder
change. Not the 10-15 hr estimated, because only three models were in scope and
the corpus never needed rebuilding — `annotate_gold_spans.py` back-fills
offsets onto the existing corpus, so every document-level row stays comparable.

<details>
<summary>Original plan (kept for the design rationale)</summary>

#### E2 (original). Preserve span offsets, then character F1 (Phase 1 only)

**Unlocks:** a character-F1 section at all; the only way to rank four models
currently bunched at 0.00–0.24% leak; separates "wrong label" from "wrong
location."

**The builder change:** raw `start`/`end` index into the *original* text, and the
builder edits that text twice before any model sees it — `normalize_text()`
collapses every whitespace run to one space, then `wrap_in_message()` prepends
one of 8 random prefixes (up to 46 chars). The fix is an index map through the
whitespace collapse plus `len(prefix)`. **A re-download does not recover this**,
and neither does searching for the span text (gold strings repeat — one case has
`Richard` at 5+ positions).

**Phase 1 is cheap: pplx only, ~500-case slice, minutes not hours.** pplx already
computes `start`/`end` and just discards them. If character F1 separates the
models more than the leak rates do, commit the ~10–15 hr of re-inference for the
remaining detectors; if not, the post keeps document-level scoring and says why.

Why it is worth it: **it measures precision without a benign arm.** At character
level an over-tagging model is penalized on *positive* documents. It does not
retire E1 — the ceiling is the document's PII density, so flagging every
character scores precision ~0.24 on the worked example rather than 0 — but it
stops precision being *completely* unmeasured.

**Prefer character F1 over span-exact F1.** Models fragment: mmBERT emits one ID
as 6 pieces, pplx emits one email as `' d'`,`'aniel'`,`'@'`,… Span-exact scores
near-zero for correct detections.

Undefined for LLM routers and embedding classifiers — they emit a decision, never
a span. Those rows stay document-level permanently; a property of the method, not
a gap.

</details>

#### E3. Measure what the gate costs per prompt

**Unlocks:** §10, which currently cannot be written.

The runtime column mixes classification with the routed model answering — 7.19hr
E2E vs ~28 min for classification alone over the same 20,001 cases. No table
answers "what does the PII check add to my p95," which is the first question a
systems reader asks.

**Measure:** classifier-only p50/p95 per prompt, per model, on CPU (and NPU/GPU
if available), plus the end-to-end delta against a no-router baseline. Report per
prompt, not per corpus.

### Tier 2 — shores up claims already being made

#### E4. Re-time pplx ONNX on an uninterrupted run

The 2.17hr figure is a resumed run at onnxruntime's default thread count against
torch on 16 threads. Re-run uninterrupted with `--intra-op-threads` pinned
(~2.5hr) if the post wants to say anything about ONNX runtime. Otherwise drop the
runtime comparison for that row entirely.

#### E5. Re-run the three embedding classifiers on the real corpus

n=20, zero benign, and previously transcribed inverted. "Semantic similarity is
the wrong tool for PII detection" is a good beat but cannot rest on 20 cases —
readers will notice. Embeddings are fast; this is cheap.

#### E6. OpenMed-v2-ONNX: direct verbose eval + parity check

Its 0% row comes from a **router** log, so it carries exactly the confound that
made the mmBERT rows look like a backend difference (`pii_benchmarking.md` §6.4).
If OpenMed-v2 is the §12 recommendation, it needs the same treatment pplx
got: a direct verbose eval plus a `--parity-against` diff. Same for mmBERT-ONNX,
which still rests on a router log (though its gap is now explained).

#### E7. Run the local-default policy variant, and commit the policies

`l2_pii_onnx_pplx_masking/policy_local_default.json` (currently untracked) routes
PII → `Qwen3.5-0.8B-GGUF` with `Qwen3.5-9B-GGUF` as default — local-vs-local
instead of local-vs-cloud. Running it answers "what does routing cost in answer
quality" **without a cloud dependency**, and makes the post reproducible for
readers with no API key.

Also close the open item: **commit the policy JSONs that back table rows.**
`policy_smoke.json` produced the mmBERT-ONNX row and is not in the repo — its
`min_score` had to be recovered from the run itself. A published post should not
have a headline row whose configuration lives only in someone's working
directory.

### Tier 3 — cheap, high narrative payoff

#### E8. `min_score` curve for pplx

Currently only mmBERT has one. One sweep on the ONNX export (~28 min) plus
arithmetic. Shows the curve is model-dependent rather than a property of the
router, and combined with E1 becomes an actual ROC instead of a one-sided curve
that needs three sentences of caveat.

#### E9. Print the "flag everything" strawman beside every leak rate

Costs zero compute. Today it **ties the best model** on every metric in the
table — which *is* the §5 argument, made visually instead of verbally. Strongest
possible way to concede the benign gap.

#### E10. A false-negative gallery

10–20 real leaked documents with the PII highlighted, pulled from the run logs.
Zero compute, pure narrative. Readers remember three documents better than
`Social Science 8.8x`.

#### E11. Per-domain leak rates for every model

`domain=` and `doc_type=` are already in each case's `note` field, so this is a
parse away — **no inference at all**. Generalizes the existing pplx finding
(Social Science 8.8x, Environmental 5.0x, Elections 4.2x, Sports 3.6x) across the
whole model set for free. Likely the best value-per-minute item in this file.

### Summary

| # | Experiment | Cost | Unlocks | Tier |
|---|---|---|---|---|
| E1 | Benign arm (2k hard negatives) | builder + 4 runs | precision, FPR, over-route | 1 |
| ~~E2~~ | ~~Span offsets + char F1~~ **DONE** | ~3.5 hr actual | char-F1 §7, real precision, 2 decoder findings | ~~1~~ |
| E3 | Per-prompt gate latency | short | §10 | 1 |
| E4 | Re-time pplx ONNX pinned | ~2.5hr | runtime comparison | 2 |
| E5 | Embedding classifiers on 20k | low | a defensible §6(a) | 2 |
| E6 | OpenMed-v2-ONNX direct + parity | ~8hr | §12 recommendation | 2 |
| E7 | local-default policy + commit policies | ~3hr | reproducibility | 2 |
| E8 | pplx `min_score` curve | ~30min | model-dependent curve | 3 |
| E9 | "Flag everything" strawman column | 0 | visual §5 concession | 3 |
| E10 | False-negative gallery | 0 | narrative | 3 |
| E11 | Per-domain leak rates, all models | 0 (parse only) | a whole subsection | 3 |

**Minimum set for a publishable post: 6.1, 6.3, 6.7, 6.9, 6.11.** That yields an
over-route number, a latency number, committed configs, an honest baseline, and a
free extra subsection. 6.2 Phase 1 is a cheap option worth exercising before
deciding whether the post gets a character-F1 section or a "here's what we'd
measure next" section.

---

## 7. Commands

```bash
# detector evals (no Lemonade server needed) - ALWAYS --verbose
python test/eval/pii_ner_eval.py    --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose
python test/eval/pii_pplx_eval.py   --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose
python test/eval/pii_gliner_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose

# ONNX + parity diff against a safetensors run
python test/eval/pii_pplx_onnx_eval.py --corpus-dir test/conformance/routing/1/l2_pii_nemotron_20k --verbose \
    --parity-against <safetensors log>

# re-score existing logs per category - no new inference
python test/eval/pii_category_recall.py            --corpus-dir <corpus> <corpus>/runs/*.log
python test/eval/pii_category_recall.py --strict    --corpus-dir <corpus> <corpus>/runs/*.log
python test/eval/pii_category_recall.py --doc-level --corpus-dir <corpus> <log> ...

# min_score sweep + curve (6.8)
python test/eval/pii_min_score_sweep.py --corpus-dir <corpus> ...
python test/eval/pii_min_score_curve.py --sweep <corpus>/runs/min_score_sweep.jsonl

# router replay (6.7)
python test/eval/pii_routing_eval.py --corpus-dir <corpus> --policy <policy.json>
```

Two invariants worth repeating because they silently destroy a run:

- **`--verbose` always.** A non-verbose log records only failures, which makes
  "passed" and "not yet run" indistinguishable and breaks both `--resume-from`
  and every re-scoring path.
- **Check resumed summaries before quoting them.** `--resume-from` reconstructs
  tallies from the log; a metric it forgets to restore ends up reported over a
  different denominator than the rest of the summary. This already happened once
  (pplx-ONNX sensitivity head over 15,568 cases while every primary metric
  covered 20,001).

---

## 8. Related files

| File | Purpose |
|---|---|
| `pii_benchmarking.md` | Source of record for every number in the post |
| `privacy_filter_ml_v2_onnx_repro.md` | The gpt-oss reinterpretation trap, same shape as the `-vllm-tmp` one |
| `docs/dev/router-policy.md` | Policy schema — link, don't reproduce |
| `test/conformance/routing/README.md` | Router conformance corpus and v1 semantics |
| `l2_pii_nemotron/` | 2,500-case corpus (LLM-as-router runs) |
| `l2_pii_nemotron_20k/` | 20,000-case corpus (detector runs) |
