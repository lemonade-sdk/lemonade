# MCP Parity Third Rejection Retrospective

**Recorded:** 2026-07-30T10:48:46.811-06:00
**Facilitator:** Lovell
**Result:** Rejected
**Scope:** Facts and acceptance criteria only. No source, test, or documentation files were changed.

## Facts

1. The load preflight checks `SystemInfo::get_supported_backends(info.recipe)` and then checks only `supported.backends.front()`. The Router separately resolves effective recipe options from model and server configuration before loading the backend. Therefore an installed fallback can satisfy the preflight while the configured or model-selected backend that Router will use is unavailable.

2. `safe_public_scalar()` redacts strings containing a URL scheme, but it does not reject ordinary relative paths or bare host names. `safe_public_text()` covers some absolute and dot-relative paths, but not every ordinary relative path or bare URL form. Several allowlisted diagnostic fields use the scalar path, so the protection is not uniform across nested diagnostic payloads.

3. The diagnostic test adds local assertions for handcrafted values such as `models/foo.gguf` and `github.com`, then applies that detector to naturally returned MCP payloads. It does not force the live MCP serializer to produce those values and assert the serializer's redacted or omitted output. A local detector can therefore pass while the MCP output behavior remains incorrect.

4. The documentation says that diagnostic responses never expose paths, path-bearing values, backend URLs, credentials, processes, and administrative controls, and describes recursive redaction. Those promises are broader than the implemented scalar and text checks. The documentation also must describe only fields the serializers actually return.

## Root Cause

### Backend selection

The load guard and the Router do not use one shared backend-selection result. The guard treats the first supported backend as the required backend, while the Router derives the effective backend from configuration and model options. This allows a fallback candidate to mask an unavailable backend that the load operation can actually select.

### Diagnostic safety

Diagnostic safety is split between two string filters with different coverage. The implementation does not apply one complete path-and-URL rule to every string value at every nesting level, including values retained by explicit public allowlists.

### Test evidence

The tests validate the test helper's ability to recognize unsafe strings, not the complete observable behavior of the MCP serializer for controlled unsafe input. The live response checks do not establish that the serializer itself redacts ordinary relative paths or bare URLs.

### Documentation accuracy

The documentation was written as a stronger security contract than the implementation and tests establish. It promises universal recursive redaction instead of documenting the exact fields and value transformations that are actually implemented and verified.

## Exact Acceptance Criteria

### 1. Backend validation

- The MCP load preflight must resolve backend selection using the same effective model and server configuration that `Router::load_model` uses.
- Every configured or model-selected backend that this load path can use must be validated before `Router::load_model` is called. Checking only the first fallback returned by `SystemInfo::get_supported_backends` is not sufficient.
- A missing or unavailable selected backend must return an MCP tool error before backend loading begins.
- The rejected request must not invoke backend installation, executable download, model download, fallback substitution, or configuration mutation.
- A test must configure an unavailable selected backend while an otherwise supported fallback is available, call `lemonade_load_model` through `/mcp`, and verify the error, unchanged backend status, unchanged model download state, and unchanged loaded state.
- A test must cover the model-level selection path as well as the server-level configured selection path when both are supported by the Router.

### 2. Diagnostic path and URL protection

- One complete safety rule must be applied to every string value emitted by `lemonade_get_model_info`, `lemonade_get_server_info`, and `lemonade_list_backends`, recursively through every object and array.
- The rule must reject or replace scheme URLs, bare URLs, host names, absolute paths, Windows paths, dot-relative paths, and ordinary relative paths such as `models/foo.gguf` and `models\foo.gguf`.
- The rule must cover every allowlisted string field, not only backend messages.
- No original path-like or URL-like value may remain in any nested MCP text block or serialized diagnostic object.

### 3. Test behavior

- Tests must call the actual `/mcp` endpoint and parse the returned MCP content blocks for each diagnostic tool.
- Tests must arrange controlled serializer input or server state that produces unsafe relative-path and bare-URL values, then assert the actual MCP response contains the required redaction or omission.
- A helper-only assertion against a handcrafted Python dictionary does not satisfy this criterion.
- Recursive checks must cover objects, arrays, all diagnostic tools, all allowlisted fields, forbidden field names, credentials, process identifiers, controls, paths, and URLs.
- The lifecycle test must prove the selected-backend preflight runs before any install or load side effect, rather than only checking an error message.

### 4. Documentation accuracy

- The documentation must list only fields the current serializers actually emit.
- It must not promise redaction or omission for a field, value class, or nesting behavior that the implementation and live MCP tests do not demonstrate.
- It must describe the exact behavior for relative paths, bare URLs, scheme URLs, credentials, process data, and administrative controls only after those behaviors are implemented and tested.
- The documented payload allowlists must match the MCP serializer output exactly, including optional fields and omitted fields.

## Lockout and Reviewer Assignments

- **C++ implementation:** Mattingly owns the revision. Liebergot, Aaron, and Haise are locked out because they are prior authors of the rejected C++ work.
- **Tests:** Swigert owns the revision. Haise, Liebergot, and Kranz are locked out because they are prior authors of the rejected test work.
- **Documentation:** Haise owns the revision. Liebergot, Kranz, and Aaron are locked out because they are prior authors of the rejected documentation work.
- Lovell remains the reviewer and must not modify the implementation, tests, or documentation while enforcing this review.
- Approval requires scoped diffs showing that each artifact was revised only by its assigned owner and that all criteria above are verified by targeted tests and live MCP output when the server is available.
