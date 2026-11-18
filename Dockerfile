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
    python3 \
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
    tini \
    && rm -rf /var/lib/apt/lists/*

# Container backends run beside this container on the host's Podman or Docker,
# which lemond reaches through the socket the user mounts at
# /run/podman/podman.sock or /var/run/docker.sock. See
# docs/guide/install/docker.md.
ARG TARGETARCH
ARG PODMAN_VERSION=5.7.0
ARG DOCKER_VERSION=29.9.3
RUN arch="${TARGETARCH:-$(dpkg --print-architecture)}" && \
    case "$arch" in \
      amd64) sum=0d1759e3fc42b0e3a4496176dabc7b65f0f334a0ab2b22eaed7c36afeece8b0c ;; \
      arm64) sum=fecff7e133313220e66d3108e64439555ad1fc95b212de8c38ff68089df971fd ;; \
      *) echo "no podman-remote for $arch" >&2; exit 1 ;; \
    esac && \
    curl -fsSL -o /tmp/podman-remote.tar.gz \
      "https://github.com/containers/podman/releases/download/v${PODMAN_VERSION}/podman-remote-static-linux_${arch}.tar.gz" && \
    echo "$sum  /tmp/podman-remote.tar.gz" | sha256sum -c - && \
    tar -xzf /tmp/podman-remote.tar.gz -C /tmp && \
    install -m 0755 "/tmp/bin/podman-remote-static-linux_${arch}" /usr/local/bin/podman && \
    rm -rf /tmp/podman-remote.tar.gz /tmp/bin
# Docker publishes no checksums for its static builds; these are of the
# tarballs as downloaded when the version was pinned.
RUN arch="${TARGETARCH:-$(dpkg --print-architecture)}" && \
    case "$arch" in \
      amd64) darch=x86_64; sum=3fb5d1f13014c18cc54945bc177c58a9056a482dada031e17d12cf2e6922c973 ;; \
      arm64) darch=aarch64; sum=1b27fd739378ef938401e81ce5afb73b7ad7432d4f598814a9ff5f826bb464d2 ;; \
      *) echo "no docker client for $arch" >&2; exit 1 ;; \
    esac && \
    curl -fsSL -o /tmp/docker.tgz \
      "https://download.docker.com/linux/static/stable/${darch}/docker-${DOCKER_VERSION}.tgz" && \
    echo "$sum  /tmp/docker.tgz" | sha256sum -c - && \
    tar -xzf /tmp/docker.tgz -C /tmp docker/docker && \
    install -m 0755 /tmp/docker/docker /usr/local/bin/docker && \
    rm -rf /tmp/docker.tgz /tmp/docker
ENV CONTAINER_HOST=unix:///run/podman/podman.sock

# Run as an unprivileged user; lemond never needs root at runtime.
RUN useradd -r -u 10001 -s /usr/sbin/nologin lemonade

# The application directory doubles as the user's HOME so Lemonade's cache and
# config directories (plus the HuggingFace cache) resolve to writable paths.
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
    /opt/lemonade/.config/lemonade \
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

# Container backends join lemond's PID namespace, so processes they orphan
# reparent to its PID 1, which must reap them; lemond reaps only its own children.
ENTRYPOINT ["/usr/bin/tini", "--"]
