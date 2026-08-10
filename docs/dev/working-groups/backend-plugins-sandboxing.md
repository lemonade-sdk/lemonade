# Working Group: Backend Plugins & Sandboxing

## Overview

**Lead:** This working group is led by Arun Babu Neelicattu, whose handle is @abn on GitHub and @abn.null on Discord.

**Background:** Lemonade runs inference by spawning backends as subprocesses (`llama-server`, `whisper-server`, `sd-server`, `flm`, `vllm`). Currently, adding a backend requires modifying C++ server sources and recompiling `lemond`. This limits Lemonade to in-tree backends and slows adoption of new inference engines.

Running third-party or community backend binaries introduces security risks (unauthorized file access, network egress, privilege escalation). Experimentation with `nono.sh` demonstrated that backend subprocesses can be restricted to least-privilege filesystem and network boundaries without impacting inference performance.

**Why:** Enables adding third-party backends without C++ code modifications while securing backend subprocesses with OS kernel sandboxing.

**Goal:** Enable `lemond` to orchestrate out-of-tree backends under kernel sandboxing by default, supported by a recipe marketplace and CLI/GUI management tooling.

---

## Scope Boundaries

- **In Scope:** Descriptor schema definition, out-of-process HTTP/socket backend adapter, process sandboxing (`nono` on Linux, platform primitives on Windows/macOS), recipe marketplace, client UI integration.
- **Out of Scope:** Engine binary compilation/build infrastructure. Lemonade orchestrates and sandboxes pre-built backend binaries; it does not build them.
- **Related Work:** Deep container engine integration and containerized backend orchestration are recommended as a follow-on **Containerized Backends & Isolation Working Group**.

---

## Proposed Architecture

```mermaid
flowchart TD
    Client["Client (CLI / GUI / SDK)"] -->|"HTTP API"| Server["lemond (Server Core)"]
    Server --> Registry["Backend Descriptor Registry"]
    Server --> Router["Router & Slot Manager"]

    subgraph Plugin System
        Registry -->|"Loads"| Descriptor["JSON Descriptors (~/.config/lemonade/backends/)"]
        Router --> Adapter["External Backend Adapter"]
    end

    subgraph Process Sandboxing (nono)
        Adapter -->|"HTTP / IPC over socket"| S1["llama-server"]
        Adapter -->|"HTTP / IPC over socket"| S2["Custom vLLM Backend"]
        Adapter -->|"HTTP / IPC over socket"| S3["NPU Engine"]
    end
```

### 1. Backend Plugin Specification

Backends transition from statically linked C++ `WrappedServer` classes to declarative JSON descriptors.

