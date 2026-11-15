# Smart Thinking: experimental working roles

This is an opt-in **14-mode** alternative to micro-prompt routing: `default` adds zero
instructions, and 13 roles specify task methods rather than personalities. No 213-prompt
catalog or additional answering/review rounds are included.

The public `/api/v1/chat/completions` endpoint remains unchanged when `smart_roles` is
absent. Set `smart_roles: {"mode":"auto","decision_model":"d3-lite-FLM"}` to
classify the current user instruction once. A compatible downloaded zero-shot classifier
is recommended; a chat LLM is also supported. Omitting `decision_model` uses the chat
model for an extra short decision request. A missing/failed decision produces `default`
without aborting normal inference. For manually pinned modes use
`smart_roles: {"mode":"manual","role":"code_review"}`.

The response headers `X-Lemonade-Smart-Role` and `X-Lemonade-Smart-State` expose the
**effective applied** method, not merely the candidate. They are available on SSE and
non-streaming responses, with CORS exposure for GUI3. GUI3 carries the small bounded
state only within its conversation, and freezes a selected role during tool follow-ups.
Refreshing the browser resets the transition evidence; historical message labels remain
attached to previously completed turns. No server-global session table is created.

Settings > Chat > Smart Thinking enables the experiment (off by default), selects a
helper, and optionally pins a role. Model-level reasoning (the separate thinking
on/off switch) is not changed.

This implementation is **experimental, not quality-validated**. Before enabling by
default, run A/B evaluations against raw inference for multiple languages and model sizes;
measure correct task switching, severe wrong switches, false positives, accuracy,
response latency and extra prompt tokens. The role prompts and transition thresholds are
prototypes and must be tuned against these results before production rollout.

## Router diagnosis and regression examples

Two decisions are distinct: the selected **candidate** and the **effective**
working role after applying context/hysteresis. When enabled, the server logs
`Smart roles candidate: ...` (best and second-best relative scores, task hints)
followed by `Smart roles: effective=...`. Neither log includes the user input.
These scores are not calibrated probabilities. Do **not** judge classifier quality
only by the badge, which correctly shows the effective role.

The rubric is based on the requested **action/output**, not on domain keywords:

| Request intent (paraphrase in DE/EN) | Appropriate mode |
|---|---|
| Help me get a garden hut built; where should I start? | `plan` |
| Help me prepare a feature PR; what are the steps? | `plan` |
| Draft the PR description for this implementation. | `write` |
| Implement the feature and add tests. | `code` |
| Critically review the existing PR's changes. | `code_review` |
| Which material should I choose for the hut? | `advise` |
| Explain why roof drainage works that way. | `explain` |
| Hi there, how's it going? | `default` |

These are **expected behaviors for offline evaluation, not model-verified
results**. A short goal statement without an explicit request can be genuinely
ambiguous. Test surrounding chat histories, weak/near-tied predictions, abrupt
switches, German paraphrases, and negative neighbors before treating any
zero-shot helper as reliably calibrated. A strong first-task activation is now
possible without accumulated evidence; far implicit switches retain the old
hysteresis guard.

## Action-aware selector (experimental v2)

The classifier's previous 14-label menu included long descriptions with
negative examples and the transition controller normalised only the top two
scores. This made `write` competitive even when the user asked to **make a PR**
(a planning goal rather than a request for a finished PR description). It could
also leave `write` active after a high-confidence `code` candidate.

The current selector distinguishes three cases **without adding an inference
pass**:

1. **Narrow explicit deliverables:** A direct request to implement code, inspect
   an existing PR, draft a document, etc. may be resolved from the current
   *user instruction* alone. The server logs `source=explicit_action` and skips
   the classifier. Cues are bounded by instruction-voice and quote checks, not
   domain words such as `GUI`, `Lemonade`, `garden`, `PR`, `code` or `story` alone.
2. **Recognizable but non-explicit goals:** A small lexical/action shortlist
   offers positive, concise label descriptions, always including `default`.
   For example, "make a PR" offers `plan` and related workflows, not `write`
   merely because `PR` appears. The decision model remains authoritative
   among the offered labels. No individual micro-prompt is selected or stacked.
3. **Unknown or genuinely mixed instructions:** All fourteen modes are offered.
   An unknown language never silently excludes a specialist. A previous request
   is passed to the classifier only for short referential follow-ups; the
   current instruction always appears last. A confident independent new task can
   switch immediately, while vague continuations still accumulate evidence.

Classifier scores are now normalized across the *entire offered menu* (up to
fourteen labels), not just the best two. These are **relative scores, not
calibrated probabilities**. Routing telemetry reports the `source`, `menu_size`,
raw candidate IDs and relative scores, separately from the effective role.
Only the effective role appears on the reply badge. The UI and API contract
are otherwise unchanged.

Because the lexical shortlist changes the labels that the classifier sees,
lexical agreement and model choice are **not statistically independent**.
The extra low-score activation path is deliberately restricted to clear new
tasks with a decisive lead. Thresholds still require actual model calibration.

The action vocabulary is a small, intentionally conservative filter, **not a
classifier trained on 14 roles** and not a replacement for a real D3 evaluation.
High-confidence direct actions can still produce false positives on unusual
nested quotations or mixed requests. Unit tests cover positive/negative
requests, multilingual examples, full-score normalization, independent tasks,
and transient writing subtasks. They are not evidence of real model accuracy.

Build and run the existing controller test target (`test_smart_roles`) after the
patch. Then compare an NPU classifier with a chat-LLM helper on a frozen set of
independent turns *and* multi-turn conversations. Measure the candidate vs.
effective role, missed and severe false switches, and switching latency. Do not
make Auto the production default before real inference validation.
