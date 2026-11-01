# Container declarations and execution in v2

V2 resolves, authorizes, compiles and executes reviewed builtin container leaves.
Execution requires a loader-owned sealed execution owner independently of all hook
grants. No production container grant is installed. External container definitions
and container inheritance remain unavailable, including descendants that remove
every hook. Ungranted builtins can still be admitted and compiled as inert data;
binding, runner construction and the legacy planner retain independent rejection
boundaries. V1 does not gain container fields.

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

A definition names a version-file key, not an image version. The runner reads the
effective configured `backend_versions.json` value and validate C1's
`<tag>@sha256:<64 lowercase hex>` syntax, including administrative overrides.
C1's `container_image` produces `repository@sha256:...` for installation and
launch; the tag remains update-channel metadata. There is no digest resolver or mutable tag default. Missing version-file keys,
path overrides, bare tags and malformed digests reject before installation. The
OCI installation path inspects or pulls only the effective digest identity and
honors offline and executable-fetch restrictions. It never selects a managed
binary or substitutes a host executable.

Mounts name the existing artifact inventory. No host path, volume syntax,
writable mode, arbitrary destination or recursive host traversal is accepted.
The existing artifact inventory resolves symlinks, preserves approved HF
snapshot/blob relationships and opens regular file handles. Container mounts use
that inventory to expand complete numbered shard sets or sorted directory files,
reject escaped sources and collisions, and rewrite artifact values to final
container paths before evaluating argument expressions. No arbitrary host-string
substring replacement occurs.
The confinement policy fixes read-only `:ro,z` access, capability drop,
no-new-privileges, default seccomp and labelled internal networking in trusted code.

`BackendDescriptor::containers` carries every policy field, including the pin
source, devices, capabilities, network and teardown. The compiled program keeps
explicit execution cases and mount records alongside its immutable normalized
declaration. Compilation grants no execution authority. External authorization rejects container
records independently of hook selection and checks each inherited layer. For
reviewed builtin leaves, authorization retains the execution owner separately from
hook grants and commits its digest into the authority identity. Binding and runner
construction recheck its sealed semantics. The owner covers process kind, backend
selectors, repositories and pin sources, entrypoint mode, devices, capabilities,
artifact sources and mount destinations, environment, address/network, confinement
and teardown. Hook removal never erases this owner.

The runner selects exactly one case for the chosen backend before installation or
program lookup. Missing or ambiguous cases and container cases without their
matching policies reject. Container plans hold `ContainerRunSpec`; native plans
retain the existing process path. `ConfiguredServer` always constructs
`ContainerProcess` for a container plan. Its optional fake-client seam supplies
only the attached engine client, never a substitute process kind. The legacy
planner remains unavailable to all container records because it carries no sealed
execution owner.

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

`test_container_execution` supplies a fixed test-only Gufo execution grant, the
fixture's version-file pin and a fake `ContainerHost`. It proves catalog admission,
compilation, binding, OCI installation, inventory mounts, loopback readiness,
request forwarding and stop/remove/reap through `ConfiguredServer` and
`ContainerProcess`, without executing Podman or Docker. The production builtin
policies, seven v1 sources, both v2 sources, schemas and historical disclosures
remain unchanged in C3. No additional production schema/catalog identity change
is intended.

Before an engine ships as production JSON it still needs an audited builtin
execution grant, its pin and model/recipe inventory, protocol/argv parity,
packaging and real engine/network/device/cancellation qualification. Existing
`llamacpp@1` sources, `LocalExecutableV1` and external forks remain local-only;
container-capable `llamacpp@2` is outside this unit. CPU fake checks establish no
real container, model or GPU behavior.
