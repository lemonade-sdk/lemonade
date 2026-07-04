# Alpine / musl packaging

Builds an installable `lemonade-server` `.apk` (lemond + lemonade CLI + web app)
for musl-based Linux. NPU backends (FastFlowLM/RyzenAI) and moonshine are not
available on musl and are excluded; GPU (Vulkan) backends work if a Vulkan driver
is present at runtime.

## Build

From the repo root, in an `alpine:latest` container:

```sh
docker run --rm -v "$PWD:/src" -w /src alpine:latest \
    sh packaging/alpine/build-apk.sh
```

The `.apk` lands in `./dist/`. `build-apk.sh` snapshots the working tree, drives
`abuild` with a throwaway signing key, and copies out the result. Set `OUTDIR` to
change the destination.

CI builds both `x86_64` and `aarch64` packages via
`.github/workflows/alpine_apk_build.yml` (runs the same script inside an
`alpine:latest` container, smoke-tests the install, and uploads the `.apk` as a
workflow artifact).

## Install & run

```sh
apk add --allow-untrusted ./dist/lemonade-server-*.apk
rc-service lemonade-server start        # or run `lemond` directly
```

Web UI: <http://localhost:13305/app>

The service runs as the unprivileged `lemonade` user (state in
`/var/lib/lemonade`). Env vars (`HF_TOKEN`, `LEMONADE_API_KEY`, …) go in
`/etc/lemonade/conf.d/*.conf`, mirroring the systemd drop-in on glibc distros.

## musl backend assets

llama.cpp, whisper.cpp, and stable-diffusion.cpp publish musl (`-linux-musl-` /
`-Linux-musl-`) release assets from a fork until they land in `lemonade-sdk/*`.
Until then, point backend downloads at that fork:

```sh
export LEMONADE_BACKEND_REPO_OWNER=clemperorpenguin
```

Only `lemonade-sdk/*` repos are remapped; upstream repos (`ggml-org`, `leejet`)
are untouched. Without this, `lemond` resolves `lemonade-sdk/*` musl assets that
do not exist yet and the download 404s. Drop it in `/etc/lemonade/conf.d/*.conf`
to make it persistent for the service.
