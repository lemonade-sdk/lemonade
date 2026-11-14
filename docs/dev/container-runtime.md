# Container runtime

The shared container runtime can execute reviewed builtin v2 container leaves
through `ConfiguredServer`. Execution requires a sealed loader-owned grant;
Gufo has a production JSON resource and a reviewed dormant grant, with no active
leaf policy or executable registration. See `container-conversion.md` for its
unvalidated stack pin and remaining activation gates. The executable catalog has
no active container leaf. Native plans continue to select `NativeProcess`. CPU tests inject a fake
container host and fake attached client and never execute Podman or Docker.

`utils::ContainerManager` selects Podman before Docker, checks the host, emits
container argv, manages pinned images and labelled internal networks, materializes
read-only model mounts, and inspects an owned container's address. Host commands,
streaming output, filesystem reads, group and account lookup, environment and
sleep are supplied by `ContainerHost`. A fake host can exercise every branch
without executing a container tool or opening GPU devices.

`ContainerRunSpec` holds runtime data. `container_image(repository, pin)` converts
a `<tag>@sha256:<64 lowercase hex>` version pin into a digest-only image reference.
The tag records the update channel. Image operations and `run_command` reject
mutable references and repositories outside `docker.io/kyuz0/` and
`ghcr.io/peonist-ai/`. Namespace acceptance is not an execution grant.

Every emitted run retains `--pull=never`, `--cap-drop=all`, only the supplied
`--cap-add` grants, no-new-privileges and the runtime's default seccomp profile.
Rootless Podman retains supplementary groups and publishes the selected port on
loopback. Rootful Podman and Docker add only the host's render GID and use the
private container address. Docker Desktop, rootless Docker and rootful Podman
with CNI are rejected. Unknown Podman mode fails closed.

`model_mounts` resolves file symlinks, expands directory files individually, sorts
the resulting mounts and rejects missing or unsupported files, escaped inventory
paths, duplicate targets and invalid destinations. Hugging Face snapshot links
may resolve into a separate blobs directory. Mount source authorization belongs
to the future admitted artifact plan; this helper does not authorize arbitrary
host paths. Run argv checks volume syntax and fixes access to `:ro,z`.

`ContainerProcess` implements the same `ServerProcess::start`, `stop` and `running`
interface as `NativeProcess`. Its constructor accepts a run template, a shared
manager, an optional client process and cancellation/clock/sleep effects. The
template contains the owned identity, image, devices, capabilities and mounts.
`start(ServerCommand)` supplies the program, arguments, environment and port;
prepopulated command/environment fields or a conflicting port are rejected.
Paths must already be container-relative: this adapter does not discover artifacts,
rewrite arbitrary strings, select GPUs, resolve versions or install images.

The adapter starts an attached native Podman/Docker client, returns loopback for
rootless Podman, and discovers the rootful host with a 30-second deadline and
cancellation checks. Each inspection is bounded to at most five seconds. It
requires exactly one inspected object, its exact owned name and Lemonade label,
and the expected network attachment. Other networks never supply the result.
Duplicate JSON keys, multiple objects, multiple address families, malformed or
non-unicast IPv4 addresses fail closed. Missing attachments or addresses can be
retried until the deadline; they never produce a fallback host. This initial
runtime accepts one IPv4 address; IPv6-only operation remains unsupported.

Readiness, watchdog and request routing remain with `WrappedServer`, using the
host returned by the process. Failed starts, unload and destruction stop the
owned container by name with the retained ten-second grace, remove it and its
network, and stop/reap the attached client. Cleanup attempts continue after an
individual host exception and failures are logged. Normal cleanup does not
establish crash cleanup after lemond or the runtime client is killed.

[V2 container execution](container-definitions-v2.md) attaches effective pinned
versions, leaf-owned OCI installation, sealed authority and explicit execution and
mount plans. Process address discovery stays here; readiness, watchdog and
forwarding consume the validated returned host. External containers and container
inheritance remain unavailable. The fake end-to-end test demonstrates the full
admitted definition path without granting production container authority.

CPU checks use `test_container_manager`; `test/container_runtime_mutations.py`
compiles each rule mutation in a separate persistent scratch directory and
requires its named assertion to fail. These checks qualify runtime construction
and failure behavior. They do not qualify Podman/Docker networking, GPU inference,
packaging, native Windows/macOS builds or crash cleanup.
