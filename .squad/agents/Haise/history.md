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

### 2026-07-30T09:52:57.950-06:00

**MCP parity tests.** Extended `test/server_mcp.py` with ten-tool schema checks, safe diagnostic payload checks, structured lifecycle validation, explicit load/unload state assertions, and API-key coverage. Static syntax, whitespace, and Black validation pass; live execution waits for the C++ MCP parity build.

### 2026-07-30T09:17:15.715Z — MCP parity review: REJECTED, lockout issued

**Lovell review outcome:** CRITICAL REJECTION on three security defects.

**Test suite defects:**
- Assertions validate only structure/shape; missing recursive validations for forbidden field absence
- No assertions covering `action`, `release_url`, filesystem paths, credentials
- A tool can pass all tests while still leaking data if implementation isn't defensive

**Revision assignment:** **Liebergot (MCP Tests)** — after Aaron's C++ revision. Haise LOCKED OUT of MCP test code.

**Approval gates for Liebergot's revision:**
1. Recursive field redaction assertions for all five new tools
2. Explicit validations that `action`, `release_url`, checkpoint paths, credentials are NOT present
3. All existing five-tool tests remain green (no regressions)

**Timeline:** Aaron revises C++ (~1-2 days) → Liebergot takes over tests (~1 day) → live test execution deferred until server rebuilt.

## 2026-07-30T09:52:57.950-06:00 — Phase-1 preset-decoupling QA tests

- Added runtime coverage for canonical direct model tuning migration, collision archival, idempotence, malformed/unknown key preservation, direct save/reset behavior, named-value non-use, and API load/chat precedence.
- Added accessibility coverage for MCP selection persistence across model changes, picker reopen, reload, and explicit `mcp_enabled` precedence over legacy `use_tools`.
- Added MCP runtime assertions that `change_preset` is not exposed and cannot execute.
- Validation: MCP runtime, storage runtime, targeted A187d/A187e accessibility tests, syntax, and diff checks pass. Preset runtime is currently blocked by the concurrent implementation still using `s-default` rather than the approved legacy `s_default` migration key; app typecheck also reports unrelated concurrent Effective Settings/ModelDetailPanel mismatches.


## 2026-07-30T09:52:57.950-06:00 — MCP lifecycle no-install guard

Added an MCP-only preflight immediately before `Router::load_model` in `lemonade_load_model`. It uses `SystemInfo::get_supported_backends` to identify the selected backend and `SystemInfo::get_all_recipe_statuses` to require the exact `installed` state; missing, installable, update-required, update-available, and unknown states return a structured tool error directing callers to existing management APIs. Cloud recipes remain exempt because they do not use a local installable backend. The `lemonade-server-core` Release target compiled successfully; the full `lemond` link was blocked by the already-running `build\Release\lemond.exe` process.


### 2026-07-30T10:51:42.883-06:00 — MCP docs exact serializer alignment

Updated `docs/api/mcp.md` to match the MCP serializer's exact diagnostic field allowlists and field-specific scalar/text sanitizer behavior. Removed universal path, URL, credential, process, and administrative redaction claims; documented the actual `[redacted]` patterns, omitted fields, and retained bare URL/relative-path cases. Preserved `/mcp` API-key authentication, the five legacy and five parity tools, and the parity tools' no-install/download/pull/delete/media/admin scope.

Validation: `git diff --check` passed; targeted Markdown structure/content assertions passed; no existing `markdownlint` executable was available.
