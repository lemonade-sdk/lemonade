#!/bin/sh
set -eu

TARGET_USER="lemonade"

add_device_group() {
    path="$1"

    if [ ! -e "$path" ]; then
        return
    fi

    gid="$(stat -c '%g' "$path" 2>/dev/null || true)"
    if [ -z "$gid" ] || [ "$gid" = "0" ]; then
        return
    fi

    group_name="$(getent group "$gid" | cut -d: -f1 || true)"
    if [ -z "$group_name" ]; then
        group_name="hostdev_${gid}"
        if ! getent group "$group_name" >/dev/null 2>&1; then
            groupadd -g "$gid" "$group_name" >/dev/null 2>&1 || true
        fi
        group_name="$(getent group "$gid" | cut -d: -f1 || true)"
    fi

    if [ -n "$group_name" ]; then
        usermod -a -G "$group_name" "$TARGET_USER" >/dev/null 2>&1 || true
    fi
}

if [ "$(id -u)" = "0" ]; then
    # Runtime directories may be ephemeral in container runtimes.
    mkdir -p /run/lemonade
    chown "$TARGET_USER":"$TARGET_USER" /run/lemonade

    # Mirror host device group ownership into the container user so non-root
    # access works for mapped accelerator devices.
    add_device_group /dev/accel
    add_device_group /dev/dri
    add_device_group /dev/kfd

    for dev in /dev/accel/* /dev/dri/renderD*; do
        add_device_group "$dev"
    done

    if command -v setpriv >/dev/null 2>&1; then
        target_uid="$(id -u "$TARGET_USER")"
        target_gid="$(id -g "$TARGET_USER")"
        exec setpriv --reuid "$target_uid" --regid "$target_gid" --init-groups "$@"
    fi

    exec runuser -u "$TARGET_USER" -- "$@"
fi

exec "$@"
