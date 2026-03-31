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
    && rm -rf /var/lib/apt/lists/*

# Copy source code
COPY . /app
WORKDIR /app

# Build the project
RUN rm -rf build && \
    cmake --preset default && \
    cmake --build --preset default

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
    libdrm2 \
    vulkan-tools \
    libvulkan1 \
    unzip \
    libgomp1 \
    libatomic1 \
    software-properties-common \
    jq \
    && rm -rf /var/lib/apt/lists/*

RUN add-apt-repository -y ppa:amd-team/xrt

# Create application directory
WORKDIR /opt/lemonade

# Copy built executables and resources from builder
COPY --from=builder /app/build/lemond ./lemond
COPY --from=builder /app/build/lemonade-server ./lemonade-server
COPY --from=builder /app/build/lemonade ./lemonade
COPY --from=builder /app/build/resources ./resources

# Download and install FLM using version from backend_versions.json
RUN FLM_VERSION=$(jq -r '.flm.npu' ./resources/backend_versions.json) && \
    FLM_VERSION_NUM=$(echo $FLM_VERSION | sed 's/^v//') && \
    curl -L -o fastflowlm.deb "https://github.com/FastFlowLM/FastFlowLM/releases/download/${FLM_VERSION}/fastflowlm_${FLM_VERSION_NUM}_ubuntu24.04_amd64.deb" && \
    apt install -y ./fastflowlm.deb && \
    rm fastflowlm.deb

# Make executables executable
RUN chmod +x ./lemond ./lemonade-server

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
