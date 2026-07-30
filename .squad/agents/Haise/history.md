# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** QA / Integration — Python tests against live server

## Core Context

Owns Python integration tests in `test/`. Tests run against a live `lemond` server,
auto-discovered from the build dir. Black v26.1.0 formatting enforced in CI.

## Learnings

### 2026-06-25

**MCP in GUI3 — Phase A PR #2418 open; Phase B design on #2404.** Mattingly built read-only MCP dashboard (`McpPanel.tsx`, ~175 LOC). Phase B (GUI3 as external MCP client host) design posted by Lovell. May route Phase B implementation to Aaron or Haise post-approval.

### 2026-07-30T15:52:57.950Z

**MCP Parity Contract Approved.** Lovell published detailed MCP server specification as approved C++ server exception. Ten-tool Streamable HTTP interface for model management + inference. See .squad/decisions/inbox/lovell-mcp-parity-contract.md. Unblocks GUI3 MCP client frontend work.
