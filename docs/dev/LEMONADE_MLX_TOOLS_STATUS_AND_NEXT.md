# Lemonade MLX + Tools: Status & Next Work Plan

**Status date:** 2026-07-20 (live-checked against GitHub)  
**Owner (this workstream):** antmikinka  
**Backend owner (lemonade lemon-mlx integration):** fl0rianr (+ co-author bong-water-water-bong)  
**Constraint:** No force-push for landing tools work. Do not race a second full backend PR.

This document is the single coherent reference for **where we are**, **what each issue/PR/branch means**, and **exactly how we proceed next**.

---

## 1. One-sentence status

**Land lemon-mlx backend via #2013 first; ship OpenAI tools as dedicated follow-up PR `feat/lemon-mlx-tools` (tools-only, stacked on #2013 until merge). Engine tools (#62) merged; product pin for tools is `b1050-stable`. Full-stack draft #2751 remains sandbox only.**

---

## 2. Architecture (two layers)

Tools require **both** repos. Do not confuse them.

```
┌──────────────────────────────────────────────────────────────────┐
│  lemon-mlx-engine  (inference binary)                             │
│  Job: load model, run chat, PARSE markup → emit OpenAI tool_calls │
│  Engine #62: MERGED to main (2026-07-19)                          │
│  Tools-capable release pin: b1050-stable (includes #62)           │
│  Stock b1049 / earlier: NO tools commits                          │
└────────────────────────────┬─────────────────────────────────────┘
                             │  lemonade spawns server via
                             │  lemon-mlx.rocm_bin / builtin pin
                             ▼
┌──────────────────────────────────────────────────────────────────┐
│  lemonade  (API / process manager / UI)                           │
│  Job: forward tools, preserve tool_calls on SSE, caps/tests      │
│  Backend vehicle: PR #2013 (fl0rianr/add_mlx_lemon_backend)       │
│  Tools follow-up: feat/lemon-mlx-tools (this workstream PR)      │
└──────────────────────────────────────────────────────────────────┘
```

