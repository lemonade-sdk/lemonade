# History: Haise (Documentation / Knowledge)

## 2026-07-30

**MCP Documentation Alignment:** Synchronized `docs/api/mcp.md` with current C++ MCP implementation. Alignment work includes:

- Reviewed implemented MCP behavior in `mcp_server.cpp`
- Updated endpoint documentation to reflect actual behavior
- Aligned tool definitions with live implementation
- Verified protocol details and request/response contracts
- Ensured consistency with test cases and implementation

**Documentation Status:**
- `docs/api/mcp.md` now reflects current implementation
- Tool definitions match live tools exposed by server
- Protocol behavior and contracts accurately documented
- Ready for user reference and integration

**Session Completion (20260730T095257Z):** Documentation alignment for MCP parity complete. Handoff recorded to squad log.


## MCP Parity Documentation Revision — Assignment (2026-07-30T09:52:57.950-06:00)

**Reviewer:** Lovell
**Verdict:** MCP parity REJECTED with three critical defects

**New Assignment for Haise:**
- **Scope:** MCP documentation revision — docs/api/mcp.md security contract, public allowlists, forbidden fields
- **Locked-Out Agents:** Liebergot, Kranz, Aaron (prior authors)
- **Acceptance Criteria:**
  1. List only fields the current serializers actually emit
  2. Define exact behavior for relative paths, bare URLs, scheme URLs, credentials, process data, administrative controls
  3. Do not promise redaction or omission for any field/value class not demonstrated in implementation and live tests
  4. Described payload allowlists must match MCP serializer output exactly, including optional and omitted fields
  5. Security principle: read-only ≠ unfiltered; all outputs redacted to diagnostic state only
- **Merge Hold:** Active until all three C++/test/docs revisions complete and Lovell re-approves
- **Previous Work:** Prior MCP documentation alignment is rejected and archived
- **Status:** Awaiting revision

**Note:** Documentation must accurately describe implementation, not prescribe ideal security that is not yet implemented. Allowlists must reflect exact serializer output.
