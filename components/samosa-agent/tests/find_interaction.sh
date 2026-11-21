#!/bin/sh
set -eu
agent=$1
sources=$2
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-find-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root" "$fixture/root/a" "$fixture/root/b"
printf 'Code 7319, first document.\n' > "$fixture/root/a/duplicate.txt"
printf 'Code 7319, second document.\n' > "$fixture/root/b/duplicate.txt"
printf 'Unrelated document.\n' > "$fixture/root/other.txt"
create=$(printf '%s' '{"contract_version":1,"op":"index_create","path":"","operation_id":"create"}' | "$sources" --root "$fixture/root" --state "$fixture/state")
job=$(printf '%s\n' "$create" | sed -n 's/.*"job_id":"\([0-9a-f]*\)".*/\1/p' | head -1)
test ${#job} -eq 64
for operation in step1 step2; do
  printf '{"contract_version":1,"op":"index_step","job_id":"%s","operation_id":"%s","max_items":4}' "$job" "$operation" | "$sources" --root "$fixture/root" --state "$fixture/state" > "$fixture/step"
done
# Captured evidence survives loss of the live folder. Agent has only public argv.
find "$fixture/root" -depth -delete
quote() { printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
source_args=$(printf '["%s","--state","%s"]' "$(quote "$sources")" "$(quote "$fixture/state")")
result=$(printf '{"contract_version":1,"op":"find_files","job_id":"%s","query":"7319","max_inspect":3}' "$job" | "$agent" --sources-command "$source_args")
printf '%s\n' "$result" | grep -q '"verified_selections":2'
printf '%s\n' "$result" | grep -q 'a/duplicate.txt'
printf '%s\n' "$result" | grep -q 'b/duplicate.txt'
printf '%s\n' "$result" | grep -q '"exhaustive_current_filesystem":false'
printf '%s\n' "$result" | grep -q '"kind":"literal"'
printf '%s\n' "$result" | grep -q '"content_coverage":"partial"'
printf 'PASS public native file-finding pipeline: duplicate names, pinned literal evidence after live folder deletion, no model\n'
