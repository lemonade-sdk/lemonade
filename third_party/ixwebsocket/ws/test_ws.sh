#!/bin/bash

# Handle Ctrl-C by killing all sub-processing AND exiting
trap cleanup_and_exit INT

function cleanup_and_exit {
    local exit_code=${1:-1}
    echo "cleaning up..."
    echo
    kill `cat /tmp/ws_test/pidfile.transfer` &>/dev/null
    kill `cat /tmp/ws_test/pidfile.receive` &>/dev/null
    kill `cat /tmp/ws_test/pidfile.send` &>/dev/null
    exit ${exit_code}
}

WITH_TLS=${WITH_TLS:-0}
BLOCKS=${BLOCKS:-20000}

rm -rf /tmp/ws_test
mkdir -p /tmp/ws_test
client_tls=''
server_tls=''
protocol='ws://'
delay='--delay 10'
if [ "$WITH_TLS" == "1" ]; then
    certs="/tmp/ws_test/certs"
    ./generate_certs.sh "${certs}"
    client_tls="--cert-file ${certs}/trusted-client-crt.pem"
    client_tls="${client_tls} --key-file ${certs}/trusted-client-key.pem"
    client_tls="${client_tls} --ca-file ${certs}/trusted-ca-crt.pem"
    server_tls="--cert-file ${certs}/trusted-server-crt.pem"
    server_tls="${server_tls} --key-file ${certs}/trusted-server-key.pem"
    server_tls="${server_tls} --ca-file ${certs}/trusted-ca-crt.pem"
    server_tls="${server_tls} --tls"
    protocol='wss://'
    delay=''
fi

# Start a transport server
cd /tmp/ws_test
ws transfer --port 8090 ${server_tls} --pidfile /tmp/ws_test/pidfile.transfer &

# Wait until the transfer server is up
while true
do
    nc -zv 127.0.0.1 8090 && {
        echo "Transfer server up and running"
        break
    }
    echo "sleep ... wait for transfer server"
    sleep 0.1
done

# Start a receiver
mkdir -p /tmp/ws_test/receive
cd /tmp/ws_test/receive
ws receive "${protocol}127.0.0.1:8090" ${delay} --pidfile /tmp/ws_test/pidfile.receive ${client_tls} &

mkdir -p /tmp/ws_test/send
cd /tmp/ws_test/send
dd if=/dev/urandom of=/tmp/ws_test/send/20M_file count=$BLOCKS bs=1024

# Start the sender job
ws send ${client_tls} --pidfile /tmp/ws_test/pidfile.send "${protocol}127.0.0.1:8090" /tmp/ws_test/send/20M_file

# Wait until the file has been written to disk
while true
do
    if test -f /tmp/ws_test/receive/20M_file ; then
        echo "Received file does exists, exiting loop"
        break
    fi
    echo "sleep ... wait for output file"
    sleep 0.1
done

cksum /tmp/ws_test/send/20M_file
cksum /tmp/ws_test/receive/20M_file

# Give some time to ws receive to terminate
sleep 2

# Cleanup
cleanup_and_exit 0
