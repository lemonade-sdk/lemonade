# Documentation Guide

Lemonade's documentation is a first-class part of the product. Per our [philosophy](./philosophy.md): if the document is long and full of conditionals, the feature isn't done yet. Good documentation is short, accurate, and self-evident.

This guide covers style, structure, and the contribution process for both community authors and AI-assisted contributions.

---

## Principles

### Documentation completes the feature

A feature isn't shipped until users can find, understand, and act on it without asking for help. Treat every doc PR the same as a code PR: reviewable, mergeable, improvable.

### Automate the documentation away

Long docs signal a design problem, not a thoroughness problem. If you find yourself adding paragraph after paragraph of conditionals and edge cases, bring the issue back to the feature and simplify.

### Shorter is better, not briefer

"Short" doesn't mean "skip context." It means every sentence earns its place. Remove sentences that restate the title, define terms the reader knows, or describe what's visible in a code example.

### The reader is a capable adult

Don't pad with reassurances ("This is easy!") or apologies ("Unfortunately…"). State facts.

---

## Voice and Tone

| Attribute | Do | Don't |
|---|---|---|
| Register | Professional and direct | Formal or stiff |
| Warmth | Welcoming, community-first | Condescending or presumptuous |
| Pronouns | "you" for the reader; "we" for the project | "the user", "one" |
| Contractions | Fine in prose | Avoid in parameter descriptions or API tables |
| Emoji | 🍋 in the main README and taglines only | Scattered through guides |
| Humor | A single dry observation is fine | Extended jokes or memes |

**Confident → qualifying → hedging**

Prefer confident statements. Reserve qualifiers for genuine uncertainty.

```
✓  The server loads models on first request.
✗  The server should load models on first request in most cases.
```

---

## Document Structure

### Required sections by type

**Concept / explainer**
1. One-sentence statement of what this is and why it matters.
2. Key concepts in a table or short list — no more than 5.
3. A concrete example (code or screenshot).
4. Links to the next logical step.

**How-to / task guide**
1. One-sentence goal statement.
2. Prerequisites (inline, not a separate page unless they are substantial).
3. Numbered steps. Each step is a single action with a single expected outcome.
4. A "next steps" or "see also" list at the end.

**API reference**
1. Method + path as an H2 heading.
2. Status badge immediately after the heading.
3. One-sentence description.
4. Parameters table.
5. Example request (bash curl by default; add Python/JS tabs when usage warrants).
6. Response format.
7. Error notes, if non-obvious.

**Integration guide**
1. Brief description of what the integration does (one sentence).
2. Prerequisites.
3. Steps: launch → configure → verify.
4. Link to the external tool's own documentation for anything Lemonade doesn't control.

### Headings

Use ATX-style headings (`#`, `##`, `###`). Never skip a level.

- H1 (`#`): document title, one per file.
- H2 (`##`): major sections — prerequisites, steps, parameters.
- H3 (`###`): subsections within a major section.
- H4 (`####`): use sparingly; restructure if you need H5.

Make heading text scannable and action-oriented: "Install on Ubuntu", not "Installation".

### Table of contents

Add a ToC only if the document has **5 or more H2 sections**. Use a plain markdown list with anchor links. Place it directly after the opening paragraph.

---

## Formatting

### Code blocks

Always include a language tag.

````markdown
```bash
lemonade run Gemma-4-E2B-it-GGUF
```
````

Common tags: `bash`, `powershell`, `python`, `json`, `cpp`, `typescript`, `yaml`.

Use `bash` for shell commands that work cross-platform (curl, lemonade CLI). Use `powershell` only for Windows-specific syntax.

Annotate code sparingly — only when the command has a non-obvious flag or the output is surprising. Never restate what the code visibly does.

### Multi-platform variants

Use MkDocs tab syntax when a task has meaningfully different steps per OS. Keep the tab names consistent across documents: `Windows`, `Linux`, `macOS`.

```markdown
=== "Linux"
    ```bash
    sudo apt install lemonade
    ```

=== "Windows"
    Download the installer from the releases page.
```

Don't create tabs for minor differences (a single flag change, a different path). Use a `>` note instead.

### Tables

Use tables for:
- API parameters (always)
- Platform or feature matrices
- Comparisons of 3+ items

Don't use tables for:
- Two-item comparisons (use a list)
- Simple key-value pairs when prose reads better

Column header convention: Title Case. Align pipes. Include a header separator row.

```markdown
| Parameter | Required | Description |
|-----------|----------|-------------|
| `model`   | Yes      | Model name from the registry. |
```

### Inline formatting

- **Bold**: key terms on first use, UI labels, file names when emphasis aids scanning.
- `Inline code`: CLI commands, parameter names, file paths, values, environment variables.
- *Italics*: titles of external works, or occasional emphasis in prose. Not for technical terms.

Never bold a sentence for general emphasis. If a point is important enough to bold, make it its own sentence or a `>` callout.

### Callouts

Use `>` blockquotes for notes that interrupt the flow:

```markdown
> Note: `model_name` for user-registered models must use the `user.` namespace to avoid collisions with built-in models.
```

For warnings that could cause data loss or security issues:

```markdown
> **Warning:** Deleting a backend removes all associated binaries. Models remain intact.
```

Avoid overuse. If every other paragraph has a note, the content needs restructuring.

### Status badges

Use inline status badges in API reference documents to show endpoint availability:

```markdown
<sub>![Status](https://img.shields.io/badge/status-fully_available-green)</sub>
<sub>![Status](https://img.shields.io/badge/status-partially_available-yellow)</sub>
<sub>![Status](https://img.shields.io/badge/status-not_available-red)</sub>
```

Place them on the line immediately after the endpoint H2, before the description.

### Collapsible sections

