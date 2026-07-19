# Running Lemonade in Docker

## Quick Start with Docker

> You may need additional configuration depending on your environment.

> **Security note**
>
> The container runs `lemond` as an unprivileged user and binds `0.0.0.0`
> *inside* the container (required for Docker port publishing to work). The
> examples below use `-p 13305:13305`, which exposes the **unauthenticated** API
> on every host interface. To limit exposure:
> - Publish to host loopback only: `-p 127.0.0.1:13305:13305`.
> - Require authentication by setting `-e LEMONADE_API_KEY=<key>`.

### Docker Run with Default Configuration

```bash
docker run -d \
  --name lemonade-server \
  -p 13305:13305 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -v lemonade-recipe:/opt/lemonade/.cache/lemonade \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

> **Upgrading from an older version?**
>
> If you're upgrading from an image that ran as root (versions prior to v10.10.1), your existing named volumes may still be owned by root. The new image runs as UID 10001 and will fail to write to root-owned volumes. Fix ownership with helper containers:
>
> ```bash
> docker run --rm -v lemonade-cache:/v ubuntu:24.04 chown -R 10001:10001 /v
> docker run --rm -v lemonade-llama:/v ubuntu:24.04 chown -R 10001:10001 /v
> docker run --rm -v lemonade-recipe:/v ubuntu:24.04 chown -R 10001:10001 /v
> ```
>
> Alternatively, remove the old volumes and let the new container recreate them:
>
> ```bash
> docker volume rm lemonade-cache lemonade-llama lemonade-recipe
> ```

### Docker Run with a Specific Port

```bash
docker run -d \
  --name lemonade-server \
  -p 4000:5000 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -v lemonade-recipe:/opt/lemonade/.cache/lemonade \
  ghcr.io/lemonade-sdk/lemonade-server:latest \
  ./lemond --host 0.0.0.0 --port 5000
```

> This will run the server on port 5000 inside the container, mapped to port 4000 on your host.

### Docker Run with CPU backend

To use the CPU backend, create or modify the `config.json` file in the `lemonade-recipe` volume:

```json
{
  "llamacpp": {
    "backend": "cpu"
  }
}
```

Then run:

```bash
docker run -d \
  --name lemonade-server \
  -p 13305:13305 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -v lemonade-recipe:/opt/lemonade/.cache/lemonade \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

### Docker Run with AMD GPU Passthrough using ROCm

To use the ROCm backend, create or modify the `config.json` file in the `lemonade-recipe` volume:

```json
{
  "llamacpp": {
    "backend": "rocm"
  }
}
```

Then run:

```bash
docker run -d \
  --name lemonade-server \
  -p 13305:13305 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -v lemonade-recipe:/opt/lemonade/.cache/lemonade \
  --device=/dev/kfd \
  --device=/dev/dri \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

> This will run the server using the ROCm backend as the default for llama.cpp.

### Docker Run with AMD GPU Passthrough using ROCm on WSL

Make sure you follow install steps described in [ROCm for WSL](https://rocm.docs.amd.com/projects/radeon-ryzen/en/latest/docs/install/installrad/wsl/howto_wsl.html)

Create or modify the `config.json` file in the `lemonade-recipe` volume:

```json
{
  "llamacpp": {
    "backend": "rocm"
  }
}
```

Then:

```bash
docker run -d \
  --name lemonade-server \
  -p 13305:13305 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -v lemonade-recipe:/opt/lemonade/.cache/lemonade \
  -v /usr/lib/wsl/lib:/usr/lib/wsl/lib:ro \
  -v /opt/rocm/lib:/opt/rocm/lib:ro \
  -e LD_LIBRARY_PATH=/opt/rocm/lib:/opt/rocm/lib/rocm_sysdeps/lib:/usr/lib/wsl/lib:/usr/lib \
  --device=/dev/dxg \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

> This will run the server using the ROCm backend as the default for llama.cpp.

### Other Docker Methods

#### Docker Compose Setup
Docker Compose makes it easier to manage multi-container applications.
1. Make sure you have Docker Compose installed.
2. Create a `docker-compose.yml` file like this:

```yml
services:
  lemonade:
    image: ghcr.io/lemonade-sdk/lemonade-server:latest
    container_name: lemonade-server
    ports:
      - "13305:13305"
    volumes:
      # Persist downloaded models
      - lemonade-cache:/opt/lemonade/.cache/huggingface
      # Persist llama binaries
      - lemonade-llama:/opt/lemonade/llama
      # Persist model options and other backend binaries
      - lemonade-recipe:/opt/lemonade/.cache/lemonade
    restart: unless-stopped

volumes:
  lemonade-cache:
  lemonade-llama:
  lemonade-recipe:
```

