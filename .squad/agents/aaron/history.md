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

(Append as work progresses.)
