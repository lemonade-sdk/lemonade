# Project Context

- **Project:** lemonade
- **User:** Kyle Poineal
- **Created:** 2026-05-15
- **Role:** Build & Release — CMake, WiX, .deb/.rpm, macOS pkg, CI

## Core Context

Owns cross-platform packaging across MSVC / GCC / Clang / AppleClang.
Critical defender of invariant #12: web-app/desktop-app `package.json` split for Debian native packaging.

## Learnings

### 2026-05-15: Web-app build pipeline anatomy
- Single CMake target: `web-app` (CMakeLists.txt:981), ALL target, stamp-driven (`web-app.stamp`).
- Driven by `src/web-app/BuildWebApp.cmake` invoked via `cmake -P` from a custom command.
- **Staging step is mandatory.** `BuildWebApp.cmake` copies BOTH `src/app/` and `src/web-app/` into `${CMAKE_BINARY_DIR}/web-app-staging/{app,web-app}/` (robocopy on Windows, `cp -rL` elsewhere). Webpack runs from staged `web-app/` and reaches into `../app/src/renderer/index.tsx` for the shared renderer. NO OS symlinks committed — would break Windows checkouts without dev mode + core.symlinks=true.
- Webpack output → `${CMAKE_BINARY_DIR}/resources/web-app/` via `WEBPACK_OUTPUT_PATH` env var.
- `USE_SYSTEM_NODEJS_MODULES=ON` switches webpack's `resolve.modules` from `node_modules` to `/usr/share/nodejs:/usr/lib/nodejs:/usr/share/javascript` plus a katex overlay shim (`require('/usr/share/javascript/katex/katex.js')`) and CSS symlinked from `/usr/share/javascript/katex/katex.min.css`. In this mode CMake runs `webpack` directly (system binary); npm is never invoked. In OFF mode CMake runs `npm ci --ignore-scripts` then `npm run build`.

### 2026-05-15: Debian build path (the real constraint)
- `debian/rules` is 16 lines. It calls `dh $@ --buildsystem=cmake+ninja` and only overrides `dh_auto_configure` to pass `-DBUILD_WEB_APP=ON -DUSE_SYSTEM_NODEJS_MODULES=ON`.
- This means: **webpack DOES run at .deb build time**, against `/usr/share/nodejs`. There is no pre-built artifact pattern in place today. Anything the renderer needs MUST exist as a `node-*` Debian package and be listed in `debian/control` Build-Depends.
- Build-Depends today covers: node-buffer, node-css-loader, node-highlight.js, node-html-webpack-plugin, node-markdown-it, node-markdown-it-texmath, node-process, node-react, node-react-dom, node-style-loader, node-ts-loader, node-typescript, node-webpack, node-webpack-cli, plus fonts-katex and libjs-katex via the overlay.
- `markdown-it-texmath` is NOT in `src/web-app/package.json` but IS in `debian/control` and `src/app/package.json` — already a low-grade smell that the two trees diverge in Debian land.

### 2026-05-15: WiX / RPM / macOS constraints
- WiX (`installer/Product.wxs.in`): consumes the pre-built bundle from `resources/web-app/` via a generated fragment. Framework-agnostic — it cares about output files, not source language.
- macOS pkg, Linux RPM, generic `cpack`: same story. No additional npm-source constraints. Only Debian native packaging enforces "build from Debian-shipped modules."

