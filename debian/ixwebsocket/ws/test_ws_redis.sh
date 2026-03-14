#!/bin/sh

# Handle Ctrl-C by killing all sub-processing AND exiting
trap cleanup INT

function cleanup {
    kill `cat /tmp/pidfile.subscribe`
    exit 1
}

REDIS_HOST=${REDIS_HOST:=localhost}

ws redis_subscribe --pidfile /tmp/pidfile.subscribe --host $REDIS_HOST foo &

# Wait for the subscriber to be ready
sleep 0.5

# Now publish messages
ws redis_publish -c 100000 --host ${REDIS_HOST} foo bar

# Wait a little for all messages to be received
sleep 1.5

# Cleanup
cleanup
