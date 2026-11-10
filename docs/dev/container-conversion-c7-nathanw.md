# C7 Nathanw conversion stop

C7 inspected stack `d491c946fed2ab079f7387cf02a0eaabb51afce4` against integration base
`4f9bb9e3e768953ddc967c59a06710259a5eed60`. This engine was **not migrated**:
the remaining behavior below cannot be preserved by the current v2 contracts.
The assignment requires stopping an engine rather than dropping behavior.

Production definition lines added: **0**. No execution grant or owner digest,
active leaf, registration, model row, version pin, generated default/support
section, parity fixture or identity transition was added for this engine.
The source captures and CPU probes are preflight evidence, not migration parity.

## Pinned inventory

| Field | Stack source inventory, not production additions |
|---|---|
| Recipe / backend | `llamacpp` / `nathanw` |
| Version key | `llamacpp.nathanw` |
| Repository | `docker.io/kyuz0/amd-strix-halo-toolboxes` |
| Pin | `vulkan-radv-performance@sha256:41018261e61b7e32fb38e4985ba569d0837804e727dcc62689c2fb1d73ec8f15` |
| Registry rows with this recipe | 99 |
| Defaults | `{"args":"","backend":"auto","cpu_args":"","cpu_bin":"builtin","cuda_args":"","cuda_bin":"builtin","prefer_system":true,"rocm_args":"","rocm_bin":"builtin","vulkan_args":"","vulkan_bin":"builtin"}` |
| Pin validation | **Unvalidated. No registry access was performed.** |

Reproduce each quotation with `git show d491c946fed2ab079f7387cf02a0eaabb51afce4:<path>`.
Exact captures, including version/default files and all recipe model rows, are
in `.synagent/artifacts/container-v2/c7/source/nathanw/`.

## Missing new-identity legacy streaming contract

`src/cpp/server/backends/llamacpp/llamacpp_server.cpp:690` implements
`LlamaCppServer::forward_streaming_request`. It transforms only exact canonical
chat/responses paths, then unconditionally delegates the supplied endpoint to
`WrappedServer::forward_streaming_request`. The inherited implementation sends
`get_base_url() + endpoint`. A query-bearing endpoint therefore reaches HTTP.

This is a `llamacpp:nathanw` selector, not a standalone recipe. The stack's
`LlamaCppServer` executes both the container case and the inherited protocol
surface. C6 expresses the distinct device-validation API while retaining this
selector; it does not grant a second builtin identity legacy streaming.
The 99 model rows are shared llama recipe inventory, not 99 dedicated container
models. A conversion must preserve that distinction.

`src/cpp/server/backends/configured/backend_definition_execution.cpp:220-222`
permits `direct_stream_policy: "llamacpp_legacy"` only when the source is builtin,
its ID is `llamacpp`, and its recipe is `llamacpp`. The resource emitter in
`cmake/BackendDefinitionResources.cmake` fixes embedded source contracts at 1.
A second leaf cannot reuse `builtin:llamacpp@1` while preserving that source.

The compiled C7 probe uses a private synthetic owner and test admission only.
For ID `llamacpp-nathanw` it observes:

```text
unsupported_execution_feature /launch/direct_stream_policy
```

The same policy compiles under the protected `llamacpp` identity as a positive
control. Both permitted substitutes, `strict` and `bound_passthrough`, accept
canonical `/v1/completions` but reject `/v1/chat/completions?probe=1` with
`Streaming operation adapter is not bound`. The exact stack streaming method,
compiled from its captured body, forwards that query-bearing request to an
actual ephemeral loopback HTTP server and receives the expected SSE bytes.
The method body is byte-identical across the three pinned engine sources.
This focused source-body probe links integration shared transport; both the
pinned and integration base dispatchers concatenate the endpoint unchanged.
It is not a complete stack binary or a real container execution.

The named checks are `C7LegacyStreamIdentityRestriction:llamacpp-nathanw`,
`C7CanonicalStreamControl:strict`, `C7CanonicalStreamControl:bound_passthrough`,
`C7AlternativeLosesInheritedEndpoint:strict`,
`C7AlternativeLosesInheritedEndpoint:bound_passthrough`, and
`C7ExactStackStreamForwardsQueryEndpoint`. Logs are retained as
`.synagent/artifacts/container-v2/c7/probe-{compile,link,run}.log`.

A prerequisite must define and independently authorize the required legacy
endpoint behavior under new builtin identities, with its own disclosed stage.
Changing the existing identity, omitting this behavior, or treating a failed
compilation as parity would violate this assignment. Verbatim source logging
must also be carried through the v2 path before claiming full parity.

The older Gufo/C6b fixtures deliberately qualify bounded endpoint admission.
That narrower profile is not evidence of equality to this inherited public
C++ direct-stream method on all accepted endpoints. C7's explicit instruction
not to drop behavior is the stop criterion used here; no existing bounded
fixture or comparator was changed to manufacture broader parity.
