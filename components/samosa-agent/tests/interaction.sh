#!/bin/sh
set -eu
agent=$1
sources=$2
inference=$3
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-s02-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf 'Access code is 7319.' > "$fixture/root/code.txt"
registration=$(printf '%s' '{"contract_version":1,"op":"register_text","path":"code.txt"}' | "$sources" --root "$fixture/root" --state "$fixture/state")
id=$(printf '%s\n' "$registration" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
# JSON argv is host configuration; escape paths before passing directly to exec.
quote() { printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
source_args=$(printf '["%s","--state","%s"]' "$(quote "$sources")" "$(quote "$fixture/state")")
infer_args=$(printf '["%s"]' "$(quote "$inference")")
result=$(printf '{"contract_version":1,"op":"question","question":"What is the code?","source_id":"%s","version":"%s","start_byte":0,"end_byte":20}' "$id" "$id" | "$agent" --sources-command "$source_args" --inference-command "$infer_args")
printf '%s\n' "$result" | grep -q 'Access code is 7319.'
printf '%s\n' "$result" | grep -q 'Fake answer; native contract test only.'
printf '%s\n' "$result" | grep -q '"citations"'
printf '%s\n' "$result" | grep -q '"version":"'"$id"'"'
printf '%s\n' "$result" | grep -q '"reserved_output_tokens":256'
printf 'PASS native separate Sources/Agent/Inference: bounded evidence question and pinned citation\n'
# Durable timed orchestration uses public correlated reads and child operations.
created=$(printf '%s' '{"contract_version":1,"op":"create"}' | "$agent" --state "$fixture/agent" --owner qa)
conversation=$(printf '%s\n' "$created" | sed -n 's/.*"conversation_id":"\([0-9a-f]*\)".*/\1/p')
test ${#conversation} -eq 48
infer_args=$(printf '["%s","--admission-state","%s","--owner","qa"]' "$(quote "$inference")" "$(quote "$fixture/inference")")
request=$(printf '{"contract_version":1,"op":"submit","conversation_id":"%s","expected_revision":0,"operation_id":"timed-evidence","question":"What is the code?","source_id":"%s","version":"%s","start_byte":0,"end_byte":20,"timeout_ms":10000}' "$conversation" "$id" "$id")
result=$(printf '%s' "$request" | "$agent" --state "$fixture/agent" --owner qa --sources-command "$source_args" --inference-command "$infer_args")
printf '%s\n' "$result" | grep -q '"status":"completed"'
printf '%s\n' "$result" | grep -q '"kind":"source_read_text","status":"completed","cancellation":"not_requested"'
if printf '%s\n' "$result" | grep -Fq '\"parent_operation_id\"'; then exit 1; fi
retry=$(printf '%s' "$request" | "$agent" --state "$fixture/agent" --owner qa)
test "$retry" = "$result"
printf 'PASS native timed separate Sources/Agent/Inference: correlated child read, stable evidence and offline exact retry\n'