### 2026-05-15: Pre-build-and-ship pattern viability
- Debian native packaging allows shipping pre-built data (it's done for libjs-* packages). What's NOT clean is committing a `dist/` blob to the upstream tarball that Debian's policy treats as a build artifact rather than source.
- If we adopt the pattern: `debian/rules` would stop running cmake's web-app target and instead `cp -r resources/web-app /usr/share/lemonade/`, with a separate CI job producing the bundle. Build-Depends for the renderer toolchain would disappear from `control`. Mario (the Debian maintainer in `control`) would need to approve — this is the kind of choice that warrants a salsa.debian.org thread before commit.
- Salsa CI (`contrib/debian/salsa-ci.yml`) currently runs the full webpack build. Same trade-off applies there.

### 2026-05-15: Framework-by-framework dep survey vs /usr/share/nodejs
- **React 19**: `node-react`, `node-react-dom` — present, currently used. ✅
- **Svelte 4/5**: `node-svelte` exists in Debian sid (libnode-svelte?) but coverage on stable (bookworm) is partial; `node-svelte-loader` (webpack loader) is NOT in Debian. Best-guess based on past Debian nodejs packaging patterns. ⚠️ Requires patching control or pre-build pattern.
- **SvelteKit**: `node-sveltejs-kit`, `node-sveltejs-vite-plugin-svelte`, `node-sveltejs-adapter-static`, `node-vite`, `node-rollup` — none of the kit-specific packages exist in Debian. Vite/rollup partial. ❌ Effectively requires pre-build pattern.
- **Flutter**: Dart SDK is NOT a Debian build-tool (no `dart` package in main). Build can't happen in Debian context regardless of Node. Only viable as pre-built static assets.

### 2026-05-15: Tauri embedding constraint
- The desktop binary (`lemonade-app.exe` / `.app` / `.AppImage` equivalent) is Tauri v2, which renders via the OS WebView. Any framework that compiles to HTML/JS/CSS (React, Svelte, SvelteKit, Flutter Web) drops in. Flutter Desktop does NOT — it ships its own native renderer and would replace, not augment, Tauri. Verdict: Flutter Desktop = out of scope for this POC.


---

### 2026-05-16 — OmniRouter UI gap + `lemonade import` precedent (flagged for future)

Two integration notes from the v1.3 UI POC research pass (see `decisions.md` entry `2026-05-16T14:30:00Z`):

1. **OmniRouter has near-zero UI onboarding right now.** Collections (Ultra / Lite, `recipe: "collection"` 4-model bundles) load via `ChatWindow.tsx` and `collectionModels.ts`, but no user-facing "this is OmniRouter, here is how it works" surface exists. Deferred from v1.3 scope as a separate workstream.
2. **`lemonade import` CLI already supports remote recipe fetch from the `lemonade-sdk/recipes` GitHub repo.** Relevant precedent if/when we wire a "Browse community presets" feature into the UI — the infrastructure (HTTP fetch + recipe-JSON parse + cache) is already there to model against.

Neither is on the current critical path; flagged here so the build/release/integration view has the context when these workstreams reactivate.

---

### 2026-05-16 — Verified CLI command syntax (PR #1914 review fixes)

While addressing review comments on the bug-report template, verified the current `lemonade` CLI surface against `src/cpp/cli/main.cpp`. Pinning these so future doc work doesn't re-research them:

- **`lemonade config set KEY=VALUE`** is the canonical way to change server config at runtime. Defined at `src/cpp/cli/main.cpp:1041-1044`. The subcommand uses `allow_extras(true)` to accept arbitrary `key=value` tokens. Example from help text: `llamacpp.backend=rocm port=8123`. Log level key is **`log_level`** (snake_case, NOT `log-level`) — confirmed in `src/cpp/server/runtime_config.cpp:351` (the setter that consumes the key) and `src/cpp/server/config_file.cpp:187` (env var mapping `LEMONADE_LOG_LEVEL` → `log_level`).
- **Hot reconfiguration is supported.** `reconfigure_application_logging()` exists in `src/cpp/server/logging_config.cpp:137` and is wired through `RuntimeConfig` setters — no server restart needed when the log level changes. Confirms Jeremy's review note: "No need to restart anymore."
- **`lemonade backends`** replaced `lemonade recipes` at some point before this snapshot. Defined at `src/cpp/cli/main.cpp:1031` as `app.add_subcommand("backends", "List available recipes and backends")`. Subcommands: `backends install`, `backends uninstall` (lines 1033-1034). The word "recipes" still appears in the help text *description* (showing both concepts) but the command name is `backends`.
- **Legacy `lemond --log-level X`** still works as a CLI flag at server-start time (`src/cpp/legacy-cli/main.cpp:176` maps it to `config_set_args.push_back("log_level=" + next())`). So both paths produce the same config key — but the `lemonade config set` HTTP path is preferred for a running server.

**Pattern for handling external maintainer review feedback:** Verify CLI command names and syntax in `src/cpp/cli/main.cpp` (and `runtime_config.cpp` for config keys) BEFORE updating docs/templates. The `add_subcommand` grep + the `config_["KEY"]` getters in `runtime_config.cpp` are the two reliable sources of truth — help-string wording inside the CLI sometimes lags behind renames (see "recipes" in the `backends` subcommand description above).

