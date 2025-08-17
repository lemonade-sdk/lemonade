#!/usr/bin/env bash

set -e

if [ ! -d "/opt/venv/bin/vulkan" ]
then
    mkdir -p "/srv/lemonade/bin/vulkan"
    ln -s "/srv/lemonade/bin/vulkan" "/opt/venv/bin"
fi

if [ "$1" = "lemonade-server-dev" ]; then
    . "/opt/venv/bin/activate"
    if [ -z ${LEMONADE_HOST+x} ]
        then export LEMONADE_HOST="*"
    fi
    export HF_HOME="/srv/lemonade"
    exec lemonade-server-dev serve
fi

exec "$@"
