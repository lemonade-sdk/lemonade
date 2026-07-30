# Haise MCP exact documentation decision

**Recorded:** 2026-07-30T10:51:42.883-06:00
**Owner:** Haise
**Scope:** `docs/api/mcp.md` only for the production artifact

## Decision

Document the diagnostic MCP payloads directly from `src/cpp/server/mcp_server.cpp` rather than promising generic recursive redaction. The documentation now lists the exact model, loaded-state, health, system-info, device, and backend fields; distinguishes `safe_public_scalar` from `safe_public_text`; states when values become `[redacted]` or are omitted; and explicitly says bare hosts, bare URLs, and ordinary forward-slash relative paths are not universally redacted.

The documentation continues to state route-level `/mcp` API-key authentication, the five legacy plus five parity tools, and that the five parity tools add no installation, backend download, model pull/download, delete, media, or administrative controls.

## Validation

`git diff --check` and targeted Markdown structure/content assertions passed. No existing Markdown linter executable was available.
