# OpenSpec Project Standard

Status: active
Source pattern: https://gist.github.com/karpathy/442a6bf555914893e9891c11519de94f

## Purpose

This folder is the structured change/spec layer for this repository. It exists so agents and humans agree on intent, design, tasks, and verification before making broad changes.

## Required Flow

1. For non-trivial work, create `openspec/changes/<change-id>/`.
2. Write `proposal.md` first: why, what changes, risk, and verification.
3. Add `design.md` when architecture, storage, permissions, public behavior, or cross-repo contracts change.
4. Track implementation in `tasks.md` and keep task state current.
5. Update `docs/wiki/` with durable architecture, workflow, gotcha, or onboarding knowledge.
6. Verify with this repo's native tests/checks before marking work ready.

## Memory Model

- Raw source: committed files, examples, tests, issues, PRs, and specs.
- Wiki: `docs/wiki/` durable project memory.
- Schema: `AGENTS.md`, GitHub templates, and `openspec/`.

## Guardrails

- Prefer small, reversible changes.
- Do not overwrite existing project-specific rules.
- Do not add speculative frameworks or broad refactors.
- Record assumptions and verification.
