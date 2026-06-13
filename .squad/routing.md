# Work Routing

How to decide who handles what.

## Routing Table

| Work Type | Route To | Examples |
|-----------|----------|----------|
| Scope, architecture, cross-cutting decisions | Lovell | Framework choice, invariant enforcement, big-picture trade-offs |
| C++ server (`lemond`, router, APIs, WebSocket) | Liebergot | server.cpp, router.cpp, model_manager.cpp, anthropic/ollama API shims |
| Wrapped backends, model recipes, NPU/GPU paths | Aaron | llamacpp/flm/ryzenai/vllm/whisper/sd/kokoro_server.cpp, server_models.json, backend_versions.json |
| CMake, installers, CI, cross-platform packaging | Kranz | CMakeLists.txt, WiX, .deb/.rpm, macOS pkg, GitHub workflows |
| Python integration tests, validation scripts | Haise | test/server_*.py, test/validate_*.py, regression catches |
| UI / Frontend (Tauri app, web app, renderer) | Mattingly | src/app/, src/web-app/, React 19/TS, window.api shim, CSS |
| Code review | Lovell | Reviewer role with rejection authority |
| Session logging | Scribe | Automatic — never needs routing |
| Work queue / backlog monitoring | Ralph | Manual activation only ("Ralph, go") |

## Reviewer Authority

- **Lovell** holds reviewer authority for architectural and cross-cutting concerns.
- **Mattingly** reviews UI changes.
- On rejection, a DIFFERENT agent must produce the revision (strict lockout).

## Issue Routing

| Label | Action | Who |
|-------|--------|-----|
| `squad` | Triage: analyze issue, assign `squad:{member}` label | Lovell |
| `squad:{name}` | Pick up issue and complete the work | Named member |

### How Issue Assignment Works

1. When a GitHub issue gets the `squad` label, the **Lead** triages it — analyzing content, assigning the right `squad:{member}` label, and commenting with triage notes.
2. When a `squad:{member}` label is applied, that member picks up the issue in their next session.
3. Members can reassign by removing their label and adding another member's label.
4. The `squad` label is the "inbox" — untriaged issues waiting for Lead review.

## Rules

1. **Eager by default** — spawn all agents who could usefully start work, including anticipatory downstream work.
2. **Scribe always runs** after substantial work, always as `mode: "background"`. Never blocks.
3. **Quick facts → coordinator answers directly.** Don't spawn an agent for "what port does the server run on?"
4. **When two agents could handle it**, pick the one whose domain is the primary concern.
5. **"Team, ..." → fan-out.** Spawn all relevant agents in parallel as `mode: "background"`.
6. **Anticipate downstream work.** If a feature is being built, spawn the tester to write test cases from requirements simultaneously.
7. **Issue-labeled work** — when a `squad:{member}` label is applied to an issue, route to that member. The Lead handles all `squad` (base label) triage.
