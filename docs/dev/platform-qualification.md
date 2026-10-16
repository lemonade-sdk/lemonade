# Configured backends: platform qualification

Baseline: `integration/configured-backends` at
`d29c50af0c1b75db3996f9f7272f896f199dc2f3`. Qualification host: Linux,
2026-10-16. CPU only; no models, inference, benchmarks, GPU or NPU work.

## Mechanism and implementation plan (recorded before code changes)

The existing `escape_windows_arg` wraps arguments in quotes but only doubles a
backslash when the next character is a quote. It leaves trailing backslashes
undoubled: the last backslash escapes the closing quote. With two backslashes
before an embedded quote it emits four backslashes followed by a quote, which
decodes as two backslashes and a delimiter, instead of preserving the literal
quote. In general it emits `n+2`, rather than `2n+1`, backslashes before an
embedded quote (the single-backslash case happens to work).

The replacement is a pure UTF-8 byte-string function, compiled on every platform.
It implements the inverse of the MSVC CRT rules for arguments after `argv[0]`:

1. Open and close every argument with a double quote, including the empty string.
2. For a run of `n` backslashes before a literal quote, emit `2n+1` backslashes
   and the quote. The parser produces `n` backslashes and a literal quote.
3. For a run of `n` backslashes at the end, emit `2n` backslashes before the
   closing quote. The parser produces `n` backslashes and a delimiter.
4. Preserve backslashes elsewhere and all other bytes. A bare embedded quote is
   case 2 with `n=0`. Unicode UTF-8 bytes pass through unchanged.

`argv[0]` has special CRT rules; valid executable filenames cannot contain a
double quote or end in a directory separator. Quoting the executable therefore
preserves its path and spaces. Keep a null application-name parameter to retain
the existing executable search behavior, using a quoted UTF-16 program token.

Both `spawn` and `run_with_output` currently use `STARTUPINFOA` and
`CreateProcessA`. The inherited environment is fetched with
`GetEnvironmentStringsW`, converted into UTF-8 bytes and supplied as an ANSI
block. Thus executable, arguments, cwd and explicit environment are code-page
dependent. Replace these with strict UTF-8-to-UTF-16 conversion, writable wide
command lines, `STARTUPINFOW`, `CreateProcessW`, and a sorted, double-NUL-ended
UTF-16 environment block with `CREATE_UNICODE_ENVIRONMENT`. Preserve inherited
wide environment entries, including drive-current-directory entries, and merge
overrides using case-insensitive ordinal comparison. Convert before allocating
pipe handles. Convert the shell-command capture launch to the wide API too,
without applying CRT quoting to the shell expression.

No `activeCodePage` is declared in `lemonade.manifest.in`. The wide APIs remove
this dependency for child launch; leave the manifest unchanged to avoid a
process-wide change to unrelated ANSI APIs. This does not convert a backend's
own narrow `main` or output encoding to Unicode.

Add a hand-authored literal expectation matrix and register it in `cpp-ci` on all
hosts. Add a Windows-only `wmain` echo child and parent test that exercises both
real process launch APIs, Unicode executable/cwd/environment, argument boundaries,
and process exit/cleanup. These native tests cannot be compiled or executed on
this Linux host. Leave POSIX implementations behaviorally unchanged.

The requested 2026-10-15 10:04 report has sections 6 and 11, not 13; its section
11 retains the prerequisite from section 13 of the 2026-10-14 design. Both were
read along with sections 5/7 of the 2026-10-15 19:31 report.

