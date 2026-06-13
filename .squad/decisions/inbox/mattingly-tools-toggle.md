# 2026-06-05: Tools toggle scoped-state migration

**Author:** Mattingly
**Scope:** `prototype/ui-redesign/`
**Status:** Proposed / patched in POC

## Context

The chat tools toggle was moved from the legacy global `localStorage` key `lemonade_use_tools` into scoped account storage (`lemonade:<scope>:use_tools`) when local accounts landed. That preserves the many-clients-one-server invariant, but legacy guest users can see their previous tools preference reset because the old key is not read.

## Recommendation

Scoped UI preferences should stay client-local, but each preference moved behind `scopedStorageKey()` needs an explicit one-time legacy migration for the `guest:shared` scope and a React state refresh when `accountSession.storageScope` changes.

This is not a decision to make tools default ON. It is a migration rule: preserve an existing user's explicit local preference, otherwise keep the current OFF default.
