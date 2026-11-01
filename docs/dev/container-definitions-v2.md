# Container declarations in v2 (C2)

V2 can resolve, structurally validate and project container declarations as data.
A fixed test-only builtin policy demonstrates catalog admission and compilation;
no production container grant is installed. External containers fail authorization,
including hook-free ones. C2 cannot bind builtin container data to an executable
definition. Binding returns
`container_execution_unavailable` at `/launch/execution`, with the diagnostic
`Container execution is unavailable in C2`. The existing native process factory
is unchanged. V1 does not gain any container fields.

The complete worked definition is
[`gufo.container.json`](../../test/fixtures/backend-v2/gufo.container.json).
It has **59 authored physical lines**, including braces and its final newline,
and is a fixture outside `src/cpp/resources/backends/*.json`. It is not embedded
in production. It describes the ROCm image command `gufo serve llm`, a required
GGUF artifact, served model name, port, `/ready`, buffered chat and streaming chat.
Its pin key deliberately need not exist in the production version file: C2 checks
the source record, and does not resolve, install or run an image.

The declaration uses this shape inside the v2 envelope's `merge`:

```json
{
  "kind": "container",
  "containers": {
    "rocm": {
      "repository": "ghcr.io/peonist-ai/gufo",
      "pin": {"source": "backend_versions", "recipe": "gufo", "backend": "rocm"},
      "devices": ["/dev/dri", "/dev/kfd"],
      "cap_add": [],
      "policy": "lemonade-isolated-v1",
      "network": "private_internal",
      "teardown": "stop_remove_reap",
      "stop_grace_seconds": 10
    }
  },
  "install": {"strategy": "oci"},
  "launch": {
    "execution": {"cases": [
      {"id": "rocm-container", "backends": ["rocm"], "kind": "container",
       "container": "rocm", "program": {"source": "image_command", "name": "gufo"}}
    ]},
    "mounts": [
      {"id": "main", "artifact": "main", "expand": "gguf_shards", "destination": "/mnt/models"}
    ],
    "address": {"source": "process_start"}
  }
}
```

This excerpt omits the ordinary metadata, artifacts, arguments, environment,
readiness, watchdog and operation records present in the complete fixture.

## Closed records

| Record | Domain and constraints |
|---|---|
| `kind` | Exactly `local` or `container`; v2 only |
| `containers` | At most 32 policies, keyed by backend identifier; every policy must be used |
| `ContainerPolicy.repository` | At most 256 characters; lowercase repository components beneath exactly `docker.io/kyuz0/` or `ghcr.io/peonist-ai/`; no `:tag`, digest, transport, port, credentials, whitespace or substitution |
| `ContainerPin` | Exactly `source: backend_versions`, identifier `recipe`, identifier `backend`; no tag, digest, fallback or inline default |
| `devices` | Unique subset of `/dev/dri`, `/dev/kfd`; at most two |
| `cap_add` | Unique subset of `SYS_PTRACE`; at most one |
| `policy` | Exactly `lemonade-isolated-v1` |
| `network` | Exactly `private_internal` |
| `teardown` | Exactly `stop_remove_reap` |
| `stop_grace_seconds` | Integer constant `10` |
| `launch.execution.cases` | 1–32 records with stable `id`; 1–32 unique backend identifiers per case; backend sets cannot overlap |
| Native execution case | `kind: native`, existing managed or external-or-managed program record; no container reference |
| Container execution case | `kind: container`, container policy key, `ImageProgram`; every backend in the case equals its policy key |
| `ImageProgram` | Exactly `source: image_entrypoint` with no name, or `source: image_command` and a 1–128-character command basename containing ASCII letters, digits, `.`, `_`, `-`, starting with a letter or digit; no host lookup or path |
| `launch.mounts` | At most 64 records with stable `id`, an existing artifact identifier, `expand`, `destination`, and optional existing boolean expression `when` |
| Mount expansion | Exactly `file`, `gguf_shards` or `directory_files` |
| Mount destination | Constant root `/mnt/models`; C3 must derive and collision-check final basenames from authorized artifact handles |
| `launch.address` | Exactly `source: process_start`; no host override |
| OCI install | Exactly `strategy: oci`; no native release cases, default versions or external binary fallback |
| Mixed install | Exactly `strategy: by_execution` and a `native` record using the native install vocabulary except `hook` |
| Readiness | Existing named port/path; container timeout is `global`, integer seconds 1–86400, or `{source: global, floor_seconds: 1..86400}`; path starts with `/`; watchdog uses the same port/path |

