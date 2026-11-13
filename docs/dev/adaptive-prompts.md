# Adaptive Prompts (experimental)

Adaptive Prompts is an **opt-in server-side** instruction selector for
`POST /v1/chat/completions`. It does not change normal requests, server
model registration, or third-party agents by default. When opted in, it
preserves the caller's system-prompt text but appends selected guidance.
It works for both streamed and non-streamed completions. The Lemonade-only
`adaptive_prompt` field is removed before forwarding to the backend.

This is a controllable experiment, **not evidence that prompting improves
accuracy**. Compare the same task/model with and without it. GUI3 offers a
local opt-in under **Settings > Chat > Adaptive prompts**; other clients are unchanged.

## Selectors

- `"adaptive_prompt": true` — deterministic keyword selection, no extra inference.
- `"adaptive_prompt": {"mode":"rules", "trace":true}` — same, plus a response
  header `X-Lemonade-Adaptive-Modules` listing the selected IDs or `none`.
- `"adaptive_prompt": {"mode":"llm", "trace":true}` — one extra, non-streaming
  completion on the **selected target chat model** to choose from a shortlist
  of local module IDs. Model-generated instructions are never accepted.
- `"adaptive_prompt": {"mode":"llm", "decision_model":"d3-lite-FLM"}` — use
  a **downloaded** decision model. The server auto-loads a selected helper.
  A chat helper makes a short Chat Completion. A zero-shot classifier such as
  `d3-lite-FLM` scores the trusted module labels via `Router::classify` (separate
  role and skill requests), without generating text. Fixed-label classifiers
  cannot evaluate arbitrary instruction modules.
- `"adaptive_prompt": {"mode":"manual", "ids":["reviewer","factual-precision"]}`
  — explicitly select known, compatible IDs without any routing inference.
- `"adaptive_prompt": {"mode":"rules", "previous_ids":["creative-writing"], "trace":true}`
  — carry the IDs returned by the previous turn's trace header in the **same conversation**.
  The server merges them with the current selection and places the resulting compact guidance
  in the **initial system message**, not at the end of the conversation. Repeated IDs appear
  only once. For a different conversation or model, omit `previous_ids`.
- Omit `adaptive_prompt` or set it to `false` to leave the request untouched.

Model-based selection is opt-in because a decision incurs additional work.
A classifier on NPU can avoid generating extra text on the main Chat GPU.
The helper is loaded before the requested Chat model, so a different LLM helper
may cause an expensive model swap when only one LLM slot is allowed.
Only trusted module IDs can be selected; the classifier cannot write prompts.
If the decision call fails or returns invalid JSON/IDs, the server falls back
to conservative keyword selection. An **empty valid selection** does not trigger
fallback. Streaming remains supported for the *main* request; the optional
selector call itself is not streamed.

The server stays stateless: clients choose whether to carry the trace IDs on the
next request. GUI3 carries them for subsequent messages in the same open chat
view, resets on a model change or when Adaptive Prompts is disabled, and freezes
them during tool-call follow-ups. It does not globally mix selections from unrelated
chats; reloading the page currently clears this temporary state.
Manual selection always replaces the automatic selection instead of inheriting
previous IDs. When a new role is selected, the previous role and its carried skills are dropped.

Omni collections support `rules` and `manual`; `llm` is rejected with HTTP 400
rather than silently behaving as `rules`. Router collections select a backend
model before applying these instructions. The `/v1/responses` endpoint is not
changed. Other clients (including Pi and OpenCode) are unaffected unless they
explicitly send this option.

## Example

```bash
curl http://localhost:13305/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"Qwen3-0.6B-GGUF",
       "adaptive_prompt":{"mode":"rules","trace":true},
       "messages":[{"role":"user","content":"Review this Python patch for bugs"}],
       "stream":false}'
```

## GUI3 opt-in and inspection

In **Settings > Chat**, enable Adaptive prompts and select Rules, Decision model,
or Manual. Click **Save settings**. The option is browser-local and applies only
to Lemonade Chat, never to external clients. The server makes the selected IDs
available in `X-Lemonade-Adaptive-Modules` when `trace: true`; GUI3 shows the
last selection in Settings. The library is searchable, with full prompt text
available under Show instructions. Downloaded chat models and classifiers that
support dynamic labels are offered as decision helpers. Helpers load on demand.
The server log reports `decision_backend=chat|classify`, `helper_autoloaded=yes|no`,
and the selected IDs.

The catalog is also available at `GET /v1/adaptive-prompts` (read-only). The
built-in starting library ships with 213 distinct modules (30 roles, 183 skills).
No claim of model-specific prompt optimization is made.

## Custom catalog

