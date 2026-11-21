#!/bin/sh
# Requires jq/curl for public JSON/health checks; engine launch is native C.
set -eu
agent=$1 sources=$2 inference=$3 launcher=$4 engine=$5 model=$6 port=$7
qa=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-s06-XXXXXX")
pid=
stop_engine() {
    if test -n "$pid"; then
        # Only the instance launched by this test with this port/model is owned.
        if ps -p "$pid" -o command= | grep -F -- "$engine --model $model --host 127.0.0.1 --port $port" >/dev/null; then kill -TERM "$pid"; fi
        pid=
    fi
}
cleanup() { stop_engine; find "$qa" -depth -delete; }
trap cleanup EXIT HUP INT TERM
endpoint="http://127.0.0.1:$port"
if curl --max-time 1 --silent "$endpoint/health" >/dev/null 2>&1; then echo 'test port already occupied' >&2; exit 2; fi
launch() {
    pid=$("$launcher" "$qa/engine-$1.log" "$engine" --model "$model" --host 127.0.0.1 --port "$port" --ctx-size 4096 --parallel 1 --n-gpu-layers 99 --threads 4 --cache-ram 256 --ctx-checkpoints 4 --checkpoint-min-step 64 --ubatch-size 64 --reasoning off --no-webui)
    jq -n --argjson pid "$pid" --arg binary "$engine" --argjson port "$port" '{pid:$pid,binary:$binary,port:$port}' > "$qa/owned-engine.json"
    ready=0
    for attempt in $(seq 1 60); do
        if curl --max-time 1 --silent "$endpoint/health" | jq -e '.status=="ok"' >/dev/null 2>&1; then ready=1; break; fi
        if ! kill -0 "$pid" 2>/dev/null; then cat "$qa/engine-$1.log" >&2; exit 1; fi
        sleep 1
    done
    test "$ready" -eq 1
}
launch hot
mkdir "$qa/root"
printf 'Access code is 7319.' > "$qa/root/code.txt"
source_args=$(jq -cn --arg exe "$sources" --arg state "$qa/source-state" '[$exe,"--state",$state]')
infer_args=$(jq -cn --arg exe "$inference" --arg state "$qa/inference-state" --arg endpoint "$endpoint" --arg alias "$(basename "$model")" '[$exe,"--llama-endpoint",$endpoint,"--provider-id","native-s06-v1","--model-alias",$alias,"--context-tokens","4096","--admission-state",$state,"--queue-capacity","4"]')
jq -n '{contract_version:1,op:"generate",operation_id:"native-tracked",provider_id:"native-s06-v1",prompt:"The access code is 7319. Reply only with the access code.",max_output_tokens:16}' > "$qa/tracked-request.json"
"$inference" --llama-endpoint "$endpoint" --provider-id native-s06-v1 --model-alias "$(basename "$model")" --context-tokens 4096 --admission-state "$qa/inference-state" --queue-capacity 4 < "$qa/tracked-request.json" > "$qa/tracked-result.json"
jq -e '.operation_id=="native-tracked" and (.answer|contains("7319")) and .quality=="native"' "$qa/tracked-result.json" >/dev/null
printf '%s' '{"contract_version":1,"op":"register_text","path":"code.txt"}' | "$sources" --root "$qa/root" --state "$qa/source-state" > "$qa/source.json"
id=$(jq -r .source_id "$qa/source.json")
conversation=$(printf '%s' '{"contract_version":1,"op":"create"}' | "$agent" --state "$qa/agent-state" --owner alice | jq -r .conversation_id)
submit() {
    jq -n --arg conversation "$conversation" --arg operation "$1" --argjson revision "$2" --arg id "$id" --argjson selected "$3" '{contract_version:1,op:"submit",conversation_id:$conversation,expected_revision:$revision,operation_id:$operation,question:"What is the admitted access code? Reply only with the code."} + (if $selected then {source_id:$id,version:$id,start_byte:0,end_byte:20} else {} end)' | "$agent" --state "$qa/agent-state" --owner alice --sources-command "$source_args" --inference-command "$infer_args" > "$qa/$1.json"
    jq -e --arg id "$id" '.status=="completed" and (.result.answer|contains("7319")) and .result.admitted_evidence[0].version==$id' "$qa/$1.json" >/dev/null
}
submit first 0 true
find "$qa/root" -depth -delete
source_args='["/not-installed/source"]'
submit hot 2 false
jq -e '.result.usage.processed_input_tokens < .result.usage.input_tokens' "$qa/hot.json" >/dev/null
stop_engine
"$inference" --llama-endpoint "$endpoint" --provider-id native-s06-v1 --model-alias "$(basename "$model")" --context-tokens 4096 --admission-state "$qa/inference-state" --queue-capacity 4 < "$qa/tracked-request.json" > "$qa/tracked-offline.json"
cmp "$qa/tracked-result.json" "$qa/tracked-offline.json"
launch cold
submit cold 4 false
jq -e '.result.usage.processed_input_tokens==.result.usage.input_tokens' "$qa/cold.json" >/dev/null
jq -n --arg conversation "$conversation" '{contract_version:1,op:"compact",conversation_id:$conversation,expected_revision:6,operation_id:"compact"}' | "$agent" --state "$qa/agent-state" --owner alice > "$qa/compact.json"
jq -e '.status=="completed" and .result.session_reset==true and .result.revision==7' "$qa/compact.json" >/dev/null
jq -n --arg conversation "$conversation" '{contract_version:1,op:"get",conversation_id:$conversation}' | "$agent" --state "$qa/agent-state" --owner alice > "$qa/restored.json"
jq -e '(.messages|length==6) and .continuation.provider_id=="native-s06-v1" and (.continuation|has("session_id")|not) and (.derived_summary|startswith("Derived message excerpts"))' "$qa/restored.json" >/dev/null
submit compacted 7 false
jq -e --slurpfile cold "$qa/cold.json" '.result.context_policy.history_messages==1 and .result.continuation.revision==1 and .result.continuation.session_id!=$cold[0].result.continuation.session_id' "$qa/compacted.json" >/dev/null
jq -c '.result.usage' "$qa/hot.json"
printf 'PASS native hot/cold/compact: real7319, reduced hot processing, cold engine restart, zero Sources after first, retained originals/pin, reset session\n'
# Ordinary chat uses the same durable/provider path with no Sources capability.
chat=$(printf '%s' '{"contract_version":1,"op":"create"}' | "$agent" --state "$qa/chat-state" --owner alice | jq -r .conversation_id)
chat_submit() {
    jq -n --arg conversation "$chat" --arg operation "$1" --argjson revision "$2" --arg question "$3" '{contract_version:1,op:"submit_chat",conversation_id:$conversation,expected_revision:$revision,operation_id:$operation,question:$question}' | "$agent" --state "$qa/chat-state" --owner alice --inference-command "$infer_args" > "$qa/$1.json"
    jq -e '.status=="completed" and .result.citations==[] and .result.admitted_evidence==[]' "$qa/$1.json" >/dev/null
}
chat_submit chat-math 0 'What is 2+2? Reply only with the number.'
jq -e '.result.answer|contains("4")' "$qa/chat-math.json" >/dev/null
chat_submit chat-remember 2 'Remember my number 7319 for our conversation. Reply only OK.'
chat_submit chat-hot 4 'What number did I ask you to remember? Reply only with the number.'
jq -e '.result.answer|contains("7319")' "$qa/chat-hot.json" >/dev/null
stop_engine
launch chat-cold
chat_submit chat-cold 6 'What number did I ask you to remember? Reply only with the number.'
jq -e '.result.answer|contains("7319")' "$qa/chat-cold.json" >/dev/null
jq -n --arg conversation "$chat" '{contract_version:1,op:"compact",conversation_id:$conversation,expected_revision:8,operation_id:"chat-compact"}' | "$agent" --state "$qa/chat-state" --owner alice > "$qa/chat-compact.json"
jq -e '.status=="completed" and .result.revision==9' "$qa/chat-compact.json" >/dev/null
chat_submit chat-compacted 9 'What number did I ask you to remember? Reply only with the number.'
jq -e '(.result.answer|contains("7319")) and .result.context_policy.history_messages==1' "$qa/chat-compacted.json" >/dev/null
printf 'PASS real ordinary chat: arithmetic4, remembered7319 hot/cold/compact, empty evidence/citations, no Sources configured\n'