> To configure the llama.cpp backend (e.g., CPU instead of auto-detect), create a `config.json` file in the `lemonade-recipe` volume with:
> ```json
> {
>   "llamacpp": {
>     "backend": "cpu"
>   }
> }
> ```

> You can add more services as needed, or add host devices for the ROCM backend.

3. Run the following command in the directory containing your docker-compose.yml:

```bash
docker-compose up -d
```

This will pull the latest image (or the version you specified) from the Lemonade container registry and start the server with your mapped ports.

Once the container is running, verify it’s working:

```bash
curl http://localhost:13305/api/v1/models
```

You should receive a response listing available models.

<br>

# Build Your Own Docker Image
Documentation below shows container based workflows and how to build your own environments if needed.

## Container-based workflows

This repository supports two container-related workflows with different goals:

### Development (Dev Containers)
The `.devcontainer` ([dev container](https://github.com/lemonade-sdk/lemonade/blob/main/docs/dev-getting-started.md#developer-ide--ide-build-steps)) configuration is intended for contributors and developers.
It provides a full development environment (tooling, debuggers, source mounted)
and is primarily used with VS Code Dev Containers or GitHub Codespaces.

### Running Lemonade in a container
The Dockerfile and `docker-compose.yml` guide provided here are intended for running
Lemonade as an application in a containerized environment. This uses a
multi-stage build to produce a minimal runtime image, similar in spirit to the
MSI-based distribution, but containerized.

These workflows are complementary and serve different use cases.

## Lemonade C++ Docker Setup
This guide explains how to build and run Lemonade C++ in a Docker container using Docker Compose. The setup includes persistent caching for HuggingFace models.

> If you want to pull or use a specific Lemonade Docker image instead of building your own, check out the instructions in `README.md`

---

### Prerequisites
- Docker >= 24.x
- Docker Compose >= 2.x
- At least 8 GB RAM and 4 CPU cores recommended for small models
- Internet access to download model files from HuggingFace

---

### 1. Docker File
The Dockerfile below uses a **multi-stage build** to compile Lemonade C++ components and produce a clean, lightweight runtime image.

Place the Dockerfile in the parent directory of the repository root when building.

> **Build context note**
>
> This guide assumes the Dockerfile and `docker-compose.yml` live outside the Lemonade repository directory.
> Like below
>```css
>.
>├── docker-compose.yml
>├── Dockerfile
>└── lemonade/
>    ├── src
>    ├── docs
>    ├── .devcontainer
>    └── ...
>```
> If you place them inside the repository,
> update the Dockerfile to use `COPY . /app` instead.

This configuration has been tested with Vulkan, ROCM, and CPU backends and you can modify or extend it to suit your specific deployment needs.

```dockerfile
# ==============================================================
# # 1. Build stage — compile lemonade C++ binaries
# # ============================================================
FROM ubuntu:24.04 AS builder

# Avoid interactive prompts during build
ENV DEBIAN_FRONTEND=noninteractive

# Install build dependencies
RUN apt-get update && apt-get install -y \
    build-essential \
    cmake \
    libssl-dev \
    pkg-config \
    git \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY lemonade /app
WORKDIR /app/

# Build the project
RUN rm -rf build && \
    mkdir -p build && \
    cd build && \
    cmake .. && \
    cmake --build . --config Release -j"$(nproc)"

# Debug: Check build outputs
RUN echo "=== Build directory contents ===" && \
    ls -la build/ && \
    echo "=== Checking for resources ===" && \
    find build/ -name "*.json" -o -name "resources" -type d

# # ============================================================
# # 2. Runtime stage — small, clean image
# # ============================================================
FROM ubuntu:24.04

# vLLM/Triton JIT-compiles native launcher modules at runtime.
RUN apt-get update && apt-get install -y \
    build-essential \
    libcurl4 \
    curl \
    libssl3 \
    zlib1g \
    vulkan-tools \
    libvulkan1 \
    unzip \
    libgomp1 \
    libatomic1 \
    && rm -rf /var/lib/apt/lists/*

# Run as an unprivileged user; lemond never needs root at runtime.
RUN useradd -r -u 10001 -s /usr/sbin/nologin lemonade

# The application directory doubles as the user's HOME so the HuggingFace and
# lemonade caches (both derived from $HOME) resolve to writable, owned paths.
WORKDIR /opt/lemonade
ENV HOME=/opt/lemonade

# Provide a private runtime directory so lemond can use get_runtime_dir()
RUN mkdir -p /run/lemonade && chmod 700 /run/lemonade
ENV XDG_RUNTIME_DIR=/run/lemonade

# Copy built executables and resources from builder
COPY --from=builder /app/build/lemond ./lemond
COPY --from=builder /app/build/lemonade ./lemonade
COPY --from=builder /app/build/resources ./resources

# Make executables executable
RUN chmod +x ./lemond ./lemonade

# Expose the lemond/lemonade binaries on PATH so `docker exec` users can run
# them (e.g. `lemonade list`, `lemonade pull`) without needing the full path.
ENV PATH="/opt/lemonade:${PATH}"

# Create cache directories and hand the whole tree to the unprivileged user.
RUN mkdir -p /opt/lemonade/llama/cpu \
    /opt/lemonade/llama/vulkan \
    /opt/lemonade/.cache/huggingface \
    /opt/lemonade/.cache/lemonade && \
    chown -R lemonade:lemonade /opt/lemonade /run/lemonade

USER lemonade

# Expose default port
EXPOSE 13305

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:13305/live || exit 1

# Default command: start server in headless mode.
# Binds 0.0.0.0 because Docker port publishing (-p) reaches the container via
# its external interface, not loopback. Restrict exposure by publishing to
# host loopback (-p 127.0.0.1:13305:13305) and/or setting LEMONADE_API_KEY.
CMD ["./lemond", "--host", "0.0.0.0"]
```

### 2. Build the Docker Image

Create below `docker-compose.yml` file within the parent directory of repository root (where Dockerfile is located):

```yml
services:
  lemonade:
    build:
      context: .
      dockerfile: Dockerfile
    container_name: lemonade-server
    ports:
      - "13305:13305"
    volumes:
      # Persist downloaded models
      - lemonade-cache:/opt/lemonade/.cache/huggingface
      # Persist llama binaries
      - lemonade-llama:/opt/lemonade/llama
      # Persist model options and other backend binaries
      - lemonade-recipe:/opt/lemonade/.cache/lemonade
    restart: unless-stopped

volumes:
  lemonade-cache:
  lemonade-llama:
  lemonade-recipe:

```

> To configure the llama.cpp backend (e.g., CPU instead of auto-detect), create a `config.json` file in the `lemonade-recipe` volume with:
> ```json
> {
>   "llamacpp": {
>     "backend": "cpu"
>   }
> }
> ```

Now run below command within the same directory:

```bash
docker-compose build
```

This will:

- Compile Lemonade C++ (lemond server and lemonade CLI)
- Prepare a runtime image with all dependencies

### 3. Run the Container

Start the container with Docker Compose:

```bash
docker-compose up -d
```

- The API will be exposed on port 13305
- HuggingFace models will be cached in the lemonade-cache volume
- LLaMA binaries are persisted in lemonade-llama volume

Check that the server is running:

```bash
docker logs -f lemonade-server
```

You should see:

```bash
lemonade-server  | Lemonade Server vx.x.x started on port 13305
lemonade-server  | Chat and manage models: http://localhost:13305
```

---

### 4. Access the API

Test the API:
```bash
curl http://localhost:13305/api/v1/models
```

You should get a response with available models.

### 5. Load a Model

You can use the gui on localhost:13305 or below command to load a model (e.g., Qwen 0.6B):

```bash
curl -X POST http://localhost:13305/api/v1/load \
     -H "Content-Type: application/json" \
     -d '{"model_name": "Qwen3-0.6B-GGUF"}'
```

The server will:
- Auto-download the GGUF model from HuggingFace
- Install the backend
- Make the model ready for inference

### 6. Make a Chat Request

Once the model is loaded:

```python
from openai import OpenAI

client = OpenAI(
    base_url="http://localhost:13305/api/v1",
    api_key="lemonade"  # required but unused
)

completion = client.chat.completions.create(
    model="Qwen3-0.6B-GGUF",
    messages=[{"role": "user", "content": "Hello, Lemonade!"}]
)

print(completion.choices[0].message.content)
```

### 7. Stopping the Server

```bash
docker-compose down
```

- Keeps cached models and binaries in Docker volumes
- You can restart anytime with docker-compose up -d

### 8. Troubleshooting

Server not starting: Check logs with:

```bash
docker logs lemonade-server
```

If you want to view the logs on the web UI, you need to expose the websocket port as well:

```bash
docker run -d \
  --name lemonade-server \
  -p 13305:13305 \
  -p 9000:9000 \
  -v lemonade-cache:/opt/lemonade/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

- Model download fails: Ensure /opt/lemonade/.cache/huggingface volume is writable
- Vulkan errors on CPU-only machine: The server will fallback to CPU backend automatically
