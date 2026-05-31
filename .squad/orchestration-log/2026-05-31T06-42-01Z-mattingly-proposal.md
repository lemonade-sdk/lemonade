# Orchestration Log: mattingly — Presets v1.4 Proposal
**Date:** 2026-05-31T06:42:01Z
**Agent:** mattingly (claude-sonnet-4.6)
**Session Topic:** Presets v1.4 (capability-keyed presets, staged bindings, sampling wire-through)
**Run:** 1/2 (proposal audit)

## Task
Audit current presets UX in `prototype/ui-redesign/` and produce v1.4 proposal with concrete UX improvements.

## Output
Decision file: `.squad/decisions/inbox/mattingly-presets-v1.4-proposal.md`

## Summary
- Identified React implementation divergence from v1.3 capability-keyed model
- Documented 8 concrete UX problems (technical terminology, missing compatibility guards, sampling not applied, etc.)
- Proposed 8 v1.4 improvements with specific file locations and POC schema
- Raised 7 open questions for Kyle on design choices
- Recommended schema: `{ applies_to: Capability[], engine_hint?, options, sampling?, starter }`
