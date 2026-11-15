# Hybrid-Coder-Mode (experimental, llama.cpp)

Hybrid-Coder-Mode uses **one GGUF model and two isolated llama.cpp slots**. The model's
configured `ctx_size` is the **total** context budget: with `--parallel 2`, each role
normally receives half. Check `GET /v1/slots` for the actual `n_ctx` values.
The two roles share model weights, **not** their KV caches.

A long-lived orchestrator delegates bounded coding tasks to an isolated worker.
The worker can call the same client-provided tools as a normal chat. The client
executes those tool calls and posts its existing conversation history back to
Lemonade. A server-side message index then delivers only the *new* tool results
to the role that issued their tool-call IDs. After the worker finishes, Lemonade
passes its **short result only** to the orchestrator, erases the worker KV slot,
and starts the next task with a new empty worker transcript. Previous worker
transcripts in the client's outer history are never sent to either model role.

This is an **explicitly opted-in, server-managed** experiment on
`/v1/chat/completions`. Loading a model with `hybrid_coder_mode: true` only
prepares the two slots. Each Hybrid request must additionally set
`"hybrid_coder_mode": true` or the header `X-Lemonade-Hybrid: 1`.
An explicit body value `false` overrides the header. Other clients continue
using ordinary inference, even when the model has Hybrid support enabled.

## Enable

From the CLI:

```bash
lemonade load Qwen3.5-35B-A3B-GGUF --ctx-size 65536 --hybrid-coder-mode --save-options
lemonade launch pi --model Qwen3.5-35B-A3B-GGUF
# Pi currently remains in normal mode unless its request adapter opts in.
```

Or in the GUI: Models > select a llama.cpp chat model > Configuration > Load
settings > under Context size, check **Apply Hybrid-Coder-Mode** > Load/Reload.
Save to persist it across model loads. Disabling the checkbox and reloading
returns to normal one-slot behavior.

REST equivalent:

```bash
curl -fsS http://127.0.0.1:13305/v1/load \
  -H 'Content-Type: application/json' \
  -d '{"model_name":"Qwen3.5-35B-A3B-GGUF","ctx_size":65536,"hybrid_coder_mode":true}'

curl -fsS http://127.0.0.1:13305/v1/slots
```

For a 128K orchestrator plus a 128K worker, **only attempt**
`ctx_size=262144` if the model, backend and available memory support it. A
loaded context setting of 256K does **not** create two 256K windows. Model
metadata advertises the usable **per-role** context size rather than their sum.
Hybrid mode adds **exactly `--parallel 2`** to llama-server. An explicitly
conflicting slot count is rejected. No third slot or automatic KV snapshot
is introduced. The private role histories remain in Lemonade memory.

## Boundaries and protections

- **Other loaded models:** Embedding, classifier, image, and other backends may
  coexist with the hybrid model when Lemonade has sufficient memory and allows
  the relevant residency counts. The hybrid implementation targets **its own
  model** for slot inspection and erasure; it never selects the last-used
  backend. The public `GET /v1/slots` endpoint still targets the most-recently
  used backend, so it may not display the hybrid model after other activity.
  Use `GET /v1/health` to inspect all loaded model names and launch arguments.
  Unloading or replacing the Hybrid model invalidates its in-memory KV leases.
- **Logical sessions:** Pass `x_lemonade_hybrid_session: "client-stable-id"` in the
  request body or `X-Lemonade-Hybrid-Session: client-stable-id` as a header.
  Different Hybrid sessions preserve separate logical histories and may
  interleave on one model. Switching sessions resets the two physical KV slots,
  then replays the selected role history; it does not merge conversations.
  Without an explicit session ID, the initial user request and leading
  instructions provide a fallback identity, with strict history validation.
  At most eight logical sessions per loaded model are retained in memory.
  Only one Hybrid request **per model** runs at a time; Hybrid requests to
  different loaded models are independent. Ordinary inferences can reuse the
  same two slots, so they invalidate the private cache lease without changing
  the stored histories. The shared per-model gate can return a busy error for
  contention over 15 seconds. There is no durable restart recovery.
