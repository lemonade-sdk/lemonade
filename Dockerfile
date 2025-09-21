# Same base image as llama.cpp
FROM ubuntu:24.04

# Install system dependencies for Vulkan backend
RUN apt-get update && apt-get install -y --no-install-recommends \
    python3 python3-venv python3-pip pipx \
    pciutils \
    vulkan-tools vulkan-validationlayers mesa-vulkan-drivers libvulkan-dev \
&& rm -rf /var/lib/apt/lists/*

RUN mkdir -p /opt/lemonade && chown -R ubuntu:ubuntu /opt/lemonade

USER ubuntu

RUN pipx install uv
ENV PATH="/home/ubuntu/.local/bin:$PATH"

RUN mkdir -p /opt/lemonade
WORKDIR /opt/lemonade

# Copy setup.py, source code, and README for editable install
COPY --chown=ubuntu:ubuntu setup.py .
COPY --chown=ubuntu:ubuntu src/ src/
COPY --chown=ubuntu:ubuntu README.md .

# Install Python 3.10 and do Lemonade setup in that venv
RUN uv python install 3.10 && uv venv && uv pip install .

# Run install at build time so llama.cpp isn't downloaded at runtime
RUN uv run lemonade-install --llamacpp vulkan

# Ensure the entire .venv directory is accessible to non-root users
# This is required when using UserNS=keep-id in quadlets
RUN chmod -R 755 /opt/lemonade/.venv && \
    chown -R ubuntu:ubuntu /opt/lemonade/.venv

# Create and fix permissions for huggingface cache directory,
# so can map a shared persistent volume of models into it
RUN mkdir -p /home/ubuntu/.cache/huggingface/hub && \
    chmod -R 755 /home/ubuntu/.cache/huggingface/hub

# Make huggingface model downloads persistent
VOLUME /home/ubuntu/.cache/huggingface/hub

# Lemonade runtime envs
ENV LEMONADE_HOST=0.0.0.0 \
    LEMONADE_PORT=8000 \
    LEMONADE_LLAMACPP=vulkan

EXPOSE 8000

CMD ["sh", "-c", "uv run lemonade-server-dev serve --llamacpp vulkan"]
