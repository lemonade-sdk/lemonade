FROM mcr.microsoft.com/devcontainers/cpp:1-ubuntu-24.04

ARG REINSTALL_CMAKE_VERSION_FROM_SOURCE="3.22.2"

COPY .devcontainer/reinstall-cmake.sh /usr/local/src/

RUN if [ "${REINSTALL_CMAKE_VERSION_FROM_SOURCE}" != "none" ]; then \
        chmod +x /usr/local/src/reinstall-cmake.sh && /usr/local/src/reinstall-cmake.sh ${REINSTALL_CMAKE_VERSION_FROM_SOURCE}; \
    fi \
    && rm -f /usr/local/src/reinstall-cmake.sh

# Install Dependencies
RUN apt-get update && export DEBIAN_FRONTEND=noninteractive \
    && apt-get -y install --no-install-recommends \
    python3 \
    python3-pip \
    python3-dev \
    python3-venv \
    python3-setuptools \
    build-essential \
    libdrm-dev \
    git

# Setup Venv
ENV VIRTUAL_ENV=/opt/venv
RUN python3 -m venv $VIRTUAL_ENV
ENV PATH="$VIRTUAL_ENV/bin:$PATH"

RUN chown -R vscode:vscode ${VIRTUAL_ENV}

WORKDIR /usr/src/app

# Copy the Project Root into the container
COPY . .

# We create XDG_RUNTIME ENV VAR.
RUN mkdir -p /run/lemonade
RUN chown vscode:vscode /run/lemonade
ENV XDG_RUNTIME_DIR=/run/lemonade
