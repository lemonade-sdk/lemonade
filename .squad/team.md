# Squad Team

> lemonade

## Coordinator

| Name | Role | Notes |
|------|------|-------|
| Squad | Coordinator | Routes work, enforces handoffs and reviewer gates. |

## Members

| Name | Role | Charter | Status |
|------|------|---------|--------|
| 🏗️ Lovell | Lead | `.squad/agents/lovell/charter.md` | Active |
| 🔧 Liebergot | C++ Server Core | `.squad/agents/liebergot/charter.md` | Active |
| ⚙️ Aaron | Backend Integrator | `.squad/agents/aaron/charter.md` | Active |
| 📦 Kranz | Build & Release | `.squad/agents/kranz/charter.md` | Active |
| 🧪 Haise | QA / Integration | `.squad/agents/haise/charter.md` | Active |
| ⚛️ Mattingly | UI / Frontend | `.squad/agents/mattingly/charter.md` | Active |
| 📋 Scribe | Session Logger | `.squad/agents/scribe/charter.md` | Active |
| 🔄 Ralph | Work Monitor | `.squad/agents/ralph/charter.md` | Active |

## Project Context

- **Project:** lemonade
- **Repo:** github.com/lemonade-sdk/lemonade (working tree: c:\dev\repos\lemonade)
- **User:** Kyle Poineal (contributor / maintainer)
- **Created:** 2026-05-15
- **Working branch:** `feat/ui-testing`
- **Casting universe:** Apollo 13 (specialists keeping a complex machine running under pressure)

## Standing Rules

- 🚫 **DO NOT MERGE into `main`.** All work commits directly to `feat/ui-testing` until the user says otherwise.
- 🎯 **UI POC scope:** Build a new UI side-by-side with the existing one. Both must work in browser and as a desktop app.
- 🛑 **`lemond` backend is OFF LIMITS for the POC.** No C++ server changes for UI work.
- 🔒 **Squad files stay invisible to upstream:** `.squad/`, `.copilot/`, `.github/agents/squad.*`, squad workflows are excluded via `.git/info/exclude` (NOT `.gitignore`).
