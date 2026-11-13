# Container activation on the rebased branch

The activation starts from `a2d90783bb635e7f1fda2ae46997ea7bb91e204e`
on the isolated `rebase-squash-tmp` worktree. The original
`integration/configured-backends` reference remains at `9af81abfc`.

## Fresh baseline

On 2026-11-11 the registered worktree directory was absent, while its saved
index matched `a2d90783b` exactly. Its tracked files were restored with
`checkout-index`; no branch was moved. There was no existing `build/` to reuse.

`cmake --preset default` and
`cmake --build --preset default --target lemond lemonade cpp-ci-tests -j 8`
completed, including all independent historical control builds.
`ctest --test-dir build -L '^cpp-ci$' --output-on-failure` passed **203/203**
in 405.70 seconds. The tracked source remained unchanged during these checks.
The immutable C9 stages and the upstream absorption stage replayed as part of
that baseline.

Evidence is under `.synagent/artifacts/container-activation-20261111/` in this
worktree: `configure-baseline-host.log`, `build-baseline.log`, and
`ctest-baseline.log`. Temporary files and independent controls are under this
worktree's `.synagent/scratch/`; no task artifacts were written to `/tmp`.
The first sandboxed configuration could not resolve GitHub; the host retry
fetched the pinned build dependencies successfully.

The pre-existing `lemond` PID 2100, started on November 9 and listening on
port 8000, was observed without making requests to it or changing it.

## Active definitions and the disclosed transition

The six builtin container leaves now have fixed leaf/hook contracts and the
independently reviewed execution owners. The active set is eight v2 leaves
(the two existing llama.cpp/HRX leaves plus six container leaves) with six
container execution owners. There are twenty public recipes and twenty-two
registrations; Halo Box and Nathanw remain private selectors of `llamacpp`.
The external export registry remains unchanged.

`ds4:rocm` now selects the OCI definition `ds4_rocm` and its dedicated
`backend_versions.ds4_rocm.rocm` pin. All ten OCI inventory rows already match
`server_models.json` exactly; no model rows were changed. The native DS4 source
and its `b0001` pin remain available to the independent native oracle.

The normal backend boilerplate generator regenerated the defaults and support
pages, and its `--check` pass succeeds. Fixed container backend validation now
checks supported hardware without treating the fixed backend as a public
selector. Absolute local checkpoints are accepted for container registrations.
Installation/status lookups select the actual llama container spec. Container
ports use the dynamic range, and the launch passes only the numeric render GID.
Container HOME and TMPDIR use `/var/tmp` inside Podman's writable image layer.

The new `six-container-activation` disclosure contains **167 records**, advances
runner/service policy **20 to 21**, and retains the exact **203-test** census.
All **22 prior stages** replay in independent pinned controls. No historical
fixture was changed, and every snapshot field remains in the literal comparison.
The new fixture's SHA-256 is
`4772cd25f188c039785b0e8b5368cb6a0d9aff632d0b1b076b3aa9b19a166460`.
Its complete CLI/HTTP payloads require 832,357 bytes; the pre-commit size
allowlist names only that new fixture alongside the existing vendored bundle.
The transition comparator has no exception.

The final complete CPU pass was **203/203**, in **81.30 seconds** with four
CTest workers. Evidence: `transition-author.log`, `transition/`, `ctest-final.log`,
and `generator-check.log` under this worktree's activation artifact directory.
The earlier failed expectation checks are retained beside the corrected passes.

## GPU runtime qualification

Activation source is committed as `9ce15e416`. Each run used this worktree's
`lemond` and `lemonade`, an isolated cache/configuration directory, a loopback
ephemeral port, and bounded requests. The existing port-8000 server received no
requests. Nathanw ran first. Images were pulled by the existing immutable pins;
the earlier registry validation was used without repeating registry discovery.
Local Podman inspection matches every requested digest:

| Engine | Requested SHA-256 digest | Image state |
| --- | --- | --- |
| Nathanw | `41018261e61b7e32fb38e4985ba569d0837804e727dcc62689c2fb1d73ec8f15` | Already present |
| Halo Box | `015fddafc28bfdb1653a99905010e83a9327f072206cb1f882f1f9f922069f48` | Pulled |
| ROCmFPX | `4df326e907a469f4acc6091fa7fb68fb275c2d5a7136bacd8e47e7339a5c49f7` | Pulled |
| Gufo | `2d7e9995ff2dfd7035f610a419b4f38d5d1604c0c7ab7f3b81415ad6f3e8db8b` | Pulled |
| Halogen | `7ab5c3dfd77acb2387603f1807069cac3494e678f413c0670745ba2df74c0f0e` | Pulled |
| OCI DS4 | `31b4e7e3584684768ee060e3dfeb62c1d716219da468090f3483977fdf6dfb47` | Pulled |

Halo Box and Gufo have additional platform-child digests in `RepoDigests`;
the requested OCI index digests are present as well. The combined inspection
is `all-image-digests.json` in the activation evidence directory.

