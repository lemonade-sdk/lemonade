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
    ccache \
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

# Ship a primed compiler cache for the Debian package build. GitHub Actions
# caches cannot do this job: they are scoped per git ref, so merge-queue runs —
# which every merge passes through — can never restore one. This image is
# rebuilt nightly, so a day of drift still leaves every source a PR did not
# touch as a cache hit.
#
# Primed under /__w/lemonade/lemonade because that is where a container job
# checks the repository out. The absolute -I paths reach the compiler command
# line, and ccache hashes them, so the prime has to happen at the same path.
#
# Stops at the build target rather than producing a package: only the compile
# populates the cache, and the install step wants paths .dockerignore strips.
#
# PRIME_CCACHE is set only for the image tag whose compile CI actually gates
# (see build-container.yml).
#
# A failed prime must never block publishing: the image is infrastructure and an
# unprimed one is slower, not broken, whereas a blocked publish silently strands
# every consumer on a stale image. The prime therefore only warns, and
# build-container.yml's verify step is what turns the failure red. This matters
# because the prime compiles a .dockerignore-filtered tree, so it can fail on a
# change the .deb job — which builds the full checkout — accepts.
#
# DEB_BUILD_DEBUGPATH pins -fdebug-prefix-map, which dpkg otherwise derives from
# the changelog version — that changes every commit and would miss every lookup.
# The prime records it in /opt/ccache-debugpath so consumers set the identical
# value; writing it here rather than as an ENV means an image built without the
# prime cannot advertise a cache it does not have.
#
# The source is bind-mounted rather than COPYed so the tree does not survive as
# a layer in an image every Linux CI job and every PPA job pulls.
ARG PRIME_CCACHE=""
ENV CCACHE_DIR=/opt/ccache
RUN --mount=type=bind,target=/mnt/src \
    if [ -n "$PRIME_CCACHE" ] && [ "$(dpkg --print-architecture)" = "amd64" ]; then \
        ( set -e; \
          mkdir -p /__w/lemonade; \
          cp -a /mnt/src /__w/lemonade/lemonade; \
          cd /__w/lemonade/lemonade; \
          mv contrib/debian .; \
          sed -e "s|@@DEB_VERSION@@|0~ccache-prime|g" \
              -e "s|@@DEB_CODENAME@@|$(. /etc/os-release && echo "$UBUNTU_CODENAME")|g" \
              -e "s|@@DEB_DATE@@|$(date -R)|g" \
              debian/changelog.in > debian/changelog; \
          ccache -M 1G; \
          DEB_BUILD_DEBUGPATH=/usr/src/lemonade-server \
          CMAKE_C_COMPILER_LAUNCHER=ccache \
          CMAKE_CXX_COMPILER_LAUNCHER=ccache \
              dpkg-buildpackage -us -uc --rules-target=build; \
          ccache -s; \
          echo /usr/src/lemonade-server > /opt/ccache-debugpath ) \
        || echo "WARNING: ccache prime failed; publishing an unprimed image"; \
        rm -rf /__w; \
    fi

WORKDIR /workspace

CMD ["/bin/bash"]
