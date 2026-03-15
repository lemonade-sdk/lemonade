FROM alpine:3.12 as build

RUN apk add --no-cache \
    gcc g++ musl-dev linux-headers \
    cmake mbedtls-dev make zlib-dev python3-dev ninja git

RUN addgroup -S app && \
    adduser -S -G app app && \
    chown -R app:app /opt && \
    chown -R app:app /usr/local

# There is a bug in CMake where we cannot build from the root top folder
# So we build from /opt
COPY --chown=app:app . /opt
WORKDIR /opt

USER app
RUN make -f makefile.dev ws_mbedtls_install && \
    sh tools/trim_repo_for_docker.sh

FROM alpine:3.12 as runtime

RUN apk add --no-cache libstdc++ mbedtls ca-certificates python3 strace && \
    addgroup -S app && \
    adduser -S -G app app

COPY --chown=app:app --from=build /usr/local/bin/ws /usr/local/bin/ws

# COPY --chown=app:app --from=build /opt /opt

RUN chmod +x /usr/local/bin/ws && \
    ldd /usr/local/bin/ws

# Now run in usermode
USER app
WORKDIR /home/app

ENTRYPOINT ["ws"]
EXPOSE 8008
