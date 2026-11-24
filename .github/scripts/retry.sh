#!/usr/bin/env bash
# Usage: bash retry.sh <command> [args...]

# Runs the command until it succeeds, waiting longer between attempts.
# Launchpad and the Ubuntu mirrors fail in episodes where some requests get 503s and others succeed,
# so a later attempt usually gets through where an immediate retry does not.
# No retry starts after about 12 minutes, so a step's worst case is that plus one attempt,
# which keeps it inside the calling job's time limit.

attempts=7
delay=15
budget=720
for attempt in $(seq 1 "$attempts"); do
    "$@" && exit 0
    status=$?
    if [ "$attempt" -eq "$attempts" ] || [ $(( SECONDS + delay )) -gt "$budget" ]; then
        exit "$status"
    fi
    echo "::warning::'$*' failed with exit code $status (attempt $attempt/$attempts); retrying in ${delay}s"
    sleep "$delay"
    delay=$(( delay * 2 > 240 ? 240 : delay * 2 ))
done
