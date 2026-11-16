# Spec Writing Guide

A technical spec tells a reviewer what an important part of the Lemonade design will be, precisely enough to perform the implementation. This guide can help you write specs that meet the expectations of reviewers. It builds on the general voice and formatting guidelines in the [documentation guide](../documentation.md).

## Structure

1. The first sentence of each section states the scope of the section.
1. Describe each component once, in its own section, to produce a tops-down view of the architecture. Avoid organizing content chronologically.
1. A significant change gets its own heading, placed before the first content that relies on it.
1. Use tables, mermaid diagrams, and lists over prose. Context about a table goes in the sentence that introduces it, not in a sentence trailing after it.
1. Title-case headings. Capitalize every major word in a section heading ("Setup Assistant", not "Setup assistant").
1. A multi-step process, including one inside a table cell, is an ordered list, and a step with its own sub-steps gets a nested ordered list. A set of alternatives ("through a unit, a shell or an app") is an unordered list.

## Precision

1. Use precise names. For example: "`lemonade-server` snap", not "the snap". `class::method()`, not `method()`.
1. Replace vague values with specifics.
1. Keep each table cell self-contained. A cell never contradicts its row and never points at other rows. It holds one recommended option, not a decision tree.
1. Split tables by variant when behavior differs. When two variants (Podman and Docker, Windows and Linux) behave identically, use one table. When they differ at all, give each its own table, without exception notes.

## Content

1. Check every claim against the code, a manifest or the upstream source. State what is unverified.
1. Answer researchable questions. Look up anything the source can answer, such as which release added a feature, and cite it. An open question is reserved for a decision the reviewers must make.
1. State each fact once. Refer to it elsewhere by section name, and hyperlink every section reference to its heading.
1. Leave out the obvious. Omit behavior every reader already expects, such as an install failing while offline.
1. Phrase positively: say what the design does, and do not enumerate design alternatives that were not selected.
