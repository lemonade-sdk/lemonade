# History: Swigert (QA / Test Infrastructure)

## MCP Parity Test Revision — Assignment (2026-07-30T09:52:57.950-06:00)

**Reviewer:** Lovell
**Verdict:** MCP parity REJECTED with three critical defects

**New Assignment for Swigert:**
- **Scope:** MCP test revision — diagnostic tool redaction assertions, live MCP endpoint validation
- **Locked-Out Agents:** Haise, Liebergot, Kranz (prior authors)
- **Acceptance Criteria:**
  1. Tests must call actual `/mcp` endpoint and parse returned MCP content blocks for each diagnostic tool
  2. Arrange controlled serializer input or server state producing unsafe relative-path and bare-URL values
  3. Assert actual MCP response contains required redaction or omission
  4. Recursive checks must cover objects, arrays, all diagnostic tools, all allowlisted fields, forbidden field names, credentials, process identifiers, controls, paths, URLs
  5. Lifecycle test must prove selected-backend preflight runs before any install/load side effect
  6. Tests must validate helper-recognized unsafe strings are also redacted in live MCP output (not just in test helpers)
- **Merge Hold:** Active until all three C++/test/docs revisions complete and Lovell re-approves
- **Previous Work:** Prior MCP test work is rejected and archived
- **Status:** Awaiting revision

**Note:** Live MCP endpoint testing required when server is available. Compile-only or diff-only results cannot close a diagnostic security review.


## 2026-07-30 — Final preset CSS cleanup

- Removed the unused starter-zone rule from `src/app/src/styles/styles.css`.
- Confirmed no `.zone--starters` or `.zone__starters` references remain under `src/app`; no focused style test existed.
- Validation: focused source scan and `git diff --check -- src/app/src/styles/styles.css` passed.