- **Client system prompts:** Leading OpenAI `system` and `developer` messages
  (including multiple system entries from Kilo Code) are kept in their original
  order and folded into one leading llama.cpp `system` message alongside the
  Hybrid-Coder orchestrator instructions. The external client history is not
  rewritten. A system/developer message *after* the first user message is
  rejected rather than silently reordered; clients must start a fresh chat.
- **Client history normalization:** Older assistant tool-call messages are
  echoed back differently by some OpenAI-compatible SDKs (e.g., null versus
  omitted content). Their text is not used as a source of new instructions:
  Lemonade already owns the authoritative original. Tool-call IDs, names and
  JSON argument values still have to match. Text-only content arrays and
  reserialized JSON function arguments compare by content, not presentation.
  Actual changes to user messages or tool results still fail closed, with the
  role and differing field identified in the error; no auto-merge of rewinds.
- **External history:** The server compares existing messages and validates
  tool-call IDs strictly. Kilo's known completed-tool pruning marker is allowed.
  With an **explicit stable session ID**, a summary/rebase is accepted only if
  a unique, unchanged retained suffix matches. A changed tool ID, changed
  instruction or unresolved tool round still fails closed. The summary never
  replaces private Orchestrator/Worker messages. A replay of an already accepted
  response is not silently re-executed.
- **Tool permissions:** Only tools sent by the client are exposed to the
  worker. `hybrid_delegate_task` is reserved for internal orchestration; it
  is not executed by Pi. Steering messages arriving during a Worker task are
  queued and delivered to the Orchestrator after the private Worker handoff.
- **Loop safety:** Client-driven Worker turns and cross-request delegations are
  not capped by arbitrary constants; Pi can stop or cancel its own tool loop.
  A per-HTTP-request safety bound of 20 internal model calls still prevents a
  broken internal delegation/handoff loop from blocking one API request forever.
  A worker result longer than 8 KiB is refused instead of silently truncated.
- **Persistence:** Orchestrator messages and the outer-message mapping currently
  live in server memory only. There is no task archive, crash/restart recovery,
  per-user access control, or durable recovery after process restart. Internal role
  context rollover is supported (see below); an explicit stable session ID
  permits only a verified, unique-suffix external history rebase.
- **Streaming:** `stream: true` uses llama.cpp's native SSE path for externally
  visible answers. Orchestrator `content` deltas are sent as they are generated.
  Worker tool-call fragments are held only until the full generation has been
  validated, because the same generation may instead select the private
  `hybrid_worker_done` tool. Validated external tool calls are emitted as
  complete, ordered SSE deltas; internal tools are **never** exposed.
  SSE comment heartbeats may be forwarded during the hidden Worker generation
  to keep clients connected; these comments contain no model output.
  This safety buffering adds some delay to the display of tool arguments, not
  another model generation or a buffered user-visible text response. A usage-only
  final chunk is returned when `stream_options.include_usage` is true. Client
  disconnects or malformed backend streams invalidate any partially delivered
  answer; the server does not send a success `[DONE]` after such errors.
- **Internal routing:** The orchestrator retains its separate, short
  **non-streaming** delegate/respond routing call; externally visible replies
  still use the normal response path with no internal delegation tools. The
  worker now chooses a client tool, the private `hybrid_worker_done` tool, or
  a normal text completion in **one** model generation per worker turn instead of a routing call
  followed by another model call. This preserves the same two role-specific
  histories and tool-result-ID checks, while avoiding one model generation and
  a changed tool schema on the same worker KV slot. The internal completion
  summary is not sent to Pi. A nonempty Worker text completion with normal
  `finish_reason: stop` is likewise a private handoff, never a visible Pi
  answer. Hybrid preserves the client's `tool_choice` (normally `auto`) instead
  of forcing `required` for every Worker continuation, which could bias the
  model toward unnecessary external calls. A mixed `worker_done` + external-tool response
  is rejected before tool fragments are forwarded. Orchestrator routing
  still disables reasoning to preserve its short decision budget; incomplete
  decisions fall back to a direct responder as before.
  `x_lemonade_hybrid.worker_single_pass` marks this behavior. Its actual effect
  on latency and cache-hit rate must be measured against the prior build.
