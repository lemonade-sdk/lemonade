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
    ninja-build \
    libssl-dev \
    pkg-config \
    libdrm-dev \
    git \
    nodejs \
    npm \
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . /app
WORKDIR /app

# Build the project
RUN rm -rf build && \
    cmake --preset default && \
    cmake --build --preset default web-app

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
    libdrm2 \
    vulkan-tools \
    libvulkan1 \
    unzip \
    xz-utils \
    libgomp1 \
    libatomic1 \
    libreadline8 \
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
# its external interface, not loopback. Restrict exposure at run time by
# publishing to host loopback (-p 127.0.0.1:13305:13305) and/or setting
# LEMONADE_API_KEY. See docs/guide/install/docker.md.
CMD ["./lemond", "--host", "0.0.0.0"]
