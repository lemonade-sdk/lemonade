# LLM-as-router PII benchmark — runbook

How to reproduce the "LLM Qwen3.5-* as the router" rows of the PII routing
benchmark (leak rate + per-prompt latency) on any machine. Written so an agent
can follow it end to end; nothing here needs a GPU beyond what the router model
itself needs.

## What is being measured

Each case in the corpus is one Nemotron-PII document wrapped in a chat request.
Lemonade's router asks the *router model* (a small local Qwen3.5) to pick a
candidate; PII documents are supposed to stay on the local model and only
PII-free requests go to the cloud candidate. A PII document sent to the cloud
is a **leak**. The eval reports leak rate, recall, and per-case end-to-end /
routing / routed-model timings.

Three things must line up or the run is silently wrong:

1. The **router policy** names the local model as `routing.router.model`,
   `routing.default_model` and first candidate, and the cloud model as the
   other candidate. The prompt text mentions both names.
2. The corpus's `decision.route_to` names the **same local model**. The
   committed corpora hard-code `Qwen3.5-0.8B-GGUF`, so any other router model
   fails every case unless the corpus is rewritten.
3. The **cloud candidate is a real, distinctly named model**. Standing in a
   second local Qwen (`Qwen3.5-9B-GGUF`) for the cloud inflates leaks: with two
   `Qwen3.5-*-GGUF` names the 2B confuses which one is "private" (~70% of its
   leaks in the aborted 2026-09-11 run were of that kind). Use
   `fireworks.kimi-k2p6`.

`test/eval/prepare_llm_router_run.py` does 1 and 2 for you.

## Prerequisites

- A Lemonade build with the router (`collection.router`) — `lemond` running on
  `http://localhost:13305` (or pass `--base-url`).
- The router model pulled: `lemonade pull Qwen3.5-2B-GGUF` (or `Qwen3.5-0.8B-GGUF`,
  `Qwen3.5-9B-GGUF`).
- Fireworks configured so `fireworks.kimi-k2p6` appears in
  `GET /v1/models`: set `LEMONADE_FIREWORKS_API_KEY` in `lemond`'s environment
  before starting it (or `POST /v1/cloud/auth`). Verify with:

  ```bash
  curl -s http://localhost:13305/v1/chat/completions -H "Content-Type: application/json" \
    -d '{"model":"fireworks.kimi-k2p6","messages":[{"role":"user","content":"Say OK"}],"max_tokens":3}'
  ```

- Python deps: `pip install -r test/requirements.txt` (only `requests` is used).
- Corpus: `test/conformance/routing/1/l2_pii_nemotron_20k/` (20,000 PII cases +
  1 benign, committed). The 2,500-case `l2_pii_nemotron/` is a different
  sample; don't compare absolute rates across the two.

## Steps

All commands from the repo root.

### 1. Prepare a run directory for the router model

```bash
python test/eval/prepare_llm_router_run.py --local-model Qwen3.5-2B-GGUF --out-dir /tmp/run_qwen2b
```

Writes `cases.jsonl` (route_to rewritten), `stats.json`, and
`policy_llm_Qwen3.5-2B-GGUF.json`. The prompt is copied verbatim from
`test/conformance/routing/1/l2_pii_regex/policy_llm.json` (the prompt quoted in
the blog) with only the two model names substituted — do not edit the prompt
per run, every row must share it. Output should say
`route_to : {'Qwen3.5-2B-GGUF': 20000, 'fireworks.kimi-k2p6': 1}`.

Put the run dir outside the repo — `cases.jsonl` is a 42 MB copy.

### 2. Smoke test (15 cases, ~1 min)

```bash
python test/eval/pii_routing_eval.py \
  --corpus-dir /tmp/run_qwen2b --policy policy_llm_Qwen3.5-2B-GGUF.json \
  --limit 15 --verbose --timeout 300 --log-dir /tmp/run_qwen2b/smoke
```

Check the header before trusting anything:

- `Privacy route target: 'Qwen3.5-2B-GGUF'` and **no** `WARNING: policy privacy
  route disagrees with corpus ground truth` banner. If the banner appears, the
  corpus and policy disagree on the local model name — redo step 1.
- `LLM fallbacks (bad JSON/name) : 0`. Non-zero means the router model isn't
  emitting a candidate name the server recognizes.
- `errors (HTTP/parse) : 0`. HTTP errors on the cloud side usually mean the
  Fireworks key isn't visible to `lemond`.
- Leaked cases show `actual=fireworks.kimi-k2p6` (a real cloud round-trip, so
  `model=n/a` in the timing suffix is expected on those lines).

### 3. The real run

500-case snapshot (~40 min at ~5 s/case for the 2B):

```bash
python test/eval/pii_routing_eval.py \
  --corpus-dir /tmp/run_qwen2b --policy policy_llm_Qwen3.5-2B-GGUF.json \
  --limit 500 --timeout 300 --progress-every 50 \
  --log-dir test/conformance/routing/1/l2_pii_nemotron_20k/runs
```

Full 20k (drop `--limit`; budget ~16 h for the 2B, ~12 h for the 0.8B, more for
the 9B). Run router models **sequentially**, never in parallel — they share the
GPU and the timing column would be meaningless.

