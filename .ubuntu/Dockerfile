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
    curl \
    dpkg-dev \
    git \
    python3-pip \
    python3-venv \
    unzip \
    && \
    cd /contrib && \
    apt build-dep . -y && \
    rm -rf /var/lib/apt/lists/*

# Temporary Ubuntu 26.04 workaround for node-commander version
RUN UBUNTU_VERSION="$(. /etc/os-release && echo "$VERSION_ID")" && \
    if [ "$UBUNTU_VERSION" = "26.04" ]; then \
        UBUNTU_CODENAME="$(. /etc/os-release && echo "$UBUNTU_CODENAME")" && \
        INSTALLED_NODE_COMMANDER_VERSION="$(dpkg-query -W -f='${Version}' node-commander 2>/dev/null || true)" && \
        if [ -z "$INSTALLED_NODE_COMMANDER_VERSION" ] || dpkg --compare-versions "$INSTALLED_NODE_COMMANDER_VERSION" lt "14.0.3-4"; then \
            echo "deb http://archive.ubuntu.com/ubuntu ${UBUNTU_CODENAME}-proposed main universe" > /etc/apt/sources.list.d/proposed.list && \
            printf "Package: *\nPin: release a=${UBUNTU_CODENAME}-proposed\nPin-Priority: 100\n" > /etc/apt/preferences.d/proposed && \
            apt update && \
            apt install -y -t "${UBUNTU_CODENAME}-proposed" \
                node-commander \
                node-babel7-runtime \
                node-babel7 \
                node-clean-css \
                terser \
                node-terser && \
            rm -f /etc/apt/sources.list.d/proposed.list /etc/apt/preferences.d/proposed; \
        fi; \
    fi && \
    rm -rf /var/lib/apt/lists/*
WORKDIR /workspace

CMD ["/bin/bash"]
