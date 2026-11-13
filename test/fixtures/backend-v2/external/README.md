# llama.cpp external contract CPU fixtures

`llamacpp_external_contract.h` is the separate contract-1 manifest. Its source,
resolved definition and dependency pins select the embedded llama base. The
literal policy, owner-graph, per-slot mutation and registry pins in
`llamacpp.pins.json` are checked before test authority is constructed. The
registry digest also pins all installed ABI metadata. Changes to those pins
require review of the complete manifest and its admission failures.

`research-llama.json` is a local executable fork. An administrator supplies its
absolute executable path through `research-llama.cpu_bin` in the ordinary server
configuration. Its stable option IDs remain `backend`, `device`, and `args`, with
leaf-owned public names and no named CLI flags. The declaration removes the four
optional root hooks and selects the exact external-only install/launch profile.
`fork-b.json` owns a second set of names and removes both responses owners. The
grants are 17 at the base, 13 at research-llama, and 10 at fork-b.

The noninstalled `test_llamacpp_external_fork` executable selects the audited
registry at the builtin freeze dependency boundary. The common freeze retains
the real embedded sources, fixed builtin policies, canonical descriptor seeds,
reservations, defaults and pins. The installed export singleton remains empty.
The Python driver checks production symbols and compile definitions, and runs
an uninjected `lemond` process against the identical discovered fork bytes.

`LlamaCppExternalForkTest` launches the separate fake-engine executable through
the selected factory entry and `NativeProcess`. It checks independent argv,
child environment, request and response expectations, executable SHA-256 and
PID, missing executable/configuration, invalid arguments, readiness failure and
teardown. This is CPU qualification with fake artifacts and loopback HTTP.
Native platform builds, real models and accelerator qualification belong to 4h.
Installing this contract into production requires separate reviewed activation
and requalification against that production registry.