All records are closed. Existing source, depth, work, retained storage and
capped-diagnostic limits apply. Native install-case, execution-case and mount IDs participate in duplicate-ID
checking, provenance, ordered edits and immutable-identity rules. Policies use
their existing object keys as identities. Expressions retain the existing bounded
compiler and phase checks.

A `container` declaration requires OCI installation, explicit container-only
cases, mounts and a process address. It cannot contain a top-level managed program
or a native execution case. A `local` declaration may describe mixed native and
container cases with `by_execution`. A top-level program and execution cases are
mutually exclusive. Container support rows require matching explicit cases. No
implicit native fallback is introduced. Container launch environment and working
directory use `inherit`, avoiding native loader-path and staging behavior.

## Pin and mount ownership

A definition names a version-file key, not an image version. C3 must read the
effective configured `backend_versions.json` value and validate C1's
`<tag>@sha256:<64 lowercase hex>` syntax, including administrative overrides.
C1's `container_image` produces `repository@sha256:...` for installation and
launch; the tag remains update-channel metadata. There is no digest resolver,
mutable tag default or image installation in C2.

Mounts name the existing artifact inventory. No host path, volume syntax,
writable mode, arbitrary destination or recursive host traversal is accepted.
C3 must resolve handles, preserve approved HF symlink relationships, expand shards
and directory files, reject escaped sources and collisions, and rewrite whole
artifact-valued arguments to final container paths before constructing a run spec.
The confinement policy fixes read-only `:ro,z` access, capability drop,
no-new-privileges, default seccomp and labelled internal networking in trusted code.

`BackendDescriptor::containers` carries every policy field, including the pin
source, devices, capabilities, network and teardown. The compiled program keeps
explicit execution cases and mount records alongside its immutable normalized
declaration. Compilation grants no execution authority. External authorization rejects container
records independently of hook selection. Binding also rejects the whole
container-capable definition, including a mixed declaration whose selected backend
could otherwise be native. The runner and legacy planner reject these records
before any launch service or process is selected.

## Schema transition and remaining execution work

[`container.schema-transition.json`](../../test/fixtures/backend-v2/container.schema-transition.json)
records the exact old/new raw schema SHA-256 values, semantic schema identities and
typed schema edits. No historical snapshot is regenerated. The transition gate
replays all three historical stages from the pinned C1 source tree with their
unchanged comparators and disclosures, then independently derives the C2 schema's
admission, execution, catalog and ETag changes. It compares the complete expected
and actual snapshots with `cmp` before checking four zero-byte residual projections.

Raw pre-C2 and C2 legacy snapshots are **not byte-identical**: the schema identity
feeds admission, which feeds builtin execution and catalog identities, including
legacy responses. Source bytes, definitions, operations, defaults, feature output,
builtin policies and typed ABI records remain unchanged. The acceptance report
must distinguish raw legacy equality from equality after this explicit transition.

After C3, an ordinary new container using an existing protocol should cost a short
definition of the fixture's scale, one pin entry, model/recipe inventory entries
where needed, a reviewed loader-owned execution grant and a parity fixture. It
should need no new wrapper, process implementation or updater parser. C2 does not
make those grant and installation steps optional.

C3 must seal the complete execution owner independently of hook presence, reject
ungranted external containers and unexported inheritance, retain `llamacpp@1`,
resolve pins and artifact mounts, select exactly one authorized case, and attach a
discriminated plan to `ContainerProcess`. Address discovery remains in that process;
readiness/watchdog consume its validated result. Runtime installation, device
selection, environment protection and cancellation still require attachment and
qualification. CPU fake checks establish no real container, model or GPU behavior.
