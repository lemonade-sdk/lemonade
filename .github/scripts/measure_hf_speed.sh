#!/usr/bin/env bash
# Usage: bash measure_hf_speed.sh

# Prints Hugging Face download speed for this runner.
# The tests download several GB from Hugging Face, so a slow day shows up as a job timeout.
# this line lets a timeout be told apart from a hung test. Never fails the job.

url="https://huggingface.co/unsloth/gemma-3-270m-it-GGUF/resolve/c90975dbd40c0c7b275fefaae758c3415c906238/gemma-3-270m-it-UD-IQ2_M.gguf"
auth=()
if [ -n "${HF_TOKEN:-}" ]; then
    auth=(-H "Authorization: Bearer $HF_TOKEN")
fi
headers="$(mktemp)"
result=$(curl -sSL -o /dev/null -D "$headers" --max-time 60 "${auth[@]}" \
    -w '%{size_download} %{time_total} %{http_code} %{remote_ip}' "$url")
echo "$result" | awk '{
    mb = $1 / 1e6
    printf "Hugging Face: %.1f MB in %.1f s = %.2f MB/s (HTTP %s from %s)\n", mb, $2, ($2 > 0 ? mb / $2 : 0), $3, $4
}'
grep -i '^x-amz-cf-pop\|^x-cache' "$headers" | tr -d '\r'
rm -f "$headers"
exit 0
