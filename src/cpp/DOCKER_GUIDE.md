# Running Lemonade C++ in Docker

## Quick Start with Docker

> You may need additional configuration depending on your environment.

### Docker Run with Default Configuration

```bash
docker run -d \
  --name lemonade-server \
  -p 8000:8000 \
  -v lemonade-cache:/root/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -e LEMONADE_LLAMACPP_BACKEND=cpu \
  ghcr.io/lemonade-sdk/lemonade-server:latest
```

### Docker Run with a Specific Port and Version

```bash
docker run -d \
  --name lemonade-server \
  -p 4000:5000 \
  -v lemonade-cache:/root/.cache/huggingface \
  -v lemonade-llama:/opt/lemonade/llama \
  -e LEMONADE_LLAMACPP_BACKEND=cpu \
  ghcr.io/lemonade-sdk/lemonade-server:v9.1.3 \
  ./lemonade-server serve --no-tray --host 0.0.0.0 --port 5000
```

> This will run the server on port 5000 inside the container, mapped to port 4000 on your host.

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
      - "8000:8000"
    volumes:
      # Persist downloaded models
      - lemonade-cache:/root/.cache/huggingface
      # # Persist llama binaries
      - lemonade-llama:/opt/lemonade/llama
    environment:
      - LEMONADE_LLAMACPP_BACKEND=cpu
    restart: unless-stopped

volumes:
  lemonade-cache:
  lemonade-llama:
```

> You can add more services as needed.

3. Run the following command in the directory containing your docker-compose.yml:

```bash
docker-compose up -d
```

This will pull the latest image (or the version you specified) from the Lemonade container registry and start the server with your mapped ports.

Once the container is running, verify it’s working:

```bash
curl http://localhost:8000/api/v1/models
```

You should receive a response listing available models.

<br>

# Build Your Own Docker Image
Documentation below shows container based workflows and how to build your own environments if needed.

## Container-based workflows

This repository supports two container-related workflows with different goals:

### Development (Dev Containers)
The `.devcontainer` ([dev container](https://github.com/lemonade-sdk/lemonade/blob/main/src/cpp/README.md#developer-ide--ide-build-steps)) configuration is intended for contributors and developers.
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

This configuration has been tested with both the Vulkan and CPU backends and you can modify or extend it to suit your specific deployment needs.

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
WORKDIR /app/src/cpp

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

# Install runtime dependencies only
RUN apt-get update && apt-get install -y \
    libcurl4 \
    curl \
    libssl3 \
    zlib1g \
    vulkan-tools \
    libvulkan1 \
    unzip \
    libgomp1 \
    && rm -rf /var/lib/apt/lists/*

# Create application directory
WORKDIR /opt/lemonade

# Copy built executables and resources from builder
COPY --from=builder /app/src/cpp/build/lemonade-router ./lemonade-router
COPY --from=builder /app/src/cpp/build/lemonade-server ./lemonade-server
COPY --from=builder /app/src/cpp/build/resources ./resources

# Make executables executable
RUN chmod +x ./lemonade-router ./lemonade-server

# Create necessary directories
RUN mkdir -p /opt/lemonade/llama/cpu \
    /opt/lemonade/llama/vulkan \
    /root/.cache/huggingface

# Expose default port
EXPOSE 8000

# Health check
HEALTHCHECK --interval=30s --timeout=10s --start-period=5s --retries=3 \
    CMD curl -f http://localhost:8000/live || exit 1

# Default command: start server in headless mode
CMD ["./lemonade-server", "serve", "--no-tray", "--host", "0.0.0.0"]
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
      - "8000:8000"
    volumes:
      # Persist downloaded models
      - lemonade-cache:/root/.cache/huggingface
      # # Persist llama binaries
      - lemonade-llama:/opt/lemonade/llama
    environment:
      - LEMONADE_LLAMACPP_BACKEND=cpu
    restart: unless-stopped

volumes:
  lemonade-cache:
  lemonade-llama:

```

Now run below command within the same directory:

```bash
docker-compose build
```

This will:

- Compile Lemonade C++ (lemonade-server and lemonade-router)
- Prepare a runtime image with all dependencies

### 3. Run the Container

Start the container with Docker Compose:

```bash
docker-compose up -d
```

- The API will be exposed on port 8000
- HuggingFace models will be cached in the lemonade-cache volume
- LLaMA binaries are persisted in lemonade-llama volume

Check that the server is running:

```bash
docker logs -f lemonade-server
```

You should see:

```bash
lemonade-server  | Lemonade Server vx.x.x started on port 8000
lemonade-server  | Chat and manage models: http://localhost:8000
```

---

### 4. Access the API

Test the API:
```bash
curl http://localhost:8000/api/v1/models
```

You should get a response with available models.

### 5. Load a Model

You can use the gui on localhost:8000 or below command to load a model (e.g., Qwen 0.6B):

```bash
curl -X POST http://localhost:8000/api/v1/load \
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
    base_url="http://localhost:8000/api/v1",
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

- Model download fails: Ensure /root/.cache/huggingface volume is writable
- Vulkan errors on CPU-only machine: The server will fallback to CPU backend automatically
