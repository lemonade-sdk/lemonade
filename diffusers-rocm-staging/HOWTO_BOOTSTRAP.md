# How to bootstrap `lemonade-sdk/diffusers-rocm` from this staging directory

This directory exists temporarily inside `lemonade-sdk/lemonade` because the
Claude session that produced it can only push to the lemonade repo. The intent
is for it to be the initial commit of a **new** repository
`lemonade-sdk/diffusers-rocm`. Move it out as follows:

```bash
# 1. Create the empty repo on GitHub (UI or gh CLI):
#    gh repo create lemonade-sdk/diffusers-rocm --public \
#        --description "Portable Hugging Face Diffusers builds with AMD ROCm acceleration"

# 2. From the lemonade repo, copy the staging tree into a fresh checkout:
git clone git@github.com:lemonade-sdk/diffusers-rocm.git ~/diffusers-rocm
cp -r diffusers-rocm-staging/. ~/diffusers-rocm/
rm ~/diffusers-rocm/HOWTO_BOOTSTRAP.md   # not needed in the new repo

# 3. Initial commit + push:
cd ~/diffusers-rocm
git add .
git commit -m "Initial commit: build pipeline + diffusers-server FastAPI shim"
git push -u origin main

# 4. Trigger the first build (workflow_dispatch in the Actions tab,
#    or via gh CLI):
gh workflow run build-diffusers-rocm.yml \
   --repo lemonade-sdk/diffusers-rocm \
   -f gfx_target=gfx1151 \
   -f create_release=true

# 5. Once a release lands, remove this staging dir from lemonade:
cd /path/to/lemonade
git rm -r diffusers-rocm-staging
git commit -m "chore: drop diffusers-rocm staging, repo now lives at lemonade-sdk/diffusers-rocm"
```

## Modeled on `vllm-rocm`

The workflow, launcher shim pattern, release tagging, and split-archive flow
are deliberate ports of `lemonade-sdk/vllm-rocm`. The only structural
difference is that Diffusers ships no HTTP server of its own, so this repo
adds a small FastAPI shim (`src/diffusers_server/server.py`) and the workflow
copies it into the bundle's site-packages before tarring.

## Sanity-checking locally before the first push

You can dry-run the server module without ROCm by stubbing the pipeline:

```bash
pip install diffusers transformers fastapi uvicorn pillow torch
PYTHONPATH=src python -m diffusers_server --help
# Loading a tiny model on CPU works too (slow but proves the API surface):
PYTHONPATH=src python -m diffusers_server \
  --model hf-internal-testing/tiny-stable-diffusion-pipe \
  --dtype fp32 --port 8000 &
curl -s http://localhost:8000/health
```

## Open questions before going public

1. **`rocm-sdk-core` / `rocm-sdk-libraries-gfx<arch>` wheel availability** —
   the workflow installs these best-effort. If they're not on AMD's index for
   a given arch, the build still completes (falling back to torch's bundled
   ROCm libs), but Triton kernels may not run. Verify on each target.
2. **Video output for SANA-WM** — current server only exposes
   `/v1/images/generations`. Add a `/v1/videos/generations` route (or extend
   the image route to return MP4 when a video pipeline is loaded) once the
   image path is validated.
3. **Self-hosted runner for GPU smoke tests** — vllm-rocm tests on
   self-hosted AMD GPU hardware before releasing. The same runner pool can
   verify diffusers-rocm by adding a `gpu-test` job that runs a few-step SANA
   inference against the freshly built artifact.
