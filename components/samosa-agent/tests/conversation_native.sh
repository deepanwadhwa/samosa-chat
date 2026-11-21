#!/bin/sh
set -eu
agent=$1
sources=$2
inference=$3
endpoint=$4
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-s05-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf 'Access code is 7319.' > "$fixture/root/code.txt"
quote() { printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
source_args=$(printf '["%s","--state","%s"]' "$(quote "$sources")" "$(quote "$fixture/source-state")")
infer_args=$(printf '["%s","--llama-endpoint","%s","--provider-id","ornith-native-v1","--model-alias","Ornith-1.0-9B-Q4_K_M.gguf","--context-tokens","4096","--admission-state","%s"]' "$(quote "$inference")" "$(quote "$endpoint")" "$(quote "$fixture/inference-state")")
registration=$(printf '%s' '{"contract_version":1,"op":"register_text","path":"code.txt"}' | "$sources" --root "$fixture/root" --state "$fixture/source-state")
id=$(printf '%s\n' "$registration" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
test ${#id} -eq 64
conversation=$(printf '%s' '{"contract_version":1,"op":"create"}' | "$agent" --state "$fixture/agent-state" --owner alice | sed -n 's/.*"conversation_id":"\([0-9a-f]*\)".*/\1/p')
test ${#conversation} -eq 48
printf '{"contract_version":1,"op":"submit","conversation_id":"%s","expected_revision":0,"operation_id":"first","question":"What is the access code? Reply only with the code.","source_id":"%s","version":"%s","start_byte":0,"end_byte":20}' "$conversation" "$id" "$id" > "$fixture/first-request.json"
"$agent" --state "$fixture/agent-state" --owner alice --sources-command "$source_args" --inference-command "$infer_args" < "$fixture/first-request.json" > "$fixture/first-result.json"
answer=$(sed -n 's/.*"result":{"revision":[0-9]*,"answer":"\([^"]*\)".*/\1/p' "$fixture/first-result.json")
printf '%s\n' "$answer" | grep -q '7319'
# New empty client supplies only Agent state/owner; no browser/session persistence.
printf '{"contract_version":1,"op":"get","conversation_id":"%s"}' "$conversation" | "$agent" --state "$fixture/agent-state" --owner alice > "$fixture/restored.json"
grep -q '"revision":2' "$fixture/restored.json"
grep -q '"admitted_evidence"' "$fixture/restored.json"
grep -q '"version":"'"$id"'"' "$fixture/restored.json"
find "$fixture/root" -depth -delete
printf '{"contract_version":1,"op":"submit","conversation_id":"%s","expected_revision":2,"operation_id":"followup","question":"Repeat the earlier admitted access code. Reply only with the code."}' "$conversation" | "$agent" --state "$fixture/agent-state" --owner alice --sources-command '["/not-installed/source"]' --inference-command "$infer_args" > "$fixture/followup.json"
answer=$(sed -n 's/.*"result":{"revision":[0-9]*,"answer":"\([^"]*\)".*/\1/p' "$fixture/followup.json")
printf '%s\n' "$answer" | grep -q '7319'
grep -q '"revision":4' "$fixture/followup.json"
"$agent" --state "$fixture/agent-state" --owner alice --sources-command '["/not-installed/source"]' --inference-command '["/not-installed/inference"]' < "$fixture/first-request.json" > "$fixture/retry.json"
cmp "$fixture/first-result.json" "$fixture/retry.json"
if printf '{"contract_version":1,"op":"get","conversation_id":"%s"}' "$conversation" | "$agent" --state "$fixture/agent-state" --owner bob > "$fixture/foreign.json"; then exit 1; fi
grep -q 'unknown_conversation' "$fixture/foreign.json"
printf '{"contract_version":1,"op":"events","conversation_id":"%s","after":0}' "$conversation" | "$agent" --state "$fixture/agent-state" --owner alice > "$fixture/events.json"
grep -q '"revision":4' "$fixture/events.json"
printf 'PASS real native conversation: CLI restart/empty client, answer7319, no Sources on followup, exact retry without dependencies, owner/event checks\n'
