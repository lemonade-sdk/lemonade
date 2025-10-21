# Spec: lemon.cpp

## Introduction

This document describes `lemon.cpp`, a C++ implementation of Lemonade's python LLM server.

At a high level, `lemon.cpp` is a lightweight local LLM server-router. The user/client sends an LLM completion request, `lemon.cpp` routes that request to a local LLM server, and then routes the response back to the client.

The local LLM servers, which we refer to as `WrappedServer`s, are responsible for generating the completion using an LLM. `WrappedServer` is defined in detail below.

The initial implementaiton of `lemon.cpp` will support two `WrappedServer`s: `llama-server` (from `llama.cpp`), and `FastFlowLM`. This support allows `lemon.cpp` to run LLMs on CPU/GPU and NPU, respectively.

## Implementation Guidelines

### Similarities to Python

From a black-box user's perspective, `lemon.cpp` should be very similar to the original Python implementation:
- `lemon.cpp` will implement all server endpoints described in `docs/server/server_spec.md`.
- `lemon.cpp` will pass all tests in `test/server_llamacpp.py` and `test/server_flm.py`.
    - We will keep using these Python-based integration tests; no need to write new integration tests in C++.
    - `test/utils/server_base.py` will need a new command line argument to instantiate a `lemon.cpp` server instead of a Python `lemonade-server-dev`.
- `lemon.cpp` will be fully compatible with the web ui HTML, JavaScript, and CSS defined in `src\lemonade\tools\server\static`
    - That web ui code may need to be copied to a new location at build time so that `lemon.cpp` can serve it.
- The `lemon.cpp` CLI will implement all of the same commands and arguments as the Python `lemonade-server-dev` defined in `src\lemonade_server\cli.py`.
- `lemon.cpp` will use `src\lemonade_server\server_models.json` as its model registry.
    - This file may need to be copied at build time so that `lemon.cpp` can use it.
    - `lemon.cpp` should be able to correctly filter out models with recipes that aren't supported yet.
- `lemon.cpp` should have the same policies as the Python implemention for downloading and installing `llama.cpp` and `FastFlowLM` backends.

### Differences from Python

`lemon.cpp` also represents an opportunity for streamling the codebase relative to the original Python implementaiton:
- `lemon.cpp` will have no benchmarking, accuracy, or model building/optimization tooling.
- `lemon.cpp` will treat all LLM backends equally as peer `WrappedServer`s in the implementation.
- `lemon.cpp` will have minimal external dependencies, a fast install time, and minimal size on disk.

## Build Artifacts

The `lemon.cpp` source code is located in `src\cpp`.

Building the `lemon.cpp` project with `cmake` will result in a `lemonade` CLI, which should implement all of the same commands and arguments as the Python `lemonade-server-dev`.

`lemon.cpp` should work on Windows, Ubuntu, and macOS. On Windows, the executable should be named `lemonade.exe`.

## WrappedServer

The most important class in `lemon.cpp` is called `WrappedServer`. There is a Python reference implementation in `src\lemonade\tools\server\wrapped_server.py`.

There are also reference implementations for `llama.cpp` and `FastFlowLM` in `src\lemonade\tools\server\llamacpp.py` and `src\lemonade\tools\server\flm.py`, respetively. Note that a lot of logic is implemented in `src\lemonade\tools\llamacpp` and `src\lemonade\tools\flm`, respectively, as well as `src\lemonade_server\model_manager.py`. This code structure is messy and should not be immitated in `lemon.cpp`--just use this code as a functional reference.

The purpose of `WrappedServer` is to provide a unified interface between domain-specific local LLM servers to the high-level `lemon.cpp` server-router. 

A `WrappedServer` implementation needs to be able to:
- Install and start the backend server.
- Pull, load, unload, and delete server-specific models.
- Route `chat/completions` requests to the backend server and route the response back to the client.
    - We will implement support for `completions`, `resposnes`, `embedding`, and `reranking` APIs later.
- Parse the wrapped server's stdout/stderr for important information such as performance data, important errors to escalate, etc.

## Testing

`lemon.cpp` will be hosted in the GitHub repo https://github.com/lemonade-sdk/lemonade

`lemonade.cpp` should be tested using GitHub actions. There are already workflows in `.github` for running `test/server_llamacpp.py` and `test/server_flm.py`, so these will be copied as a starting point and adapted for testing `lemon.cpp` instead of the Python implementation.