- **Without client tools:** Hybrid uses the orchestrator for normal responses
  and does not delegate to a worker that cannot read, edit or test code. This
  is compatible with prompt-only evaluations but does not exercise worker resets
  or demonstrate context savings. To benchmark delegation, provide actual
  executable tools and run their tool-call loop.
- **Output limits:** Nonempty text replies ending in `finish_reason: "length"`
  are forwarded as *partial* completions, just like non-hybrid llama.cpp.
  A reply containing only hidden reasoning (or no text/tool calls at all) now
  produces `hybrid_output_incomplete` instead of a successful empty completion.
  This prevents Pi from reporting a normal empty final answer after a context
  exhaustion or generation-limit event. Incomplete tool-call arguments are
  also rejected with `hybrid_output_incomplete`.
  Increase the request's `max_tokens` or continue the conversation if a
  generated patch was cut short; this mode does not silently extend it.

## Long-running context safety (v10 experimental)

Hybrid uses the **actual reported per-slot** `n_ctx` from the llama.cpp
`/slots` API, not a hardcoded 32K/64K/128K role window. After successful role
inferences, Lemonade records the *rendered prompt-token usage* and the byte
footprint of the corresponding internal messages, plus the read-only `n_past`
occupancy already obtained for the backend's two slots at request entry. Before the next role inference,
it checks the observed prompt occupancy plus a conservative upper bound for
new tool results. It starts a checkpoint around 70% of slot capacity, leaving
room to write the checkpoint itself. This does not mean allocated KV is the
same as occupied KV, or that cached tokens are actually recomputed.

- **Worker:** When the active task approaches capacity **between completed
  client tool rounds**, a private, non-tool summary covers verified edits,
  tests, blockers, and outstanding work. Only after a complete valid summary
  is received is **slot 1** erased and rebuilt from the original delegated
  task plus the progress checkpoint. Code changes on disk are unchanged. The
  Worker can then continue the *same* delegated task without repeating all
  previous tool messages. External `tool_call_id`s and tool results remain in
  the unmodified visible client history.
- **Orchestrator:** On its own context pressure, the server checkpoints the
  current decisions and handoffs, then erases **slot 0**. The rebuilt prompt
  retains the original merged client `system`/`developer` instructions
  verbatim and retains the most recent user request (including any content
  parts) verbatim within a checkpoint-prefixed user message. Older internal
  exchanges are represented by the new summary, not replayed.
- **Fail closed:** Rollover is forbidden while client tool results remain
  pending, on an empty or incomplete checkpoint, or if an internal tool call
  appears in a checkpoint response. The role slot is **not** erased if the
  checkpoint fails. The caller receives an explicit incomplete/error response;
  it must not treat this as a successful assistant turn. Only an explicit,
  verified client history rebase changes the outer mapping; private role
  histories remain authoritative.
- **Remaining limit:** Summaries are model-generated and can omit details.
  They are historical data, not authority to override system instructions or
  tool permissions. Extremely large single user prompts, giant unexpected
  tool results, or backends without trustworthy prompt-token usage can still
  exhaust a slot before a safe checkpoint is possible. Those failures remain
  explicit rather than silently trimming arbitrary parts of the prompt.
  A client's *own* advertised context/compaction policy is separate; Pi/Kilo
  can retain a long outer transcript while Lemonade manages both role contexts.
  A client rebase requires an explicit stable session ID and unchanged suffix.

`x_lemonade_hybrid` reports `worker_context_rollovers_total`,
`orchestrator_context_rollovers_total`, and
`context_checkpoint_model_calls_total`. Checkpoints are counted among
`performance.model_calls` and usage totals, so their extra prefill/generation
costs are visible. `LEMONADE_HYBRID_METRICS=1` logs per-call token/cache data
and `LEMONADE_HYBRID_PROFILE=1` can independently sample `/slots` `n_past`
(in a separate diagnostic run to avoid distorting speed measurements).

The model-free `HybridContextPolicyTest` checks pressure decisions,
per-slot capacity use, and fail-closed checkpoint validation. An actual
Kilo/Pi continuation on the target GGUF is required to confirm the rollover
summaries preserve task quality and avoid new backend context-limit failures.

## Client-visible Orchestrator context

A Hybrid response has `x_lemonade_hybrid` metadata. In SSE it appears on the
**terminal choice chunk**, and again on the optional `include_usage` chunk.
Normal, non-opted-in Chat Completions never receive this extension.