Cases are pre-shuffled, so `--limit N` is a random-but-reproducible prefix and
every model sees the same N documents.

Output lands in `--log-dir` as `policy_llm_<model>_<timestamp>.log` plus a
`.json` summary with per-case timing samples. FAIL lines are written live;
PASS lines only with `--verbose`. A heartbeat with cases/s prints every
`--progress-every` cases.

If the run is interrupted (machine restart), don't start over:

```bash
python test/eval/pii_routing_eval.py ... --resume-from-log <partial .log>
```

replays the answered cases into a fresh log without re-querying.

### 4. Read the result

At the end of the log:

```
  PII -> cloud   (LEAK, FN error) :  N / 500      <- leak count
  Leak rate (FN / sensitive)     : x.x%
  E2E per case  avg / median / max
  Routing (router.model) avg/median
```

Quote leak rate as `x.x% (N / total)` and the **median** e2e per case. The
routing/model split is an estimate (see the docstring at the top of
`pii_routing_eval.py`); e2e is exact.

To see *why* the router leaked, every FAIL line carries the model's own
`rationale='...'`. Three patterns show up; grep for them before writing up a
number:

- rationale says "no PII" — a genuine miss;
- rationale names the PII and still picks the cloud — the model detected it and
  chose wrong;
- rationale says "use <local model>" but the decision field says cloud —
  self-contradictory output.

Per-category breakdown of the leaks:

```bash
python test/eval/pii_category_recall.py --corpus-dir /tmp/run_qwen2b <log>
```

### 5. Repeat for the other sizes

```bash
python test/eval/prepare_llm_router_run.py --local-model Qwen3.5-0.8B-GGUF --out-dir /tmp/run_qwen0.8b
python test/eval/prepare_llm_router_run.py --local-model Qwen3.5-9B-GGUF   --out-dir /tmp/run_qwen9b
```

then steps 2–4 with the matching `--corpus-dir` / `--policy`. For the NPU row
use `--local-model qwen3.5-0.8b-FLM` (needs the FastFlowLM backend installed
and the model pulled).

## Adding a benign arm (over-route rate)

Nemotron-PII is pure-positive, so the runs above report over-route as `0 / 0`
and a router that sends *everything* local scores a perfect 0% leak rate.
The 0.8B does exactly that with the current prompt: 0 / 500 leaks on the PII
arm, then 500 / 500 benign prompts also routed local, with rationales that say
"no PII detected; default to Qwen3.5-0.8B-GGUF". The 2B on the same 1,000
cases: 20.6% leak / 76.6% over-route, and 70% of its leaks *name the PII* and
still pick the cloud ("the powerful cloud model for sensitive data handling").
Quote a leak rate only next to an over-route rate.

`test/eval/build_benign_corpus.py` builds the missing negative arm from
human-written prompts (`HuggingFaceH4/no_robots`, `databricks-dolly-15k`),
filtered against the *prompt's* definition of PII (dates, cities, companies,
occupations and URLs count) by regex + OpenMed privacy-filter-v2 + mmBERT.
Mix it with the PII arm from step 1 so one run yields both rates:

```bash
python test/eval/build_benign_corpus.py --output-dir /tmp/run_qwen0.8b_mixed   --n-benign 500 --mix-pii-corpus /tmp/run_qwen0.8b/cases.jsonl --n-pii 500   --privacy-model Qwen3.5-0.8B-GGUF --cloud-model fireworks.kimi-k2p6
cp /tmp/run_qwen0.8b/policy_llm_Qwen3.5-0.8B-GGUF.json /tmp/run_qwen0.8b_mixed/
python test/eval/pii_routing_eval.py --corpus-dir /tmp/run_qwen0.8b_mixed   --policy policy_llm_Qwen3.5-0.8B-GGUF.json --verbose --timeout 300   --log-dir /tmp/run_qwen0.8b_mixed/runs
```

Needs `pip install datasets transformers torch onnxruntime` and ~10 min of
CPU for the detector pass. The benign prompts are short (median ~140 chars)
against document-length PII cases; a router keying off length alone would
look good here, which is what a domain-matched Tier-B arm would catch.

## Things that have gone wrong before

- **Log dialects.** In this eval's logs `[FAIL][TP]` means *leaked to cloud*;
  there are no `[FN]` lines. Read the `PASS`/`FAIL` verdict, not the bracket
  after it. (The NER evals use the opposite convention.)
- **Stale corpus target.** The 40-case `policy_llm_flm_20260911-003523.log`
  scored 100% FAIL because the corpus still said `Qwen3.5-0.8B-GGUF` while the
  policy routed to `qwen3.5-0.8b-FLM`. Step 1 exists because of this.
- **9B as cloud stand-in.** See point 3 at the top; the partial logs are in
  `l2_pii_nemotron_20k/runs/aborted_9b_standin/` for reference only.
- **Old prompt.** The 2,500-case runs from August (`l2_pii_nemotron/runs/policy_llm_*`)
  used a shorter prompt that listed only a handful of categories. Numbers from
  those logs are not comparable to runs with the current prompt.