Sources: [MSVC CRT argument parsing](https://learn.microsoft.com/en-us/cpp/cpp/main-function-command-line-args?view=msvc-170),
[CreateProcessW](https://learn.microsoft.com/en-us/windows/win32/api/processthreadsapi/nf-processthreadsapi-createprocessw),
[Windows environment blocks](https://learn.microsoft.com/en-us/windows/win32/procthread/changing-environment-variables).

## Fixed code and evidence boundary

| Area | Change | Evidence on this host |
| --- | --- | --- |
| CRT argument serialization | `utils/windows_args.{h,cpp}` provides `lemon::utils::escape_windows_arg` independently of Windows headers. | Linux GCC 15.2.0, C++17; literal matrix **19 passed / 0 failed**. |
| Regression sensitivity | Compile the unchanged baseline helper against the same hand-authored expectations. | **11 passed / 8 failed**, including trailing backslashes, runs before embedded quotes, and Unicode plus a trailing backslash. |
| Windows spawn | UTF-16 command line (including executable token), cwd and explicit environment; `STARTUPINFOW` / `CreateProcessW`; writable `wstring::data()`. | Source audit only. **Not compiled or executed here.** |
| Windows output capture | `run_with_output` uses the same escaper and UTF-16 launch/cwd. Its null environment pointer inherits the parent's environment directly. | Source audit only. **Not compiled or executed here.** |
| Windows shell capture | `run_command` converts the complete existing `cmd /c` expression to UTF-16, retaining shell syntax. | Source audit only. **Not compiled or executed here.** |
| Environment merging | Preserve inherited wide entries and drive entries, merge explicit UTF-8 overrides after conversion, sort names case-insensitively using `CompareStringOrdinal`, terminate with two wide NULs, set `CREATE_UNICODE_ENVIRONMENT`. | Source audit only. **Not compiled or executed here.** |
| Manifest | Unchanged. No active-code-page declaration is needed for the explicit wide launch APIs. | Does not establish UTF-8 support in other ANSI APIs or a backend's own narrow CRT entry point. |
| POSIX implementations | No edits to `process_linux.cpp` or `process_macos.cpp`. | Linux regression below; macOS **not compiled or executed**. |

The matrix in `test/cpp/windows_argv_cases.h` contains independent literal input
and expected-output strings. It covers empty/plain/whitespace arguments, a lone
quote, embedded and adjacent quotes, a single backslash, an interior run,
trailing single/run backslashes, one/two/three backslashes before a quote,
two backslashes followed by a quote, a quote plus a trailing backslash, serialized JSON, JSON with escaped
quotes, Unicode, and Unicode plus a trailing backslash. Expectations are not
constructed with the production function or a second quoting algorithm.
`WindowsArgsTest` is registered in `cpp-ci` on every platform, and both the server
core and standalone CLI compile the new source.

`test/cpp/windows_argv_echo_child.cpp` and `test_windows_process.cpp` are guarded
by `_WIN32`, with their CMake targets and `WindowsProcessTest` inside `if(WIN32)`.
The child uses **`wmain`**, converts observed UTF-16 values to UTF-8 and writes
ASCII-safe JSON to both a file and stdout. The parent compares original input
values, not reconstructed command lines. It copies the executable into a path
containing spaces, non-ASCII BMP characters and a supplementary-plane character;
checks `argv[0]`, every matrix argument, Unicode cwd, inherited Unicode environment,
case-insensitive overrides and a Unicode environment name/value; exercises silent,
filtered and inherited-output spawn plus capture with/without stderr; and checks
exit, nonblocking reap, termination and handle reaping. A shell-capture check
redirects ASCII output to a Unicode filename. All children are CPU-only fixtures.
**No native test result is claimed.** Linux CTest discovery includes only
`WindowsArgsTest`, not `WindowsProcessTest`.

## Debian system-module attempt

**BLOCKED BY MISSING HOST PREREQUISITES.** The host is Ubuntu 26.04.1, not a Debian
packaging environment. No system packages were installed. The existing build
cache remains `USE_SYSTEM_NODEJS_MODULES=OFF`; the system-mode attempt used the
actual CMake build script with separate staging/output/stamp paths under `/tmp`:

```sh
cmake \
  -DAPP_SOURCE_DIR="$PWD/src/app" \
  -DWEB_APP_SOURCE_DIR="$PWD/src/web-app" \
  -DWEB_APP_BUILD_STAGING_DIR=/tmp/lemonade-system-qualification/staging \
  -DWEB_APP_BUILD_DIR=/tmp/lemonade-system-qualification/output \
  -DWEB_APP_STAMP=/tmp/lemonade-system-qualification/web-app.stamp \
  -DUSE_SYSTEM_NODEJS_MODULES=ON -P src/web-app/BuildWebApp.cmake
```

Staging, the system TypeScript configuration rewrite and the KaTeX overlay
completed. The script then returned **1** at `BuildWebApp.cmake:164`:
`webpack not found - USE_SYSTEM_NODEJS_MODULES=ON requires system webpack`.
There is no `/usr/bin/webpack` and no `webpack` on PATH. Loading the staged
Webpack config with `NODE_PATH=/usr/share/nodejs:/usr/lib/nodejs:/usr/share/javascript`
independently fails with `Cannot find module 'html-webpack-plugin'`.

System-only Node resolution and `dpkg-query` confirm the following packages from
`contrib/debian/control` are absent on this host:

| Required module/tool | Debian Build-Depends package |
| --- | --- |
| webpack | `node-webpack` |
| webpack-cli | `node-webpack-cli` |
| html-webpack-plugin | `node-html-webpack-plugin` |
| ts-loader | `node-ts-loader` |
| TypeScript | `node-typescript` |
| css-loader | `node-css-loader` |
| style-loader | `node-style-loader` |
| React | `node-react` |
| React DOM | `node-react-dom` |
| highlight.js | `node-highlight.js` |
| markdown-it | `node-markdown-it` |
| markdown-it-texmath | `node-markdown-it-texmath` |
| buffer | `node-buffer` |
| process/browser | `node-process` |

The distro `nodejs` package is also not installed, but local Node **v22.17.0** is
available and was used. `libjs-katex` and `fonts-katex` are installed
(`0.16.10+~cs6.1.0-5ubuntu1`); their JS, CSS and fonts exist at the paths the CMake
script expects. The bare system `require('katex')` probe fails, but the successful
CMake overlay supplies that module, so KaTeX is **not a missing prerequisite**.
The manifest also lists Axios, which is absent from system resolution, but no
current shared-renderer import uses it; it is not an additional demonstrated
bundle prerequisite. These observations establish a host dependency block, not a
failure of a fully provisioned Debian build.

The normal npm-mode `web-app` target succeeds. A separate actual production
webpack compilation from the existing CMake staging tree also succeeds with
webpack **5.106.2** in **9.583 s**, writing to the qualification artifact directory.
It reports four warnings: a dynamic dependency expression in the vendored model
viewer and three performance/size warnings. It reports no compilation errors.

## Source-level audit of the surrounding platform paths

The audit compares main `d9ac44641` with the combined baseline and inspects the
current code; it does not claim native runtime parity.

- **POSIX process launch:** the configured-backend stack did not modify either
  POSIX implementation. Linux spawn and capture pass an argv array to `execvp`;
  macOS spawn passes arrays to `posix_spawnp`, with cwd file actions and an
  explicit inherited/override environment. These paths do not use the Windows
  quoting helper. macOS capture retains its older `fork`/`execvp` route. Linux
  `chdir`/`setenv` return values are still unchecked, and macOS capture still uses
  `fork` despite the macOS spawn path's documented reason for avoiding inherited
  runtime state. These are existing limitations, not changed here.
- **macOS lifecycle:** `is_running` uses `waitid(...WNOWAIT)` when that flag is
  available and otherwise falls back to `kill(pid, 0)`. The fallback cannot
  distinguish an unreaped zombie. Native SDK compilation and exit/reap evidence
  are still required; no speculative lifecycle change was made.
- **Embedded Windows startup:** `src/cpp/tray/main.cpp` bootstraps the catalog
  before `ConfigFile::load`, matching standalone startup. This branch uses
  `wWinMain` and explicit wide-to-UTF-8 argv conversion. Server creation remains
  inside the Windows branch; Linux/macOS tray startup remains a client. The
  detached server thread, tray-to-server shutdown request and job-object child
  cleanup require native execution, including startup from a GUI with no console.
- **Guards and build wiring:** CMake selects exactly one process implementation
  for core and CLI; the pure helper has no OS dependency. New native child/tests
  are Windows-only and the new pure test has both CI label and aggregate-target
  dependency. Configured native OS and architecture selectors cover Windows,
  Apple, Linux and x64/ARM64 spellings; shared configured sources have no new
  unguarded Windows/POSIX API imports. Linux passed the compiled paths only.
- **UTF-8 filesystem handling that already uses the shared utilities:** catalog
  discovery constructs its root with `path_from_utf8`, sorts UTF-8 filenames and
  opens `ifstream` with filesystem paths. The sentinel resolver converts root
  and result using `path_from_utf8` / `path_to_utf8` and keeps the existing
  `hf_cache` iterator options. Windows reparse points and account-dependent
  symlink permissions still need native fixtures.
- **OPEN Windows Unicode ingress risks:** `launch_planner.cpp` uses
  `std::filesystem::path(executable).parent_path().string()` for loader setup and
  executable-directory cwd, and narrow path construction for the checkpoint
  fallback. `backend_effects.cpp` uses `std::filesystem::exists(string)` and
  narrow `getenv("PATH")`. Those are separate encoding boundaries before the
  new process layer; a UTF-8 launch API cannot repair already misinterpreted
  bytes. Standalone server/CLI use narrow `main`/CLI parsing, while the tray has
  explicit wide argv conversion. Native Windows tests must cover each ingress
  separately. Proposed follow-up: use the established path conversion utilities
  and a wide environment read where the native test demonstrates loss. No
  broader change is committed without that evidence.

## Linux regression

**PASS: 106/106 `cpp-ci` tests, 0 failures, 0 skips; 223.65 seconds (CTest real time).**
This is the previous 105-test Linux set plus `WindowsArgsTest`. The new test's
internal count is **19 passed / 0 failed**, and it also passed independently.
The baseline sensitivity run is **11 passed / 8 failed**.

```sh
cmake --build build --target lemond lemonade cpp-ci-tests web-app -j6
ctest --test-dir build --output-on-failure -L cpp-ci
./build/test_windows_args
```

All four build targets succeeded using the existing Release build directory.
The normal web target was already current; the additional production webpack
invocation described above actually compiled its staged sources. GCC is
15.2.0, Node is v22.17.0. CTest ran in default mode with all three configured
opt-in environment variables unset, isolated temporary XDG config/cache/data
and TMPDIR, `STATE_DIRECTORY` unset, and accelerator visibility empty. Loopback
permission was enabled for CPU fake HTTP tests. No model weights, inference,
benchmarks or accelerator runs occurred.

The protected port-8000 listener was only inspected using read-only `ss` and
`/proc`: PID **3582471**, start ticks **12126985**, executable
`/devel/tools/lemonade/build/lemond (deleted)`. It was not requested, signalled,
stopped, restarted or reconfigured. The rebuilt executable is a different
on-disk image from the one this established process is running.

Raw evidence is in `/tmp/lemonade-platform-20261016/`: `build.log`,
`build-final.log`, `ctest.log`, `ctest-result.json`, `windows-args.log`,
`old-args.log`, `npm-web-build.log`, `system-build.log`, `system-modules.log`,
and protected-server snapshots. These temporary logs are evidence from this
host, not portable native qualification artifacts.

## Native-host checklist (not executed here)

Use a clean checkout of the qualification commit and the project's normal
configured dependencies. Keep any existing server untouched. All process tests
below use CPU fakes; do not substitute real models or accelerator backends.

Windows, PowerShell, from a configured VS 2022 checkout (use `vs18` for VS 2026):

```powershell
cmake --build --preset windows --target lemond lemonade LemonadeServer cpp-ci-tests web-app
ctest --test-dir build -C Release --output-on-failure -R '^(WindowsArgsTest|WindowsProcessTest)$'
ctest --test-dir build -C Release --output-on-failure -L cpp-ci
$env:LEMONADE_THINKSOUND_CONFIGURED='1'
$env:LEMONADE_DS4_CONFIGURED='1'
$env:LEMONADE_RYZENAI_CONFIGURED='1'
ctest --test-dir build -C Release --output-on-failure -L cpp-ci
Remove-Item Env:LEMONADE_THINKSOUND_CONFIGURED, Env:LEMONADE_DS4_CONFIGURED, Env:LEMONADE_RYZENAI_CONFIGURED
```

Record MSVC/SDK versions, Windows version, system ANSI code page and all raw test
counts. Run the native argv test on a machine whose ANSI code page is **not**
UTF-8; changing only the console's `chcp` does not establish this. The helper uses
wide CRT argv and deliberately ASCII-safe reporting. Also validate any real
backend's narrow-argv UTF-8 handling separately when real-backend work is later
authorized.

Windows process qualification to close after the automated checks:

1. Use separate temporary cache/config directories and a dynamically selected
   free loopback port. Start standalone `lemond` with the CPU echo/fake fixture,
   then repeat with `LemonadeServer.exe` from the desktop with no parent console.
   Never run the two embedded/standalone variants concurrently against one
   single-instance identity. Capture startup and shutdown logs.
2. Put `backends.d`, executable, model/checkpoint and cwd under Unicode paths with
   spaces; use serialized JSON, empty arguments, quotes and trailing backslashes
   from the literal matrix. Observe the child argv/cwd/environment and compare
   exact values through both original and configured backend arms. Test CLI
   Unicode options, HTTP-provided Unicode, and GUI-started tray input separately.
3. Verify readiness, one fake buffered response and one fake streamed response;
   normal unload; child early exit plus exit code/reap; failed readiness cleanup;
   repeated load/unload; shutdown while a fake request is active; and no surviving
   child/handle after parent/tray exit. Exercise filtered and inherited output,
   including missing console handles. No inference is required.
4. Run catalog/sentinel filesystem fixtures with the account's actual symlink/
   reparse-point permissions and record any skipped permission-dependent cases.
   Resolve the separate planner/filesystem/PATH encoding findings above before
   claiming end-to-end Unicode platform qualification.

macOS, from an already configured default build:

```sh
cmake --build --preset default --target lemond lemonade cpp-ci-tests web-app
ctest --test-dir build --output-on-failure -R '^WindowsArgsTest$'
ctest --test-dir build --output-on-failure -L cpp-ci
LEMONADE_THINKSOUND_CONFIGURED=1 LEMONADE_DS4_CONFIGURED=1 \
  LEMONADE_RYZENAI_CONFIGURED=1 \
  ctest --test-dir build --output-on-failure -L cpp-ci
```

Record AppleClang/SDK/macOS version and CPU architecture. Repeat the CPU fake
argv/cwd/environment, readiness/failure/early-exit/unload/reap/shutdown lifecycle
checks against native `posix_spawnp` and capture `fork`/`execvp`, including Unicode
paths, symlinks and the SDK's `WNOWAIT` behavior. The Windows child test must not
be registered on macOS. Where the tray is built, verify it discovers the separate
server and does not acquire embedded-server lifecycle ownership. Tauri webview
and UI behavior remain separate native evidence; Linux bundling cannot close it.


## Git delivery

All implementation, tests, build wiring and this report are committed locally on
`integration/configured-backends`, starting from `d29c50af0c1b75db3996f9f7272f896f199dc2f3`.
The commit identity is supplied in the final delivery and shared activity log;
`git log -1 --oneline` identifies the commit containing this report. Final
`git status --short --branch` must show only the branch line, with no changes.
No push, PR, issue, discussion or other public action was performed.
