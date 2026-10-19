# Configured backends: platform qualification

## Qualified

**Linux, CPU only.** The `lemond`, `lemonade`, `cpp-ci-tests` and `web-app`
targets build with GCC, and the full `cpp-ci` set passes. The configured
backends are exercised with CPU fake engines over loopback HTTP: launch argv,
environment, readiness, watchdog, cancellation, error envelopes, operation
bindings and the public handlers. No model weights, inference, benchmarks, GPU
or NPU work are part of this qualification.

`WindowsArgsTest` checks the platform-neutral Windows argument serializer
(`utils/windows_args.{h,cpp}`) against a hand-authored literal matrix
(`test/cpp/windows_argv_cases.h`) on every platform, including Linux. That
proves the serializer's output bytes, not native Windows process launch.

## Not qualified

- **Windows.** Nothing has been compiled or executed natively. Unverified:
  `CreateProcessW` launch with a UTF-16 command line, cwd and sorted
  environment block (`spawn`, `run_with_output`, `run_command`);
  `WindowsProcessTest` and its `wmain` echo child; embedded
  `LemonadeServer.exe` startup from a GUI with no console, tray-to-server
  shutdown and job-object child cleanup; catalog and sentinel fixtures under the
  account's real symlink/reparse-point permissions. Known open Unicode ingress
  boundaries: `launch_planner.cpp` builds loader/cwd paths with
  `std::filesystem::path(executable).parent_path().string()` and uses narrow
  path construction for the absolute-checkpoint fallback; `backend_effects.cpp`
  uses `std::filesystem::exists(string)` and narrow `getenv("PATH")`; the
  standalone server and CLI parse narrow `main` arguments (the tray converts
  wide argv explicitly).
- **macOS.** Nothing has been compiled or executed natively. Unverified:
  `posix_spawnp` launch, the `fork`/`execvp` capture path, `is_running` with the
  SDK's `waitid(WNOWAIT)` (the `kill(pid, 0)` fallback cannot distinguish an
  unreaped zombie), and tray discovery without embedded-server ownership.
- **Debian system-module packaging** (`USE_SYSTEM_NODEJS_MODULES=ON`). Not run
  on a provisioned Debian host. The web-app build requires the Node
  Build-Depends from `contrib/debian/control` (webpack, webpack-cli,
  html-webpack-plugin, ts-loader, TypeScript, css-loader, style-loader, React,
  React DOM, highlight.js, markdown-it, markdown-it-texmath, buffer, process)
  plus `libjs-katex` and `fonts-katex`.
- **Native Tauri** webview behavior and distribution packaging on any OS.

## Native-host checklist

Use a clean checkout and the project's normal configured dependencies. Keep any
existing server untouched. All process tests below use CPU fakes; do not
substitute real models or accelerator backends. Record compiler/SDK and OS
versions and all raw test counts.

Windows, PowerShell, from a configured VS 2022 checkout (use `vs18` for VS 2026):

```powershell
cmake --build --preset windows --target lemond lemonade LemonadeServer cpp-ci-tests web-app
ctest --test-dir build -C Release --output-on-failure -R '^(WindowsArgsTest|WindowsProcessTest)$'
ctest --test-dir build -C Release --output-on-failure -L cpp-ci
```

Run the native argv test on a machine whose ANSI code page is **not** UTF-8;
changing only the console's `chcp` does not establish this. Then:

1. Use separate temporary cache/config directories and a dynamically selected
   free loopback port. Start standalone `lemond` with the CPU echo/fake fixture,
   then repeat with `LemonadeServer.exe` started from the desktop with no parent
   console. Never run the embedded and standalone variants concurrently against
   one single-instance identity. Capture startup and shutdown logs.
2. Put `backends.d`, executable, model/checkpoint and cwd under Unicode paths with
   spaces; use serialized JSON, empty arguments, quotes and trailing backslashes
   from the literal matrix. Observe the child argv/cwd/environment and compare
   exact values. Test CLI Unicode options, HTTP-provided Unicode and
   GUI-started tray input separately.
3. Verify readiness, one fake buffered response and one fake streamed response;
   normal unload; child early exit plus exit code/reap; failed readiness cleanup;
   repeated load/unload; shutdown while a fake request is active; and no surviving
   child/handle after parent/tray exit. Exercise filtered and inherited output,
   including missing console handles.
4. Run catalog/sentinel filesystem fixtures with the account's actual symlink/
   reparse-point permissions and record any skipped permission-dependent cases.
   Resolve the planner/filesystem/PATH encoding boundaries above before
   claiming end-to-end Unicode qualification.

macOS, from an already configured default build:

```sh
cmake --build --preset default --target lemond lemonade cpp-ci-tests web-app
ctest --test-dir build --output-on-failure -R '^WindowsArgsTest$'
ctest --test-dir build --output-on-failure -L cpp-ci
```

Repeat the CPU fake argv/cwd/environment, readiness/failure/early-exit/unload/
reap/shutdown lifecycle checks against native `posix_spawnp` and the
`fork`/`execvp` capture path, including Unicode paths, symlinks and the SDK's
`WNOWAIT` behavior. `WindowsProcessTest` must not be registered on macOS. Where
the tray is built, verify it discovers the separate server and does not acquire
embedded-server lifecycle ownership.

Debian, on a host with the `contrib/debian/control` Build-Depends installed:

```sh
cmake \
  -DAPP_SOURCE_DIR="$PWD/src/app" \
  -DWEB_APP_SOURCE_DIR="$PWD/src/web-app" \
  -DWEB_APP_BUILD_STAGING_DIR="$PWD/build/system-web-app/staging" \
  -DWEB_APP_BUILD_DIR="$PWD/build/system-web-app/output" \
  -DWEB_APP_STAMP="$PWD/build/system-web-app/web-app.stamp" \
  -DUSE_SYSTEM_NODEJS_MODULES=ON -P src/web-app/BuildWebApp.cmake
```

The build must resolve every module from the system Node paths with no npm
install, then serve the resulting `/app` against a running `lemond`.