Set `LEMONADE_ADAPTIVE_PROMPTS_FILE=/absolute/path/prompts.json` **before
starting** `lemond`. The file replaces the built-in catalog. A complete editable starting copy
is shipped as `src/cpp/resources/adaptive_prompts.example.json`. It is loaded
on the first enabled request and cached for the lifetime of the process;
restart to apply changes. A missing, oversized or invalid file is a
configuration error; Lemonade does not silently substitute another catalog.

```json
{
  "version": 1,
  "modules": [
    {
      "id": "reviewer",
      "type": "role",
      "description": "Inspect existing code for bugs, correctness and regressions",
      "examples": ["Review this pull request", "Find the bug in this patch"],
      "keywords": ["review this", "check this patch", "code review"],
      "priority": 10,
      "prompt": "Find actionable correctness defects. Do not rewrite unless requested."
    },
    {
      "id": "factual-precision",
      "type": "skill",
      "group": "factuality",
      "description": "Handle precise numbers and factual statements carefully",
      "keywords": ["how many", "wie hoch", "what year"],
      "prompt": "Differentiate verified measurements from estimates."
    }
  ]
}
```

- `role` is exclusive: at most one role plus two skills are selected.
- `group` (optional) makes related modules mutually exclusive, including
  across skills. `excludes` can forbid a pair of existing module IDs.
- Compatibility is deliberately sparse rather than a 213-by-213 matrix:
  explicit `group` or `excludes` means **incompatible**; otherwise two modules
  are permitted together (**neutral or complementary**). A newly requested
  skill takes precedence over conflicting or excess old skills. For example,
  `concise_answer` and `detailed_explanation` use the `answer-detail` group;
  `creative-writing` and `story_characterization` are compatible.
- `description` and optional `examples` support candidate retrieval for
  `llm` mode. `keywords` exclusively activate a module in `rules` mode.
- `priority` only breaks selection-score ties. It does not override constraints.
- Module IDs must be lowercase ASCII (`[a-z0-9_-]`). The catalog is bounded to
  2,048 entries / 4 MiB; each prompt is limited to 4 KiB; selected text to
  8 KiB. The custom catalog is trusted **server-local** configuration.

In `llm` mode, catalogs of at most 24 entries are presented directly. Larger
catalogs use lexical retrieval across `keywords`, `description`, and
`examples`, then offer up to 24 candidates (up to 8 roles and 16 skills).
A small number of general modules are included alongside lexical matches,
so model-based selection does not silently skip the helper. This **is not semantic
retrieval**: specialized modules can still be missed. For a classifier, roles
and skills are scored in separate zero-shot calls. The selection thresholds
(0.04 absolute and 0.01 above the explicit no-match label) are experimental,
not model-calibrated. When the classifier selects none for a role or skill group
but a candidate has an exact catalog keyword match, the selector retains that
explicit match as a fallback. This can misinterpret negated or quoted text;
manual mode remains available for controlled comparisons. For large libraries, provide informative keywords and
examples. An embedding router could later improve recall.

The selector extracts text from the most recent `user` message and ignores
text in fenced code blocks. For model-based selection, a preceding user request may be included as
limited context for short follow-ups (but does not override the current task). Embedded images and audio are left untouched.
The prompt composer preserves caller instructions, combines only trusted
library text, and avoids appending the same guidance repeatedly to a request.
An empty selection inserts no instructions. Module order is canonicalized
(role first, then ID), improving prompt-prefix stability when a decision model
returns the same set in a different order. `adaptive_prompt` is never passed
to the model backend. A trace exposes only module IDs, never full prompts.

## Known limitations

- Chinese (Simplified) keywords are supplied for common roles and skills. Han
  keywords match inside continuous Chinese text (without requiring spaces).
  Rules mode remains keyword-based, so this is not full multilingual coverage.
  LLM/classifier modes evaluate the user text against candidate descriptions,
  but their candidate shortlist is still lexical and can omit a specialist.
  Support for other languages and uncommon Chinese tasks is not guaranteed;
  prefer manual selection for controlled comparisons.

- Custom prompts are **not** a guarantee of factual correctness; evaluate both
  wrong answers fixed and right answers damaged.
- Model-based selection defaults to the target Chat model. Downloaded helpers
  load on demand; a second chat LLM can evict the first if LLM residency is
  limited. The selection thresholds are not calibrated for every classifier.
- The lexical shortlist may miss a relevant module in a large catalog, and
  may misinterpret ambiguous instructions. Manual mode supports oracle tests.
- A changed system prompt near the start of the conversation can invalidate
  some backends' prompt-prefix/KV caches. Benchmark long chats.
- Only `chat/completions` participates; there is no embeddings model or automatic
  tool orchestration. GET `/v1/adaptive-prompts` exposes the current read-only catalog
  (module IDs, descriptions, keywords and actual instructions) for the GUI.
- Built-in modules are a curated starting set, **not** performance-validated
  experts. They can be replaced by a custom JSON catalog.
- These are trusted instructions. Do not load module catalogs from arbitrary
  user messages or third-party URLs.
