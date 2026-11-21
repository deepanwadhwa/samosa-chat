#!/bin/sh
# Optional actual-model QA, using only an owned managed provider and public CLI.
set -eu
[ "$#" = 9 ] || { echo 'usage: child_native.sh DRIVER MANAGED CLI ENGINE ENGINE_SHA ENGINE_SIZE MODEL MODEL_SHA MODEL_SIZE' >&2; exit 2; }
driver=$1 managed=$2 cli=$3 engine=$4 engine_sha=$5 engine_size=$6 model=$7 model_sha=$8 model_size=$9
fixture=$(mktemp -d /tmp/samosa-child-native.XXXXXX)
printf 'Actual Agent child QA root: %s\n' "$fixture"
managed_pid=
cp "$engine" "$fixture/engine"
chmod 700 "$fixture/engine"
engine=$fixture/engine
cleanup() { if [ -n "$managed_pid" ]; then kill "$managed_pid" 2>/dev/null || true; wait "$managed_pid" 2>/dev/null || true; fi; rm -f "$fixture/credential"; }
trap cleanup EXIT HUP INT TERM
port=${SAMOSA_QA_PORT:-53559}
cat > "$fixture/host.json" <<EOF
{"contract_version":1,"profile":"llama-static-system-v1","state":"$fixture/state","engine":"$engine","engine_sha256":"$engine_sha","engine_size":$engine_size,"model":"$model","model_sha256":"$model_sha","model_size":$model_size,"context_tokens":4096,"model_alias":"agent-child-native","owner":"qa","port":$port,"expected_revision":0,"gpu_layers":99,"threads":4}
EOF
chmod 600 "$fixture/host.json"
mkfifo "$fixture/requests"
"$managed" --config "$fixture/host.json" < "$fixture/requests" > "$fixture/results" 2> "$fixture/stderr" &
managed_pid=$!
exec 3> "$fixture/requests"
attempt=0
while ! grep -q provider_started "$fixture/results"; do
    if [ ! -f "$fixture/credential" ] && [ -d "$fixture/state" ]; then
        key_file=$(find "$fixture/state" -maxdepth 1 -name 'key.*' -print | head -1)
        if [ -n "$key_file" ]; then (umask 077; cat "$key_file" > "$fixture/credential"); fi
    fi
    attempt=$((attempt+1)); [ "$attempt" -lt 150 ]; kill -0 "$managed_pid"; sleep 1
done
provider=$(sed -n 's/.*"provider_id":"\(managed-text-[a-f0-9]*\)".*/\1/p' "$fixture/results" | head -1)
[ -n "$provider" ] && [ -f "$fixture/credential" ]
"$driver" "$cli" "http://127.0.0.1:$port" "$provider" "$fixture/state/admission" "$fixture/credential" "$fixture"
rm "$fixture/credential"
exec 3>&-
attempt=0
while kill -0 "$managed_pid" 2>/dev/null; do attempt=$((attempt+1)); [ "$attempt" -lt 10 ]; sleep 1; done
wait "$managed_pid"
managed_pid=
[ -z "$(find "$fixture/state" -name 'key.*' -print)" ]
printf 'PASS actual Agent managed lifecycle: owned provider EOF shutdown and no retained credential\n'
