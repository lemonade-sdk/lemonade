# Spec Writing Guide

A technical spec tells a reviewer exactly what changes from `main`, precisely enough to implement without asking the author. These rules apply to RFC bodies and design specs. The general voice and formatting rules in the [documentation guide](./documentation.md) apply too.

- [Structure](#structure)
- [Precision](#precision)
- [Content](#content)
- [Checklist](#checklist)

## Structure

1. **Open every section with its point, and stay inside it.** The first sentence of each section and subsection says what it covers. Content outside that scope moves to the section that owns it: a section scoped to native hosts holds no sandbox cases.
2. **Mirror the thing described.** If a command has three parts, the section has three subsections, one per part.
3. **Organize by component, not by timeline.** Describe each component once, in its own section. A section that retells other sections in time order duplicates them.
4. **Put a change above what depends on it.** A significant change, such as a package bundling a new binary, gets its own heading, placed before the first content that relies on it.
5. **Use tables and lists over prose.** Cases that fit a table go in its rows. Context about a table goes in the sentence that introduces it ("With Podman, `lemond` connects at `127.0.0.1:<port>`, and the command adds:"), never in a sentence trailing after it.
6. **Title-case headings.** Capitalize every major word in a section heading ("Setup Assistant", not "Setup assistant"). Keep articles, conjunctions and short prepositions lowercase unless they come first, and leave code such as `lemond.service` as written. A link to a section uses the heading's exact text.
7. **List steps and alternatives.** A multi-step process, including one inside a table cell, is an ordered list, and a step with its own sub-steps gets a nested ordered list. A set of alternatives ("through a unit, a shell or an app") is an unordered list.

## Precision

8. **Name the exact thing, every time.** Name the package (`lemonade-server` snap, not "the snap"), the account (`lemonade`, root, the user's own), the process or the service (`lemond` or `lemond.service`), and the file or socket path.
9. **Define every term.** Each term is defined in the spec or is plain English. Avoid invented shorthand such as "the argv after the image".
10. **Replace vague values with specifics.** Words such as "several", "all of it", "the backend's code" or "as in the rows above" hide a value the reader needs.
11. **Keep each table cell self-contained.** A cell never contradicts its row and never points at other rows. It holds one recommended option, not a decision tree.
12. **Split tables by variant when behavior differs.** When two variants (Podman and Docker, Windows and Linux) behave identically, use one table. When they differ at all, give each its own table, without exception notes.

## Content

13. **Describe the change from `main`.** Describe `main` only where the reader needs it to understand the change. Describe the design, not a prototype branch.
14. **Verify before writing.** Check every claim against the code, a manifest or the upstream source. State what is unverified.
15. **Answer researchable questions.** Look up anything the source can answer, such as which release added a feature, and cite it. An open question is reserved for a decision the reviewers must make.
16. **State each fact once.** Refer to it elsewhere by section name, and hyperlink every section reference to its heading.
17. **Leave out the obvious.** Omit behavior every reader already expects, such as an install failing while offline.
18. **Phrase positively.** Say what the design does.

## Checklist

- [ ] Every section and subsection opens with a sentence saying what it covers, and holds nothing outside that scope.
- [ ] Every heading is title-cased, and every link to it uses its exact text.
- [ ] Every package, account, process, service and path is named exactly.
- [ ] No table cell contradicts its row, points at another row, or holds a decision tree.
- [ ] Variants that behave differently have separate tables.
- [ ] Multi-step processes are ordered lists.
- [ ] Every fact appears once, and every section reference is a link.
- [ ] Every claim was checked against code or upstream source.
- [ ] The whole spec was re-read end to end after the last edit, for consistency.
