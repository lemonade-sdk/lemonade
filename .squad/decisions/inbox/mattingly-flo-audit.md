# 2026-06-05: Tool runtime and Omni collection guardrails

**Author:** Mattingly
**Scope:** `prototype/ui-redesign/`
**Status:** Proposed

## Recommendation

1. Tool-call runtimes should never fail silently. Invalid streamed tool chunks, executor exceptions, and backend tool failures should be surfaced in the chat UI and logged with tool name/model/conversation context, without API keys or local paths.
2. `collection.omni` should remain UI-only in the POC. Load/Get & Load actions for Omni collections should operate on component models, not call `lemond` with the collection wrapper recipe.
3. The chat Tools toggle should distinguish Lemonade management tools from always-on Omni media tools, or expose a separate “Omni tools” state if users are expected to disable them.