```json
{
  "x_lemonade_hybrid": {
    "context_management": "server",
    "context": {
      "management": "server",
      "outer_transcript_is_model_context": false,
      "outer_transcript_limit_bytes": 33554432,
      "orchestrator": {
        "capacity_tokens": 65536,
        "last_prompt_tokens": 28000,
        "measurement": "last_completed_inference"
      },
      "worker": {
        "capacity_tokens": 65536,
        "last_prompt_tokens": null,
        "measurement": "unavailable"
      }
    }
  }
}
```

These numbers are **illustrative**. `capacity_tokens` is actual per-slot
`n_ctx` from llama.cpp, not `ctx_size` divided heuristically. `last_prompt_tokens`
is the most recently measured, fully rendered prompt for that role, including
cached input and tool schemas. It is `null` after a role reset or before its
first inference. It is **not** current KV `n_past`, a promise that the next
prompt will fit, or the total tokens spent in the request. The ordinary
OpenAI `usage` object intentionally still counts all internal model calls,
which is useful for accounting but is **not** the Orchestrator's occupancy.
The separate `outer_transcript_limit_bytes` (32 MiB in this implementation)
remains a hard request-size guard. A client must still handle that bound even
when it delegates internal role compaction to Lemonade.

A benchmarking client such as GAIA can opt in directly:

```bash
curl -sS http://127.0.0.1:13305/v1/chat/completions \
  -H 'Content-Type: application/json' \
  -d '{"model":"Qwen3.5-35B-A3B-GGUF", "hybrid_coder_mode":true,
       "x_lemonade_hybrid_session":"gaia-run-001",
       "messages":[{"role":"user","content":"Describe the change."}]}'
```

To test actual Orchestrator/Worker separation, GAIA must expose tools, execute
the returned tool calls, and return their results with unchanged `tool_call_id`s.
Without tools, only the Orchestrator answers and Worker recycling is untested.

A future Pi/Kilo integration can use `context_management: "server"` to avoid
compacting the entire visible tool transcript merely because request `usage`
is large. This requires an **explicit client change**; the current standard
OpenAI adapters do not automatically interpret this vendor extension. Clients
must not forward the Hybrid opt-in header on standalone summarization, title,
or other auxiliary requests. Manual `/compact` remains a client action and its
retained suffix must pass server validation.

## What is actually saved?

*Prompts to llama.cpp* include the orchestrator's own history or the current
worker's history, **never** the complete external Pi transcript after the
session begins. That is the actual context saving. Clearing the worker slot
removes its prompt cache; it does not release the allocated KV buffer's VRAM.

For large, single-shot coding prompts, **do not confuse input length with
available per-slot context**. For example, with `ctx_size=65536`, each role
has roughly 32768 tokens. An input using 38K prompt tokens cannot fit before
any output. To test a 38K-token input with a 4K completion and headroom, try
`ctx_size=131072` (64K per slot) if the model and available memory allow it;
check `/v1/slots` rather than assuming the requested size was retained.
No code can complete a request that does not fit the loaded slot's KV budget.
A normal `finish_reason: "length"` reports a partial completion, not a valid
SWE-bench patch by itself. `patch=0 bytes` also means the evaluation harness
may need real file/test tools, a larger output budget, or a correctly parsed
patch response.

The normal `usage` object aggregates prompt and generated tokens across
**all internal inference calls made for that external HTTP response**, including
the internal routing calls. For streamed responses the server requests upstream
usage (with a llama.cpp timings fallback) and emits the aggregate in the final
usage-only SSE chunk when requested. The `x_lemonade_hybrid` metadata is also
attached to the terminal choice chunk. These metrics differ from the last active
role's actual context occupancy and from uncached prefill cost.

