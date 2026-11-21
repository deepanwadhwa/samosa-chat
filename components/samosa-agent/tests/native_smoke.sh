#!/bin/sh
set -eu
agent=$1
sources=$2
inference=$3
pdf_generator=$4
pdfium=$5
endpoint=$6
fixture=$(mktemp -d "${TMPDIR:-/tmp}/samosa-native-s04-smoke-XXXXXX")
trap 'find "$fixture" -depth -delete' EXIT HUP INT TERM
mkdir "$fixture/root"
printf 'Access code is 7319.' > "$fixture/root/code.txt"
"$pdf_generator" "$fixture/root/hundred.pdf"
quote() { printf '%s' "$1" | sed 's/\\/\\\\/g;s/"/\\"/g'; }
source_args=$(printf '["%s","--state","%s","--pdfium-library","%s"]' "$(quote "$sources")" "$(quote "$fixture/state")" "$(quote "$pdfium")")
infer_args=$(printf '["%s","--llama-endpoint","%s","--provider-id","ornith-native-v1","--model-alias","Ornith-1.0-9B-Q4_K_M.gguf","--context-tokens","4096","--admission-state","%s"]' "$(quote "$inference")" "$(quote "$endpoint")" "$(quote "$fixture/admission")")
for kind in text pdf; do
  if [ "$kind" = text ]; then path=code.txt; selection='"start_byte":0,"end_byte":20'; else path=hundred.pdf; selection='"start_page":97,"end_page":97'; fi
  registration=$(printf '{"contract_version":1,"op":"register_%s","path":"%s"}' "$kind" "$path" | "$sources" --root "$fixture/root" --state "$fixture/state")
  id=$(printf '%s\n' "$registration" | sed -n 's/.*"source_id":"\([0-9a-f]*\)".*/\1/p')
  test ${#id} -eq 64
  printf '{"contract_version":1,"op":"question","question":"What is the code in the selected evidence? Reply with only the code.","source_id":"%s","version":"%s",%s}' "$id" "$id" "$selection" | "$agent" --sources-command "$source_args" --inference-command "$infer_args" > "$fixture/$kind-result.json"
  answer=$(sed -n 's/^{"contract_version":1,"answer":"\([^"]*\)".*/\1/p' "$fixture/$kind-result.json")
  printf '%s\n' "$answer" | grep -q '7319'
  grep -q '"citations"' "$fixture/$kind-result.json"
  grep -q '"version":"'"$id"'"' "$fixture/$kind-result.json"
  if [ "$kind" = pdf ]; then grep -q '"start_page":97' "$fixture/$kind-result.json"; grep -q 'text_layer_only' "$fixture/$kind-result.json"; fi
  printf 'PASS real native %s answer=%s; pinned source/version/range citation\n' "$kind" "$answer"
done
