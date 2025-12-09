# Lemonade SDK Contribution Guide

The Lemonade SDK project welcomes contributions from everyone!

See [code organization](https://github.com/lemonade-sdk/lemonade/blob/main/docs/code.md) for an overview of the repository.

## Collaborate with Your App

Lemonade Server integrates quickly with most OpenAI-compatible LLM apps.

You can:
- Share an example of your app using Lemonade via [Discord](https://discord.gg/5xXzkMu8Zk), [GitHub Issue](https://github.com/lemonade-sdk/lemonade/issues), or [email](mailto:lemonade@amd.com).
- Contribute a guide by adding a `.md` file to the [server apps folder](https://github.com/lemonade-sdk/lemonade/tree/main/docs/server/apps). Follow the style of the [Open WebUI guide](./server/apps/open-webui.md).

Guides should:
- Work in under 10 minutes.
- Require no code changes to the app.
- Use OpenAI API-compatible apps with configurable base URLs.

## Backend Contributions

To contribute code or examples, first open an [Issue](https://github.com/lemonade-sdk/lemonade/issues) with:
   - A descriptive title.
   - Relevant labels (`enhancement`, `good first issue`, etc.).
   - A proposal explaining what you're contributing.
   - The use case it supports.

One of the maintainers will get back to you ASAP with guidance.

## UI/Frontend Contributions

**Current UI Development Approach:**

For now, UI and frontend development is being handled exclusively by core maintainers. Here's why: AI-assisted coding has made building UIs incredibly fast, but it's also made reviewing UI PRs quite challenging. UI changes often involve complex state management, visual consistency, accessibility considerations, and cross-platform considerations that require deep context about the entire application architecture.

**How You Can Still Influence the UI:**

We want your creativity and insights! Share UI/UX ideas, report bugs, or request features via [Issue](https://github.com/lemonade-sdk/lemonade/issues) or [Discord](https://discord.gg/5xXzkMu8Zk). Include mockups, screenshots, and reproduction steps where relevant.

**UI Scope: Management, Not Competition:**  
Our UI exists to facilitate Lemonade management - not to compete with the apps built on top of Lemonade. While it's tempting to add agentic workflows, advanced chat features, or other sophisticated capabilities, that's not our goal. We focus on making model management, configuration, and monitoring delightful and effortless. Defining this line isn't always easy, but use this principle as your guide when considering new UI features.

## Issues

Use [Issues](https://github.com/lemonade-sdk/lemonade/issues) to report bugs or suggest features. 

A maintainer will apply one of these labels to indicate the status:
- `on roadmap`: planned for development.
- `good first issue`: open for contributors.
- `needs detail`: needs clarification before proceeding.
- `wontfix`: out of scope or unmaintainable.

## Pull Requests

Submit a PR to contribute code. Maintainers:
- @danielholanda
- @jeremyfowers
- @ramkrishna2910
- @vgodsoe

Discuss major changes via an Issue first.

## Code Formatting

We require that all Python files in this repo adhere to black formatting. This is enforced with a black check in CI workflows.

### Running Black formatting

The easiest way to ensure proper formatting is to enable the black formatter in VSCode with format-on-save:

1. **Install the Python extension**: Install the Python extension for VSCode if you haven't already.

2. **Set black as the default formatter**: 
   - Open VSCode settings (Ctrl/Cmd + ,)
   - Search for "Formatter"
   - Set the Python default formatter to "black"

3. **Enable format-on-save**:
   - In VSCode settings, search for "format on save"
   - Check the "Format On Save" option

This will automatically format your code according to black standards whenever you save a file.

#### Alternative Setup

You can also install black directly and run it manually:
```bash
# Install black (if not already installed)
pip install black

# Run black formatter on your file
black your_file.py
```

### Linting

We use linting tools to maintain code quality and catch potential issues. The project uses standard Python linting tools that are automatically run in CI.

#### Running Linters Locally

To run linting checks locally before submitting a PR:

```bash
# Install linting dependencies (if not already installed)
pip install pylint

# Run pylint from the root of the repo
pylint src/lemonade --rcfile .pylintrc --disable E0401
```
This will show linting warnings and errors in your terminal.

## Testing

Tests are run automatically on each PR. These include:
- Linting
- Code formatting (`black`)
- Unit tests
- End-to-end tests

To run tests locally, use the commands in `.github/workflows/`.

## Versioning

We follow [Semantic Versioning](https://github.com/lemonade-sdk/lemonade/blob/main/docs/versioning.md).