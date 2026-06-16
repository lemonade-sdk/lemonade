# Audit Pass 2 — af66ea14 (kpoin/ui-testing, 2026-06-13)

**Author:** Mattingly  
**Date:** 2026-06-13T08:57:25-06:00  
**HEAD:** af66ea14 (6 commits from fl0rianr)  

## Summary

Prototype is substantially improved from previous pass. The two prior P0s (Omni collection wrapper leaking to lemond, tools toggle broken) are fixed. New surface area (LogViewer, image generation, realtime audio, preset rails) is generally solid. Remaining risks are in error visibility, an unhandled-reject hang path in the tool loop, and a preset↔image-settings disconnect.

## Key findings (see full report in chat for detail):

### Still needs attention
- **Tool loop hang** (P1 → P0 risk): `onToolCalls` is not awaited in `api.chatCompletion`. If `execute()` throws unexpectedly, the stream hangs permanently. See `api.ts:1101`, `useChatStreaming.ts:215`.
- **Load errors silently swallowed** (P1): `ModelManager.tsx:858–860` — `handleLoad` catches error but only logs to console. User sees nothing.
- **Preset image settings not reflected in composer** (P1): Active preset `recipe_options.steps`/`cfg_scale` are not fed into `imageSettings` state. "Sharp (30 steps)" preset shows 20 in composer.
- **ScriptProcessorNode deprecated** (P1): `useAudioCapture.ts:70` — Chrome deprecation path. Replacement is AudioWorklet.
- **tools toggle still disabled for unknown-recipe models** (P1, narrowed): Mostly fixed by App.tsx enrichment. Persists only for custom models with no recipe field.

### Fixed since last audit
- Omni collection P0: `loadModelRuntime` loads component models correctly ✅
- Tools toggle scoping: `lemonade:<scope>:use_tools` ✅
- Preset scope sync in App.tsx: init + effect + handler ✅
- useChatStreaming `MAX_TOOL_ROUNDS = 5` ✅

### New features that work
- LogViewer: wired, virtualized, server log-level control, inline toggle in ChatView ✅
- `ask_question` UI: renders interactive buttons + custom input ✅
- `composeToolRuntimes`: mixes Lemonade + Omni tools cleanly ✅
- Image generation: validated settings, mode switching, edit gate ✅
- Realtime audio: useAudioCapture cleanup, WebSocket reconnect, error surface ✅

## Recommended next actions (priority order):
1. Add try/catch wrapper or `.catch(reject)` on the `onToolCalls` async execution path
2. Add user-visible error display to `handleLoad`/`handlePullAndLoad`
3. Sync active preset image params into `imageSettings` on preset-change event
4. Migrate ScriptProcessorNode to AudioWorklet
5. Split ChatView.tsx (101KB is unmanageable)