When **every** internal llama.cpp call reports cached tokens, Lemonade now
aggregates them under `usage.prompt_tokens_details.cached_tokens` in the
response (and final usage-only SSE chunk), allowing Pi to distinguish cached
from uncached input. Unknown cache counts are **not** synthesized as zero.
`x_lemonade_hybrid.performance` reports per-HTTP-request internal `model_calls`,
`routing_calls`, `reply_calls`, `cache_samples`, `cached_tokens_observed`,
`prefill_token_samples`, `evaluated_prompt_tokens_observed`,
`prefill_time_samples`, `prefill_ms_observed`, and routing/reply wall time in ms.
The diagnostics now also include separate `orchestrator` and `worker` objects
with model-call counts, cache/prefill measurements, and wall time for each role.
These metrics include calls that do not result in a visible tool call; a
`*_samples` count smaller than `model_calls` means the corresponding total
is incomplete and must not be treated as a full measurement.

For **low-overhead per-inference measurements** (including actual backend
cached/evaluated token counters when supplied), start `lemond` with
`LEMONADE_HYBRID_METRICS=1`. Each `HybridMetrics` log line reports the role,
routing-vs-reply kind, prompt/output tokens, cached and evaluated tokens,
reported prefill milliseconds and internal call wall time. Unknown upstream
metrics are printed as `unknown`, not zero. Successful slot erases are logged
separately. No additional model call or `/slots` request is made in this mode.
Measure end-to-end latency with this flag on and off if log I/O is significant.

To inspect **actual occupied KV**, start `lemond` with
`LEMONADE_HYBRID_PROFILE=1`. This enables `HybridProfile` log entries around
each internal model call and worker-slot reset. The log includes whichever
per-slot metrics the installed llama.cpp reports (`n_past`, `n_ctx`, processed
prompt/cache tokens), without user content or tool arguments. **Profile mode
adds read-only `/slots` requests**, so benchmark speed with it disabled; run a
separate diagnostic pass to inspect occupancy and reset behavior. Note that
allocated `n_ctx` is a capacity, not the length of the active prompt.

The worker is instructed to favor a minimal patch and test representative
counterexamples before introducing a new parser or abstraction. The orchestrator
is instructed to relay relevant client/system constraints with each task without
pasting the entire original Pi prompt into each Worker context. This is advisory,
not a substitute for full prompt fidelity or independent patch-quality grading.

The worker is also instructed to perform relevant verification but to **report** a
missing dependency or incompatible interpreter once established, rather than
repeatedly constructing fake dependency stubs or substitute test harnesses. A
normal final text response is now accepted as a private completion if no tool
call is needed; the worker no longer has to invoke another tool just because
`tool_choice` was forced to `required` by Hybrid.
Actual test failures in code still need investigation. This is guidance, not
an unconditional time or tool-call limit, and cannot guarantee the model obeys.

Pi may still compact based on its own **outer** conversation length. This patch
does **not** fix Pi's compaction algorithm and does not claim that an inflated
outer transcript can be ignored by every client. Verify the interaction with
Pi on the exact installed version before considering a session protocol or
context accounting hook.

## Evaluation

The opt-in `test/server_hybrid_coder.py` reloads its selected model with
`LEMONADE_HYBRID_TEST_CTX_SIZE` (default: 65536 total). This may replace a
larger loaded context; choose a suitable size for the test and reload your
normal settings afterward. For example:

```bash
LEMONADE_HYBRID_TEST_MODEL=Qwen3.8-27B-GGUF \
LEMONADE_HYBRID_TEST_CTX_SIZE=131072 \
pytest -q test/server_hybrid_coder.py -s
```

Use a clean project revision and repeat the same coding task with and without
Hybrid-Coder-Mode. Compare final tests, end-to-end wall time, time to the **first content delta**
(not just the SSE role header), llama.cpp slot `n_prompt_tokens_processed` and
`n_prompt_tokens_cache`, maximum real prompt occupancy, and peak memory. Check
that several deltas arrive **during** generation rather than back-to-back after
inference ends. Verify ordinary text, external tool-call fragments, worker
handoffs, client disconnects, and the `stream_options.include_usage` terminal
chunk. Logs should be inspected to
ensure tool-call IDs and worker summaries land in the intended context.

The no-GPU `HybridWorkerPolicyTest` checks classification, undeclared tools,
missing/duplicate tool IDs, mixed internal/external tool calls and empty
visible completions. It does not replace a Pi SSE integration run with the
actual GGUF and the installed llama.cpp backend.

A successful prompt reduction is not by itself evidence of faster end-to-end
coding. Keep the opt-in disabled if quality, latency or recovery are worse.
