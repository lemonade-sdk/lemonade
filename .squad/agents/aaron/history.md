# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** Backend Integrator — wrapped servers, model registry, recipes

## Core Context

Owns 7 backend integrations (llama.cpp, FastFlowLM, RyzenAI, vLLM, whisper.cpp,
stable-diffusion.cpp, Kokoro) plus the recipe & version pin system.

Backends are subprocess-based — never in-process. NPU exclusivity is critical.

## Learnings

### 2026-06-25

**MCP in GUI3 — Phase A PR #2418 open; Phase B design on #2404.** Mattingly built read-only MCP dashboard (`McpPanel.tsx`, ~175 LOC). Phase B (GUI3 as external MCP client host) design posted by Lovell. May route Phase B implementation to Aaron or Haise post-approval.
