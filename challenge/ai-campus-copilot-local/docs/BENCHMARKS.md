# Benchmarks

## No published numbers

**This project publishes no benchmark results.**

The benchmark runner is built, tested and working, but I have not run a controlled
comparison across hardware or models, so there is nothing here I could state as a
measured fact. Any table of timings in this file would be invented, and inventing
performance data is exactly what the rest of this project is designed not to do.

Run the benchmark on your own machine to get numbers that mean something for your
hardware.

## What the runner measures

Five fixed questions, each driven through the complete pipeline:

| Metric | Source | Notes |
|--------|--------|-------|
| Question embedding | Wall clock in-app | Round trip to `POST /api/v1/embeddings` |
| Retrieval | Wall clock in-app | Pure local computation over stored vectors |
| Time to first token | First non-empty SSE delta | Falls back to `/api/v1/stats` `time_to_first_token` |
| Generation | Wall clock in-app | First byte of request to end of stream |
| Total | Wall clock in-app | Embedding + retrieval + generation |
| Tokens per second | `GET /api/v1/stats` | **Never estimated.** Blank when Lemonade does not report it |
| Output tokens | `GET /api/v1/stats` | Blank when not reported |
| Retrieved chunks | In-app | How many chunks cleared the threshold |
| Top score | In-app | Cosine similarity of the best-matching chunk |

Document processing timings — extraction, chunking, embedding — are recorded separately
when a document is uploaded and shown on the same page.

## The question set

```
1. What are the key dates mentioned in this document?
2. Who is eligible, and what are the eligibility conditions?
3. What actions must a student take, and by when?
4. Are any fees, stipends or amounts of money mentioned?
5. Who should a student contact for questions about this document?
```

Fixed so runs are comparable across machines and models. They target the kinds of facts
a campus notice actually contains rather than general knowledge.

## Running it

1. Process a document — `samples/semester-examination-notice.md` is a reasonable
   baseline.
2. Open **Performance**, select that document, press **Run benchmark**.
3. Export as JSON or CSV.

Each run records the chat model, the embedding model, the Lemonade version, and the OS
and processor strings Lemonade reports, so an exported file is self-describing.

## Reading the results honestly

- **A blank cell means "not measured", not "zero".** Tokens per second is blank whenever
  Lemonade did not report it for that request.
- **The first question of a run is usually slowest.** Lemonade loads the model on first
  use. Compare like with like, or discard the first result.
- **Averages skip failed questions,** but failures are recorded with their error message,
  so the export accounts for all five either way.
- **These are end-to-end application timings, not model benchmarks.** They include this
  app's own overhead — HTTP proxy hop, JSON parsing, IndexedDB reads. For pure model
  throughput, use Lemonade's own `lemonade bench` instead.
- **Retrieval time scales with chunk count,** and is pure local computation. On a
  typical single notice it is small compared to generation.

## What would make these numbers comparable

If you want to report results, state at minimum: chat model and quantisation, embedding
model, Lemonade version and backend/recipe, device (CPU/GPU/NPU), the document used, and
whether the first run was discarded. Without those, a tokens-per-second figure means
very little.
