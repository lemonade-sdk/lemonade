ARG BASE_IMAGE
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.description="Lemonade build environment with all dependencies pre-installed"
LABEL org.opencontainers.image.source="https://github.com/lemonade-sdk/lemonade"

# Prevent interactive prompts during installation
ENV DEBIAN_FRONTEND=noninteractive
ARG UBUNTU_PPA=""

# Install all build dependencies
RUN apt-get update && apt-get install -y \
    software-properties-common \
    && if [ -n "$UBUNTU_PPA" ]; then add-apt-repository -y "$UBUNTU_PPA"; fi \
    && apt-get update && apt-get install -y \
    build-essential \
    cmake \
    debhelper-compat \
    dh-cmake \
    dh-sequence-cmake \
    dpkg-dev \
    fakeroot \
    fonts-katex \
    git \
    libcap-dev \
    libcli11-dev \
    libcpp-httplib-dev \
    libcurl4-openssl-dev \
    libdrm-dev \
    libjs-katex \
    libssl-dev \
    libsystemd-dev \
    libwebsockets-dev \
    libzstd-dev \
    nlohmann-json3-dev \
    ninja-build \
    node-buffer \
    node-css-loader \
    node-eventsource \
    node-highlight.js \
    node-html-webpack-plugin \
    node-markdown-it \
    node-markdown-it-texmath \
    node-process \
    node-react \
    node-react-dom \
    node-style-loader \
    node-ts-loader \
    node-typescript \
    node-webpack \
    node-webpack-cli \
    nodejs \
    pkg-config \
    quilt \

    && rm -rf /var/lib/apt/lists/*

WORKDIR /workspace

CMD ["/bin/bash"]
