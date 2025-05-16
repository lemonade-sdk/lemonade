# Lemonade Demos

The demo scripts in this folder show how `lemonade` can be used in integrate OnnxRuntime-GenAI (OGA) into higher-level applications such as chat and search.

The format of each demo is to have two files which show the before-and-after of integrating OGA:
    - `*_start.py`: a version of the application that uses regular software to try and handle a natural language task.
    - `*_hybrid.py`: an upgrade of the application that integrates an LLM with Ryzen AI Hybrid to improve the natural language task.

The demos available are:
    - `chat/`: prompts the user for a message and then streams the LLM's response to the terminal.
    - `search/`: demonstrates how a user can search an employee handbook in natural language using an LLM.

To run a demo:
1. Set up a conda environment with the appropriate framework and backend support.
1. `cd` into the demo directory (e.g., `cd search/`)
1. Run the `*_start.py` script to see what the application is like without the LLM (e.g., `python search_start.py`)
1. Run the `*_hybrid.py` script to see what the application is like with the LLM (e.g., `python search_hybrid.py`)

<!--This file was originally licensed under Apache 2.0. It has been modified.
Modifications Copyright (c) 2025 AMD-->