| Layer | Without it… |
|-------|-------------|
| Engine tools (#62 / b1050+) | Model may print tool XML/text; no structured `tool_calls` |
| Lemonade stream hygiene | Engine tools can be dropped/clobbered on stream path |
| Lemonade caps True | `server_llm` 012/013 skip for lemon-mlx |
| Backend #2013 | No lemon-mlx recipe in product at all |

---

## 3. Issues

| Issue | Repo | Title | Role | State |
|-------|------|-------|------|--------|
| [#1642](https://github.com/lemonade-sdk/lemonade/issues/1642) | lemonade | MLX Engine ROCm backend feature | **Product home for lemon-mlx** | OPEN |
| [#60](https://github.com/lemonade-sdk/lemon-mlx-engine/issues/60) | engine | gfx1150 missing from ROCm release fatbin | Hardware/release (orthogonal to tools) | OPEN |

**Rule:** `#1642` is closed by the **backend** PR (#2013), not by a tools-only follow-up.

---

## 4. Pull requests (live roles)

### 4.1 lemonade

| PR | Branch | Role | State | Merge? |
|----|--------|------|-------|--------|
| [#2013](https://github.com/lemonade-sdk/lemonade/pull/2013) | `fl0rianr/add_mlx_lemon_backend` | **Backend vehicle** (chat, reasoning stream, ROCm, registry, UI, CI) | OPEN, MERGEABLE | **Yes — primary** |
| **Tools follow-up** | `feat/lemon-mlx-tools` | **Tools-only** (stream hygiene, caps, harness, honest labels, pin b1050) | Open when pushed | **Yes — after #2013** (or stacked on #2013 tip for review) |
| [#2751](https://github.com/lemonade-sdk/lemonade/pull/2751) | `fix/mlx-stream-tool-hygiene` | **Sandbox / validation stack** (full reimpl + tools for learning) | OPEN **draft** | **No** as second full backend |

### 4.2 lemon-mlx-engine

| PR | Branch | Role | State | Merge? |
|----|--------|------|-------|--------|
| [#62](https://github.com/lemonade-sdk/lemon-mlx-engine/pull/62) | `feat/openai-tools-server` | **Tools runtime** (parse/emit, Qwen XML, tools_auto thinking) | **MERGED** 2026-07-19 | Done |
| [#61](https://github.com/lemonade-sdk/lemon-mlx-engine/pull/61) | `fix/rocm-hip-arch-gfx1150` | gfx1150+gfx1151 HIP fatbin for ROCm CI/releases | OPEN **draft** | Independent of tools |

---

## 5. Branches (what exists / what does not)

| Branch | Repo | Status | Notes |
|--------|------|--------|-------|
| `fl0rianr/add_mlx_lemon_backend` | lemonade | **Active** | Backend tip — **does not** contain tools commits |
| `feat/lemon-mlx-tools` | lemonade | **Active** | Tools-only stack on #2013 tip; dedicated PR |
| `fix/mlx-stream-tool-hygiene` | lemonade | **Active (draft PR)** | Sandbox tip — reference only |
| `tools/stack-on-pr2013` | lemonade | **Deleted** | Temporary mirror; removed by design |
| `feat/openai-tools-server` | engine | **Merged via #62** | Tools on engine main / b1050-stable |
| `fix/rocm-hip-arch-gfx1150` | engine | **Active (draft PR)** | gfx1150 fatbin only |
| `main` | lemonade | trunk | **No** lemon-mlx until #2013 merges |

---

## 6. Does #2013 “already have tools”?

### Short answer: **No** (not end-to-end)

| Check on #2013 tip | Result |
|--------------------|--------|
| Engine pin | `b1049-stable` (no tools) |
| `mlx_server.cpp` tool_calls preserve/hygiene | **Absent** |
| `capabilities.py` lemon-mlx `tool_calls` | **False** |
| server_llm 012/013 for lemon-mlx | **Skipped** |
| Registry `tool-calling` labels | Present on some MLX models (label-only; tests off) |

---

## 7. Engine tools status

| Item | State |
|------|--------|
| PR #62 | **MERGED** to engine `main` |
| Commits | `66c0353` tools parse/emit; `52b061b` Qwen XML; `dbc5740` tools_auto |
| Release pin with tools | **`b1050-stable`** (2026-07-20) |
| b1049 and earlier | **No** tools |

Product override still works:

```json
"lemon-mlx": {
  "backend": "rocm",
  "rocm_bin": "/path/to/tools-enabled/server",
  "args": ""
}
```

Product `args` must stay `""` (no global process `--no-think` default). Test harness may use `--no-think` for lemon-mlx tools tests only.

---

## 8. History of our lemonade tools work (what happened)

1. Built and validated tools end-to-end locally (engine + lemonade).
2. Opened draft PRs: lemonade **#2751**, engine **#62**, engine **#61**.
3. Recognized **#2013** already owns the backend + intends to close **#1642**.
4. Coordinated with fl0rianr: merge **#2013 first**, tools as **follow-up**.
5. Temporary tools stack on #2013 was later wiped by force-update of his tip.
6. Engine **#62 merged**; **b1050-stable** cut with tools.
7. **2026-07-20:** opened dedicated **`feat/lemon-mlx-tools`** from #2013 tip with tools-only commits (no second full backend).

Sandbox #2751 remains for reference; **do not merge it as the backend.**

---

## 9. Social / process agreements (rules)

| Rule | Detail |
|------|--------|
| Backend ownership | fl0rianr / #2013 |
| Tools follow-up ownership | antmikinka (`feat/lemon-mlx-tools`) |
| No force-push | Do not rewrite his or published history for landing |
| No dual full backend | Do not land #2751 as competing “add lemon-mlx” |
| #1642 close | **#2013 only** — tools PR uses `Related to #1642`, never `Closes #1642` |
| Product defaults | `lemon-mlx.args` stays `""` |
| No lemonade L4a | Do not auto-inject `/no_think` merely because `tools` is present; engine `tools_auto` owns that |

---

## 10. What `feat/lemon-mlx-tools` contains

### 10.1 Included (must)

| Area | Intent |
|------|--------|
| `mlx_server.cpp` stream path | Preserve/dedupe engine `tool_calls`; finish_reason tracking; blocking→stream emit tools; no free-text invent |
| Optional argv INFO log | Engine argv on spawn (audit custom args) |
| `capabilities.py` | `tool_calls` / `tool_calls_streaming` **True** for lemon-mlx + policy comments |
| `server_models.json` | Only `Qwen3.5-4B-MLX` labeled `tool-calling`; strip dishonest labels on plumbing models |
| `server_base.py` | Nested config merge; test-only default `--no-think` for lemon-mlx tests |
| `server_llm.py` | lemon-mlx tools budget ≥128; name asserts on 012/013 |
| `test_models.py` | `SAMPLE_TOOL` `description` |
| Integration tests | Hygiene markers + scoped caps True + label honesty |
| `backend_versions.json` | Pin lemon-mlx → **`b1050-stable`** (tools-capable; CI honesty policy **A**) |
| Optional L-fwd | Forward `enable_thinking=true` to backend (no `/no_think` injection for tools) |

### 10.2 CI honesty policy chosen: **A**

| Path | Honest? |
|------|---------|
| Caps True + b1049 | **No** |
| Caps True + **b1050-stable** (tools) | **Yes** |
| Caps False until pin | Interim only |

**This PR:** caps True **and** pin b1050-stable in the same tools follow-up.

### 10.3 Forbidden

| Item | Note |
|------|------|
| Whole-file replace of `mlx_server.cpp` | Surgical port only |
| Second full backend scaffold | #2013 owns that |
| `Closes #1642` on tools PR | Related only |

---

## 11. Next work plan (ordered)

### Phase 0 — Backend landing

| # | Action | Owner |
|---|--------|--------|
| 0.1 | **Do not** re-push tools onto #2013 tip unless asked | antmikinka |
| 0.2 | Watch **#2013** for merge to `main` | antmikinka / fl0rianr |
| 0.3 | Leave **#2751** as draft sandbox or close when ready | antmikinka |

### Phase 1 — Tools follow-up (in progress)

```text
# Branch (created 2026-07-20 from #2013 tip while main lacks mlx)
git fetch origin
git checkout feat/lemon-mlx-tools   # tools-only commits on top of #2013 tip

# After #2013 merges:
git fetch origin main
git rebase origin/main   # or retarget PR base to main
# normal push only — no force-push of others' history
```

PR body must include:

- Summary of tools-only scope  
- `Related to #1642` (not Closes)  
- Depends on **#2013** + engine tools pin **b1050-stable** (engine #62)  
- Product `args` empty  
- CI honesty policy **A**  
- Test plan  

### Phase 2 — After both land

| # | Action |
|---|--------|
| 2.1 | Confirm lemon-mlx tools 012/013 green on CI with stock b1050 pin |
| 2.2 | Close or archive sandbox #2751 |

### Phase 3 — Optional hardware

| # | Action |
|---|--------|
| 3.1 | Land engine #61 for gfx1150 ROCm release fatbin (#60) |

---

## 12. Validation gates

| Gate | Requirement | Status on feat/lemon-mlx-tools |
|------|-------------|-------------------------------|
| G1 | Build `lemond` green | (local build when available) |
| G2 | `pytest test/test_lemon_mlx_integration.py` green | **4 passed** (2026-07-20) |
| G3 | Product `defaults.json` → `lemon-mlx.args == ""` | **OK** |
| G4 | If caps True: 012/013 vs tools engine | Pin b1050; harness `--no-think` for tests |
| G5 | No second full backend in this PR | **Tools-only commits** |
| G6 | Body: Related #1642 / engine #62 / b1050; never Closes #1642 alone | PR body |
| G7 | No force-push of published history | Normal push of feat branch only |

---

## 13. Reference links

| Resource | URL |
|----------|-----|
| Feature issue | https://github.com/lemonade-sdk/lemonade/issues/1642 |
| Backend PR | https://github.com/lemonade-sdk/lemonade/pull/2013 |
| Sandbox PR | https://github.com/lemonade-sdk/lemonade/pull/2751 |
| Engine tools PR | https://github.com/lemonade-sdk/lemon-mlx-engine/pull/62 (merged) |
| gfx1150 PR | https://github.com/lemonade-sdk/lemon-mlx-engine/pull/61 |
| gfx1150 issue | https://github.com/lemonade-sdk/lemon-mlx-engine/issues/60 |
| Tools-capable pin | b1050-stable |

---

## 14. Glossary

| Term | Meaning |
|------|---------|
| **Backend vehicle** | PR that introduces lemon-mlx into lemonade (#2013) |
| **Tools delta / follow-up** | Small PR: stream hygiene + caps/tests + pin |
| **Tools runtime** | Engine code that emits `tool_calls` (#62 / b1050+) |
| **Sandbox** | #2751 full stack for validation; not the merge path |
| **Policy A** | Caps True only with tools-capable pin |
| **Tier-1 stream tools** | Complete tool_call deltas after generation (not true arg streaming) |

---

## 15. Bottom line

| Question | Answer |
|----------|--------|
| Where is the backend landing? | **#2013 → main** |
| Where is tools landing (lemonade)? | **`feat/lemon-mlx-tools` dedicated PR** |
| Where is tools landing (engine)? | **#62 MERGED; pin b1050-stable** |
| What is #2751? | **Sandbox only** |
| Are tools on #2013 tip now? | **No** (tools are on feat/lemon-mlx-tools only) |
| What do we do after #2013 merges? | Rebase/retarget tools PR onto `main` |

---

*Updated 2026-07-20: engine #62 merged, b1050-stable tools pin, feat/lemon-mlx-tools opened.*
