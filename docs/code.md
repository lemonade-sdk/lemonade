# Lemonade SDK Code Structure

The Lemonade SDK source code has a few major top-level directories:
- `.github`: defines CI workflows for GitHub Actions.
- `docs`: documentation for the entire project.
- `examples`: example scripts for use with the Lemonade tools.
- `src`: implements the `lemonade-sdk` wheel.
  - `/cpp`: source code for the lemonade-server C++ implementation.
  - `/ryzenai-server`: source code for the C++ implementation of a RyzenAI SW server.
  - `/lemonade`: source code for the lemonade-sdk python package.
    - `/tools`: implements `Tool` and defines the tools built in to the `lemonade` developer CLI (e.g., oga-load, lm-eval-harness, etc.).
      - `/server`: implements Lemonade Server.
    - `/sequence.py`: implements `Sequence` and defines the plugin API for `Tool`s.
    - `/cli`: implements the `lemonade` developer CLI.
    - `/common`: functions common to the other modules.
    - `/version.py`: defines the package version number.
    - `/state.py`: implements the `State` class.
  - `/lemonade-server`: implements the `lemonade-server` python CLI.
  - `/lemonade-install`: implements the `lemonade-install` python CLI.
- `setup.py`: defines the `lemonade-sdk` wheel.
- `test`: tests for the Lemonade SDK.

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->