- **Descriptor Schema:** Declares metadata, capability interfaces (`ICompletionServer`, `ITranscriptionServer`, etc.), a `Host OS × Accelerator` compatibility matrix, launcher command templates, and default recipe mappings.
- **Interface Contract:** Standardized health checks, load/unload lifecycle endpoints, and API passthrough.
- **Slot Policies:** Declarative slot management (`standard`, `exclusive_npu`, `coexist_by_type`, `unmetered`) mapping directly to `lemon::SlotPolicy` enum values enforced by the Router.
- **Platform Matrix:** Platform blocks are keyed by OS first, then device accelerator. Registration validates that at least one platform block matches the host OS before registering the backend. *(Note: A container recipe's platform block describes its guest OS; GPU containers target `linux`, which Windows hosts satisfy via WSL2).*
- **Endpoint & Capability Routing:** Descriptors do not dynamically register arbitrary new HTTP endpoints on `lemond`. Instead, plugins bind to Lemonade's standard capability interfaces (`completion`, `embeddings`, `transcription`, `image`, `tts`, `reranking`). `lemond` routes standard requests (`/v1/chat/completions`, `/v1/embeddings`, etc.) directly to the sandboxed subprocess port via `ExternalBackendServer`'s HTTP/REST passthrough.
- **Iterative Specification Evolution:** The descriptor schema and permission manifest are expected to evolve throughout the Working Group lifecycle. As Phase 1 & 2 RFCs are implemented across diverse inference engines, hardware backends, container runtimes, and OS sandbox APIs, unknown unknowns will be incorporated iteratively into the schema definition.

**Process Isolation vs. In-Process Shared Libraries.** In-process C++ shared libraries (`.so`/`.dll` via `dlopen`) were evaluated and rejected for core backend extensibility:
- *Alignment with Core Architecture:* Lemonade's existing design already relies on out-of-process subprocess orchestration (`lemond` spawning `llama-server`, `flm`, `sd-server`, `vllm`). Declarative out-of-process descriptors formalize this existing model.
- *Ecosystem Incentive Structures:* Lemonade is an orchestration integrator. Upstream inference engine developers (vLLM, llama.cpp, SGLang, FastFlowLM) have little incentive to write or maintain Lemonade-specific C++ shared library plugins. Requiring dynamic C++ binaries forces core Lemonade maintainers to author, compile, and maintain all integration logic.
- *Maintainer Burden & Release Cycles:* In-tree C++ backends force maintainers to package, pin, and update complex binaries for every platform/GPU combination. When upstream engines release new features or model architectures, users are locked into wait cycles until maintainers update bundled binaries. Out-of-process descriptors allow users to immediately run distro-packaged, vendor-provided, or containerized backends.
- *Testing & Maturation Friction:* Out-of-process descriptors eliminate C++ compilation overhead for testing, enabling community members to quickly prototype, share, and mature new backend recipes.
- *Security & Process Isolation:* In-process binary modules execute inside `lemond`'s memory space, bypassing `nono`/kernel sandboxing and gaining full server privileges. Out-of-process execution guarantees that `nono` (Landlock/seccomp) isolates backend subprocesses at the OS process boundary.
- *Server Stability:* A crash, segmentation fault, or memory leak in a third-party backend subprocess cannot take down `lemond` or disrupt other running model slots.
- *Open Protocol, Closed Surface Boundary:* Lemonade's public API surface is closed—a finite set of capability contracts (`completion`, `embeddings`, `transcription`, `image`, `tts`, `reranking`). Descriptors restrict their scope to launch-time parameter token substitution and fixed HTTP passthrough. If an exotic backend engine requires custom per-request payload transformations, non-standard response framing, or bespoke protocol translation, it lies beyond the boundary of JSON descriptors and graduates to a built-in C++ `WrappedServer`. This prevents JSON descriptors from degenerating into an embedded Turing-complete scripting engine.
- *Path to Built-in Status:* Adopting declarative JSON descriptors for out-of-tree plugins does not remove the path for a backend to become a built-in C++ server. Descriptors provide a low-friction entry point for incubating community backends; high-adoption engines can graduate to built-in status if core maintainers choose to maintain them in-tree.
- *Out-of-Process Compiled Binary Plugins (Future Evolution):* Rejecting *in-process* `dlopen` shared libraries does not preclude native C/C++ compiled binary plugins. A future RFC can define a stable C-header ABI / IPC protocol for compiled binary plugins, provided they run as isolated out-of-process subprocesses under the same `nono` kernel sandbox boundary. This accommodates developers desiring a native C++ plugin development workflow while preserving server crash isolation and kernel sandboxing invariants.

### Example Backend Descriptor (`llamacpp`)

Below is an example JSON descriptor illustrating how an out-of-process backend binary (such as `llama-server`) declares its capabilities, slot policy, single command line string template, health probe, stop command, environment variables, and sandbox restrictions:

```json
{
  "recipe": "llamacpp-custom",
  "display_name": "llama.cpp (Custom External)",
  "binary": "llama-server",
  "capabilities": ["chat_completion", "completion", "embeddings"],
  "slot_policy": "standard",
  "uses_ctx_size": true,
  "health_probe": {
    "type": "http",
    "endpoint": "/health",
    "expected_status": 200,
    "timeout_seconds": 60,
    "poll_interval_ms": 200
  },
  "platform": {
    "linux": {
      "vulkan": {
        "command": "{binary} -m {checkpoint:main} -c {ctx_size} -t {threads} --port {port} {custom_args}",
        "stop_command": "kill -9 {pid}",
        "env": {
          "GGML_VK_VISIBLE_DEVICES": "{custom:vk_device:-0}"
        },
        "sandbox": {
          "read_paths": ["{hf_cache}", "{binary_dir}"],
          "write_paths": ["{cache_dir}/scratch"],
          "devices": ["/dev/dri"],
          "allow_network": false
        }
      },
      "rocm": {
        "command": "{binary} -m {checkpoint:main} -c {ctx_size} -t {threads} --port {port} -ngl 99 {custom_args}",
        "stop_command": "kill -9 {pid}",
        "env": {
          "HIP_VISIBLE_DEVICES": "{hip_visible_devices}"
        },
        "sandbox": {
          "read_paths": ["{hf_cache}", "{binary_dir}"],
          "write_paths": ["{cache_dir}/scratch"],
          "devices": ["/dev/dri", "/dev/kfd"],
          "allow_network": false
        }
      }
    }
  }
}
```

### 2. Permission Model & Sandboxing

Backend subprocesses run inside an OS kernel sandbox where available (Linux `nono` via Landlock/seccomp; Windows AppContainer/Job Objects and macOS Seatbelt are planned cross-platform enforcement milestones).

- **Least Privilege:** Read-only access to model weights and install paths; read-write access restricted to scratch/log directories.
- **Device & Network Control:** Device nodes (`/dev/dri/*`, `/dev/kfd`, NPU nodes) and outbound network egress must be explicitly declared in the descriptor.
- **Consent vs Enforcement:** The descriptor's declared policy is shown to the user during installation (consent review). Enforcement is applied at process launch by the kernel sandbox (`nono`). Container recipes operate under container runtime permissions.
- **Enforcement Modes:** Configurable via `config.json` (`auto`, `enabled`, `disabled`), defaulting to `auto`.

### 3. Backend Recipe Marketplace

A mechanism to discover and install backend descriptors and default recipes.

- **Git-Backed Index:** Marketplaces are Git repositories addressed as `org/repo[/subdir]` (GitHub REST API-first, requiring no local `git` CLI).
- **Pinned & Idempotent:** Installation pins to a repository git ref and verifies a SHA-256 content hash of the fetched descriptor. Updates require explicit re-installation.
- **No Binaries:** Marketplaces distribute plain JSON descriptors and recipe configurations, never executable binaries or model weights.

### 4. Client Interfaces (CLI & GUI)

- **CLI Commands:**
  - `lemonade backends` — List installed and running backends, slot status, and sandbox state.
  - `lemonade backends install <path|repo/recipe>` — Install a local descriptor file (`<path>`) or fetch and install a remote marketplace recipe (`<repo/recipe>`).
  - `lemonade backends uninstall <name>` — Unregister and remove an installed backend recipe.
  - `lemonade backends sandbox status` — View active sandbox profiles and enforcement modes.
- **GUI:**
  - Marketplace browser in the Tauri Desktop App and Web App.
  - Permission consent dialog displaying declared paths, device nodes, and network access before enabling a third-party backend.
  - Per-backend sandbox status and resource usage indicators.

---

## Roadmap & Milestones

> Sequential high-level phases; RFCs refine specifics.

### Phase 1: Plugin Specification & External Adapter Architecture

- [ ] RFC for the **Backend Plugin Specification** (descriptor schema, OS × device matrix, capability mapping, lifecycle).
- [ ] `ExternalBackendServer` class in `lemond` wrapping out-of-process backends via HTTP/sockets while honoring `WrappedServer` interfaces.
- [ ] `ModelManager` / `Router` discovery and validation of local descriptor files in `backends/` directories.
- [ ] `lemonade backends` CLI commands (`backends`, `backends install <path>`, `backends uninstall <name>`).
- [ ] Registration-time host-OS compatibility validation.

### Phase 2: Process Sandboxing Core (`nono`) & Permission Policy Engine

- [ ] RFC for the **Sandbox Architecture & Permission Policy Schema**.
- [ ] `NonoSandbox` engine for binary backend subprocesses (Linux Landlock filesystem restrictions, seccomp-bpf system call filtering, device node rules).
- [ ] Cross-platform binary process sandboxing (Windows AppContainer / Job Objects; macOS Seatbelt / App Sandbox).
- [ ] Permission manifest parser and configurable enforcement modes (`auto`, `enabled`, `disabled`) in `lemond`.
- [ ] Sandbox telemetry in `/system-info` and `lemonade backends sandbox status`.
- [ ] **Extension Evaluation:** Evaluate container backend security posture (container runtime isolation vs. rootless Podman namespaces vs. `nono` container profile wrappers). Determines hand-off to proposed Containerized Backends WG.

### Phase 3: Marketplace Infrastructure & Recipe Sharing

- [ ] RFC and schema for the **Recipe Marketplace Registry** (GitHub `org/repo[/subdir]` index, ref + SHA-256 content-hash pinning, descriptor validation).
- [ ] API-first fetching mechanism (GitHub REST API) without local `git` binary dependencies.
- [ ] CLI marketplace command (`lemonade backends install <repo/recipe>`).
- [ ] Content-hash verification and permission consent flow during marketplace installation.
- [ ] Community recipe publishing guide.

### Phase 4: GUI, Hardening & Ecosystem

- [ ] Backend manager and marketplace UI in Tauri and Web apps.
- [ ] Permission consent modal in GUI displaying declared paths, devices, and network permissions.
- [ ] Sandbox status indicators per running backend.
- [ ] Port a built-in backend to descriptor format to validate the plugin architecture.
- [ ] CI regression suite verifying kernel sandbox enforcement.
- [ ] Developer guide: *Building, Publishing, and Using a Lemonade Backend Recipe*.

---

## Governance & Communication

- **Discord Channel:** `#wg-backend-plugins`
- **GitHub Label:** `wg-backend-plugins`
- **Meeting Cadence:** Bi-weekly working group sync (published on Discord).
- **PR Review:** Proposals for specifications, sandbox policies, or marketplace schemas require approval from the Working Group Lead and a core maintainer.
