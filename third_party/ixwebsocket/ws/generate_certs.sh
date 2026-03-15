#!/bin/bash

set -eo pipefail


generate_key() {
    local path=${1}
    local base=${2}
    local type=${3:-'rsa'} # "ec" or "rsa"

    mkdir -p ${path}
    if [[ "${type}" == "rsa" ]]; then
        openssl genrsa -out "${path}/${base}-key.pem"
    elif [[ "${type}" == "ec" ]]; then
        openssl ecparam -genkey -param_enc named_curve -name prime256v1 -out "${path}/${base}-key.pem"
    else
        echo "Error: usage: type (param \$2) should be 'rsa' or 'ec'" >&2 && exit 1
    fi
    echo "generated ${path}/${base}-key.pem"
}

generate_ca() {
    local path="${1}"
    local base="${2:-'root-ca'}"
    local type="${3:-'rsa'}" # "ec" or "rsa"
    local org="${4:-'/O=machinezone/O=IXWebSocket'}"

    mkdir -p ${path}

    generate_key "${path}" "${base}" "${type}"

    openssl req -new -x509 -sha256 -days 3650 \
        -reqexts v3_req -extensions v3_ca \
        -subj "${org}/CN=${base}" \
        -key "${path}/${base}-key.pem" \
        -out "${path}/${base}-crt.pem"

    echo "generated ${path}/${base}-crt.pem"
}

generate_cert() {
    local path="$1"
    local base="$2"
    local cabase="$3"
    local type="${4:-'rsa'}" # "ec" or "rsa"
    local org="${5:-'/O=machinezone/O=IXWebSocket'}"
    local san="${6:-'DNS:localhost,DNS:127.0.0.1'}"

    mkdir -p ${path}

    generate_key "${path}" "${base}" "${type}"

    openssl req -new -sha256 \
        -key "${path}/${base}-key.pem" \
        -subj "${org}/CN=${base}" \
        -out "${path}/${base}.csr"


    if [ "${base}" == "${cabase}" ]; then
        # self-signed
        openssl x509 -req -in "${path}/${base}.csr" \
            -signkey "${path}/${base}-key.pem" -days 365 -sha256 \
            -extfile <(printf "subjectAltName=${san}") \
            -outform PEM -out "${path}/${base}-crt.pem"
    else
        openssl x509 -req -in ${path}/${base}.csr \
            -CA "${path}/${cabase}-crt.pem" \
            -CAkey "${path}/${cabase}-key.pem" \
            -CAcreateserial -days 365 -sha256 \
            -extfile <(printf "subjectAltName=${san}") \
            -outform PEM -out "${path}/${base}-crt.pem"
    fi

    rm -f ${path}/${base}.csr
    echo "generated ${path}/${base}-crt.pem"
}

# main

outdir=${1:-'./.certs'}
type=${2:-'rsa'}
org=${3:-'/O=machinezone/O=IXWebSocket'}

if ! which openssl &>/dev/null; then

    if ! grep -qa -E 'docker|lxc' /proc/1/cgroup; then
        # launch a container with openssl and run this script there
        docker run --rm -i -v $(pwd):/work alpine sh -c "apk add bash openssl && /work/generate_certs.sh /work/${outdir} && chown -R $(id -u):$(id -u) /work/${outdir}"
    else
        echo "Please install openssl in this container to generate test certs, or launch outside of docker" >&2 && exit 1
    fi
else

    generate_ca   "${outdir}" "trusted-ca" "${type}" "${org}"

    generate_cert "${outdir}" "trusted-server" "trusted-ca" "${type}" "${org}"
    generate_cert "${outdir}" "trusted-client" "trusted-ca" "${type}" "${org}"

    generate_ca   "${outdir}" "untrusted-ca" "${type}" "${org}"

    generate_cert "${outdir}" "untrusted-client"  "untrusted-ca"        "${type}" "${org}"
    generate_cert "${outdir}" "selfsigned-client" "selfsigned-client" "${type}" "${org}"

fi
