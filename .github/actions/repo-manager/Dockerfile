# The tools a repo-manager job needs, pinned and baked in so that a job installs nothing
# on the runner. Versions come from versions.env next to this file.
ARG BASE_IMAGE=ubuntu:24.04
FROM ${BASE_IMAGE}

LABEL org.opencontainers.image.description="repo-manager, the pi coding agent it drives, a lemond to serve their model, and the GitHub CLI"
LABEL org.opencontainers.image.source="https://github.com/lemonade-sdk/lemonade"

ENV DEBIAN_FRONTEND=noninteractive

# Vulkan userspace only; the kernel side arrives as the devices the action passes in.
# Without it llama.cpp falls back to CPU, where a 35B model turns a review into an
# overnight job.
RUN apt-get update && apt-get install -y --no-install-recommends \
    ca-certificates \
    curl \
    git \
    jq \
    libatomic1 \
    libcurl4 \
    libdrm2 \
    libgomp1 \
    libreadline8 \
    libssl3 \
    libvulkan1 \
    mesa-vulkan-drivers \
    python3 \
    python3-venv \
    vulkan-tools \
    xz-utils \
    zlib1g \
    && rm -rf /var/lib/apt/lists/*

ARG NODE_VERSION
RUN curl -sfL "https://nodejs.org/dist/v${NODE_VERSION}/node-v${NODE_VERSION}-linux-x64.tar.xz" \
    | tar -xJ --strip-components=1 -C /usr/local \
    && node --version

ARG GH_CLI_VERSION
RUN curl -sfL "https://github.com/cli/cli/releases/download/v${GH_CLI_VERSION}/gh_${GH_CLI_VERSION}_linux_amd64.tar.gz" \
    | tar -xz --strip-components=1 -C /usr/local \
    && gh --version

ARG REPO_MANAGER_VERSION
ENV PATH="/opt/repo-manager/bin:${PATH}"
RUN python3 -m venv /opt/repo-manager \
    && /opt/repo-manager/bin/pip install --quiet --disable-pip-version-check \
       "git+https://github.com/lemonade-sdk/repo-manager.git@${REPO_MANAGER_VERSION}" \
    && repo-manager --version

# pi is pinned by repo-manager rather than by us: it ships the version it was tested
# against. `pi` on npm is a different package; this one installs its binary as `pi`.
RUN PI_VERSION="$(repo-manager pi version)" \
    && test -n "$PI_VERSION" \
    && npm install -g --no-fund --no-audit "@earendil-works/pi-coding-agent@${PI_VERSION}" \
    && pi --version

ARG LEMONADE_VERSION
ENV LEMOND=/opt/lemonade/lemond
RUN mkdir -p /opt/lemonade \
    && curl -sfL "https://github.com/lemonade-sdk/lemonade/releases/download/v${LEMONADE_VERSION}/lemonade-embeddable-${LEMONADE_VERSION}-ubuntu-x64.tar.gz" \
    | tar -xz --strip-components=1 -C /opt/lemonade \
    && chmod +x "$LEMOND" \
    && "$LEMOND" --version

# lemond resolves get_runtime_dir() from this. The container runs as the runner's uid,
# which has no account inside the image, so the directory has to be world-writable.
RUN mkdir -p /run/lemonade && chmod 1777 /run/lemonade
ENV XDG_RUNTIME_DIR=/run/lemonade

# Kept for the action to read back: a tag is mutable, so matching this against the
# checkout is how a job tells a current image from one built before the last bump.
COPY versions.env /opt/versions.env

CMD ["/bin/bash"]
