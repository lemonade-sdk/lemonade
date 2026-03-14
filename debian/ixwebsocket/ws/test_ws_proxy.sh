#!/bin/sh

# This test requires cobra to be available
which cobra > /dev/null || {
    echo cobra is not installed on this machine.
    exit 0
}

# Handle Ctrl-C by killing all sub-processing AND exiting
trap cleanup INT

function cleanup {
    exit_code=${1:-1}
    echo "Killing all servers (ws and cobra)"
    echo
    kill `cat /tmp/pidfile.proxy`  &>/dev/null
    kill `cat /tmp/pidfile.echo_server`  &>/dev/null
    kill `cat /tmp/pidfile.cobra`  &>/dev/null
    kill `cat /tmp/pidfile.connect.echo`  &>/dev/null
    kill `cat /tmp/pidfile.connect.cobra`  &>/dev/null
    exit ${exit_code}
}

ws proxy_server --pidfile /tmp/pidfile.proxy --config_path proxyConfig.json &
ws echo_server --pidfile /tmp/pidfile.echo_server --port 8009 &
cobra -v run --pidfile /tmp/pidfile.cobra --port 5678 &

# Wait for the servers to be up
sleep 1

# unbuffer comes with expect (written in tcl)
echo 'hello' | unbuffer ws connect --pidfile /tmp/pidfile.connect.echo ws://echo.localhost:8008 &

echo 'hello' | unbuffer ws connect --pidfile /tmp/pidfile.connect.cobra ws://cobra.localhost:8008 &

# Wait
sleep 2

cleanup
