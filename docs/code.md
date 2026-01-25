# Lemonade Code Structure

The Lemonade source code has a few major top-level directories:

- `.github`: defines CI workflows for GitHub Actions.
- `docs`: documentation for the entire project.
- `examples`: example scripts and demos.
- `src`: source code.
  - `/app`: Electron desktop application (Model Manager, Chat UI).
  - `/cpp`: C++ implementation of the Lemonade Server.
  - `/lemonade`: Python package for the `lemonade-eval` CLI.
    - `/tools`: implements `Tool` and defines the tools built in to the `lemonade-eval` CLI (e.g., `load`, `bench`, `oga-load`, `lm-eval-harness`, etc.).
    - `/sequence.py`: implements `Sequence` and defines the plugin API for `Tool`s.
    - `/cli.py`: implements the `lemonade-eval` CLI entry point.
    - `/common`: functions common to the other modules.
    - `/version.py`: defines the package version number.
    - `/state.py`: implements the `State` class.
- `setup.py`: defines the `lemonade-sdk` wheel.
- `test`: tests for Lemonade.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->
