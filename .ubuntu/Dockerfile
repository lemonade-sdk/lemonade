ARG BASE_IMAGE
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.description="Lemonade build environment with all dependencies pre-installed"
LABEL org.opencontainers.image.source="https://github.com/lemonade-sdk/lemonade"

# Prevent interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_PPA=""

# Set up PPA if needed
RUN if [ -n "$UBUNTU_PPA" ]; then \
        apt update && apt install -y software-properties-common && \
        add-apt-repository -y "$UBUNTU_PPA"; \
    fi

# Install all build dependencies
COPY contrib /contrib
RUN apt update && apt install -y \
    build-essential \
    dpkg-dev \
    git && \
    cd /contrib && \
    apt build-dep . -y && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /workspace

CMD ["/bin/bash"]