All three successful chat paths referenced
`Qwen3-0.6B-Q4_K_M.gguf` directly from the existing Hugging Face cache:
`/home/ilintar/.cache/huggingface/hub/models--unsloth--Qwen3-0.6B-GGUF/snapshots/50968a4468ef4233ed78cd7c3de230dd1d61a56b/Qwen3-0.6B-Q4_K_M.gguf`.
`lemonade pull` registered this local checkpoint without copying it. The mount
uses its canonical existing blob with `:ro,z`.

The prompt was `What is the capital of Poland? Answer in one short sentence. /no_think`.
Each response was checked and read `The capital of Poland is Warsaw.`
Each request reported 29 input and 8 output tokens. Rates below are the server's
single short-request diagnostic, not a performance comparison.

| Engine | `lemond` / backend ports | Load ready (HTTP 200) | Reported tokens/s | Actual GPU evidence |
| --- | --- | ---: | ---: | --- |
| `llamacpp:nathanw` | 54875 / 53414 | 2.479 s | 207.70 | In-container Vulkaninfo: RADV STRIX_HALO; llama-server PID 3918581 held renderD128; 29/29 layers offloaded to Vulkan0 |
| `llamacpp:halobox` | 48219 / 61401 | 2.529 s | 268.61 | In-container rocminfo: gfx1151; llama-server PID 3920994 held renderD128 and KFD; selected ROCm0 |
| `rocmfpx:rocm` | 53417 / 59820 | 2.401 s | 229.70 | In-container rocminfo: gfx1151; llama-server PID 3943459 held renderD128 and KFD |

Nathanw's reviewed Vulkan policy maps only `/dev/dri`, so no KFD mapping or
holder is claimed for it. For all three, unload returned HTTP 200, the test
server exited, `podman ps -a` was empty, the labelled private network was removed,
and the added device holders disappeared. Rootless Podman emitted
`rootless netns: kill network process: permission denied` during teardown;
post-teardown inspection nevertheless found only the default Podman network
and no containers or KFD holders. The existing desktop render users remained.

An extra non-streaming client-disconnect probe passed on Halo Box: the backend
was idle within 0.104 s after the client timeout. Nathanw promptly received
Lemonade's upstream HTTP-transfer abort, but continued computing beyond the
two-second harness limit until unload. Its `summary.json` intentionally records
`success: false` for that extra assertion, despite the successful readiness,
chat and cleanup. Nathanw backend cancellation remains unqualified. ROCmFPX
was not subjected to this additional cancellation probe.

Gufo did not complete a model load. Both Qwen3-0.6B-Q4_K_M and the existing
`/devel/models/Qwen_Qwen3.5-9B-IQ4_XS.gguf` were rejected by the pinned image with
`No text provider found for model architecture`. Both bounded trials cleaned up;
there is no Gufo inference proof. No complete matching HGN/Halogen or DS4 GGUF
checkpoint was found in the three permitted stores, so Halogen and OCI DS4
have image/pin evidence but no model-inference proof. The inventory result is
`special-model-inventory.json`. Packaging and broader crash/cancellation gates
from the C9 checklist remain outside this measured qualification.

## Launch confinement and host state

Actual Podman `CreateCommand`, network inspections and process status are saved
for each successful run. They show `--cap-drop=all`, no-new-privileges, default
seccomp (process Seccomp 2), effective capabilities zero, `--pull=never`, an
internal labelled network, loopback publication, read-only `:ro,z` model mounts,
and `stop --time 10` teardown. No video group was passed. OCI DS4 retains its
reviewed explicit `SYS_PTRACE` addition after cap-drop; it was not launched and
is not included in the zero-effective-capability runtime claim.

Launch differences from the staged manager are disclosed:

- Rootless `keep-groups` was replaced by the sole numeric render GID (990 here).
  Inside the container the supplemental group is 990; on the host it maps to
  100989. Device access on this host relies on the existing `ilintar:rw-` ACLs
  on both renderD128 and KFD. No ACL was modified. A host relying only on the
  host render supplemental group is not qualified by these runs.
- Container backend ports now start in the dynamic range, instead of 8001.
  The first Nathanw smoke at 8001 is retained in
  `nathanw-initial-port-8001/`; the final reported Nathanw proof uses 53414.
- HOME and TMPDIR are `/var/tmp` inside the container's writable layer, without
  an added host mount. All authored logs, builds, temporary data and evidence
  are retained under this worktree's `.synagent/` directories.

`final-host-state.json` records empty `podman ps -a`, only the default network,
no KFD holders, the unchanged desktop render holders, and the device ACLs.
PID 2100 still started at `Mon Nov 9 17:50:07 2026` and still listens on port
8000. No push or public action was performed; `integration/configured-backends`
was not moved and the main checkout remained on `pr-4097`. GPU model work is
finished; the caller retains responsibility for releasing its existing grant.