Use `<details>` for supplementary content that would interrupt the main flow for most readers — extended examples, historical context, deep-dive explanations.

```markdown
<details>
<summary>Why does the server use subprocesses?</summary>

Backends run as subprocesses so that a crash in one backend doesn't take down the router or any other loaded models.

</details>
```

Don't hide information a first-time reader needs. If something is essential, keep it in the main flow.

---

## File and Directory Conventions

```
docs/
  guide/          User-facing: install, CLI, configuration, concepts, FAQ
  api/            API reference by standard (openai, ollama, anthropic, lemonade)
  dev/            Developer-facing: setup, philosophy, contribute, CI
  integrations/   Third-party app integration guides (one file per app)
  embeddable/     Guides for embedding Lemonade in another app
```

**Naming:** lowercase, hyphen-separated. Match the CLI or feature name where one exists (`cli.md`, `multi-model.md`).

**One topic per file.** A file that covers installation *and* configuration *and* troubleshooting should be three files with a README index.

**Every directory must have a `README.md`** that briefly explains the section and links to its files. Keep READMEs under 30 lines; they are navigation aids, not content.

---

## Writing for AI-Assisted Contributions

AI tools can draft documentation faster than most people can type. That speed comes with specific failure modes you must check before submitting.

### What AI gets wrong in docs

| Failure mode | What to look for |
|---|---|
| Hallucinated parameters | Parameter names, defaults, or behaviors that don't match the actual code |
| Stale information | Correct as of the training data, wrong today (version numbers, endpoint paths, flags) |
| Padding | Restatements, filler phrases ("It is worth noting that…"), over-explained obvious steps |
| Invented caveats | Warnings about edge cases that don't exist in this codebase |
| Wrong tone | AI defaults to formal or hedged; Lemonade docs are direct and confident |
| Generic examples | Examples using placeholder models or endpoints not in the registry |

### Checklist before submitting an AI-drafted doc

- [ ] Every parameter name and default verified against the source code or a live server.
- [ ] All CLI commands run locally and produced the expected output.
- [ ] All links resolve to real pages.
- [ ] No sentences that begin with "It is important to note", "Please ensure", or "It's worth mentioning".
- [ ] Code examples use real Lemonade model names from the registry, not `<your-model-here>`.
- [ ] The document passes the length test: if it's longer than it needs to be, cut it.
- [ ] The document was reviewed by its human author, not just the AI that wrote it.

### Disclosing AI assistance

You are not required to disclose that AI helped draft your documentation PR, but you *are* responsible for its accuracy. "The AI wrote it" is not a response to a correctness review comment.

If you use an AI tool to generate review comments on a documentation PR, label them clearly so human reviewers can weigh them appropriately:

```
[AI-assisted review] The parameter description in the table doesn't mention the default value.
```

---

## Community Contribution Process

### Before you start

- Read the [philosophy](./philosophy.md). Ask yourself: does this doc make Lemonade simpler or more complex for a new reader?
- For substantial new sections (a new integration guide, a new concept page), post in `#documentation` on the [Discord](https://discord.gg/5xXzkMu8Zk) first. The maintainer for content and guides is `@vgodsoe`.
- For corrections, typo fixes, or small additions: just open a PR.

### What belongs in a documentation PR

A documentation PR should touch one of:
- A new file for a new feature, integration, or concept.
- An update to an existing file to correct errors or reflect new behavior.
- A structural improvement (reorganizing a directory, adding a missing README).

Don't bundle documentation changes with code changes unless the doc change is a direct consequence of the code change and the combined PR is small.

### Review expectations

Documentation PRs are reviewed for:

1. **Accuracy** — does it match what the code actually does?
2. **Alignment with the philosophy** — does it make Lemonade simpler to understand?
3. **Style** — does it follow this guide?
4. **Links** — do they all resolve?

Reviewers will not reject a PR for minor style differences. They will reject a PR for factual errors, broken links, or content that contradicts the philosophy.

### Maintainer for documentation

`@vgodsoe` is the primary maintainer for content and guides. Assign them on PRs that touch `docs/guide/` or `docs/integrations/`. For API reference (`docs/api/`), also assign the maintainer responsible for the relevant backend or API standard.

---

## Quick Reference

### Common mistakes

| Mistake | Correction |
|---|---|
| Documenting the obvious | Remove it. Trust the reader. |
| Using a table for 2 items | Use a sentence or list instead |
| Heading: "Overview" | Rename to the actual topic: "How routing works" |
| `you should` / `you must` | Just state the requirement: "Run `setup.sh` before building." |
| Passive voice | Rewrite with an active subject |
| Future tense for current behavior | Use present tense: "The server loads models on first request." |
| Nested bullet points deeper than 2 levels | Restructure as prose or a table |

### Template: integration guide

```markdown
# [App Name]

Connect Lemonade to [App Name] to [one-sentence description of what this enables].

## Prerequisites

- Lemonade Server running on `localhost:13305` ([install guide](../guide/install/README.md))
- [App Name] [version] installed

## Setup

1. Launch Lemonade: `lemonade launch`
2. Open [App Name] and navigate to **Settings → Model Provider**.
3. Set the base URL to `http://localhost:13305/v1`.
4. (Optional) Set the API key to any non-empty string if [App Name] requires one.

## Verify

Run a test prompt. You should see Lemonade's model name appear in [App Name]'s model selector.

## See also

- [App Name documentation](https://example.com)
- [Lemonade API reference](../api/openai.md)
```

### Template: CLI command reference

```markdown
## `lemonade <command>`

[One-sentence description of what the command does.]

```bash
lemonade <command> [OPTIONS]
```

| Option | Default | Description |
|--------|---------|-------------|
| `--flag` | `value` | What it controls. |

**Example:**

```bash
lemonade <command> --flag value
```
```
