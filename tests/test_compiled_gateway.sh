#!/bin/sh
set -eu

GATEWAY=${SAMOSA_COMPILED_GATEWAY:-./samosa-gateway}
JOBSD=${SAMOSA_COMPILED_JOBSD:-./samosa-jobsd}
BACKEND=${SAMOSA_FAKE_BACKEND:-./test_fake_openai_backend}
ROOT=$(CDPATH= cd -- "$(dirname "$0")/.." && pwd)
FS_SIDECAR=${SAMOSA_FS:-"$ROOT/build/samosa-fs"}
EXTRACTOR=${SAMOSA_EXTRACT:-"$ROOT/build/samosa-extract"}
TMP=$(mktemp -d "${TMPDIR:-/tmp}/samosa-compiled-gateway.XXXXXX")
HOME_DIR="$TMP/home"
PORT=18977
BACKEND_PORT=18978
PID=""
PID2=""

cleanup() {
  [ -z "$PID" ] || kill "$PID" 2>/dev/null || true
  [ -z "$PID" ] || wait "$PID" 2>/dev/null || true
  [ -z "$PID2" ] || kill "$PID2" 2>/dev/null || true
  [ -z "$PID2" ] || wait "$PID2" 2>/dev/null || true
  /bin/rm -rf "$TMP"
}
trap cleanup EXIT HUP INT TERM

mkdir -p "$HOME_DIR/models/ornith-9b"
printf 'fixture\n' >"$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf"
/bin/mkdir -p "$HOME_DIR/models/bonsai-27b-1bit"
printf 'fixture\n' >"$HOME_DIR/models/bonsai-27b-1bit/Bonsai-27B-Q1_0.gguf"
printf 'mmproj-fixture\n' >"$HOME_DIR/bonsai-mmproj.gguf"
printf 'ornith\n' >"$HOME_DIR/model-backend"
printf '<!doctype html><title>Compiled Samosa</title>\n' >"$TMP/app.html"
printf 'png\n' >"$TMP/logo.png"
/bin/mkdir "$TMP/files"
printf "Titli vaccination record, rabies booster 2026.\n" >"$TMP/files/cat-medical-note.txt"
printf "Miso vaccination record.\n" >"$TMP/files/miso-record.txt"
printf "Cafe total 4.50\n" >"$TMP/files/receipt-b.txt"
/bin/mkdir "$TMP/interlock-files"
printf "First interlock receipt.\n" >"$TMP/interlock-files/a.txt"
printf "Second interlock receipt.\n" >"$TMP/interlock-files/b.txt"
/bin/mkdir "$TMP/image-files"
/bin/cp "$ROOT/assets/samosa-chat.png" "$TMP/image-files/two.png"
/bin/mkdir -p "$HOME_DIR/jobs/review-native/results"
printf 'Coffee Shop\nTotal 8.37\n' >"$TMP/files/receipt.txt"
printf '{"unit_id":"u1","status":"review_required","input_path":"%s","extracted":{"merchant":"Coffee","total":8.0}}\n' \
  "$TMP/files/receipt.txt" >"$HOME_DIR/jobs/review-native/results/output.jsonl"
printf '{"unit_id":"u2","status":"passed","extracted":{"merchant":"Done"}}\n' \
  >>"$HOME_DIR/jobs/review-native/results/output.jsonl"
/bin/mkdir "$TMP/slow"
printf '%s\n' '#!/bin/sh' \
  'last=""; for arg do last=$arg; done' \
  'case "$last" in' \
  '  */slow) printf "%s\\n" "$$" >"'$TMP'/slow-sidecar.pid"; exec /bin/sleep 30 ;;' \
  'esac' \
  'exec "'$FS_SIDECAR'" "$@"' >"$TMP/samosa-fs-wrapper"
/bin/chmod +x "$TMP/samosa-fs-wrapper"
/usr/bin/printf '%s\n' '#!/bin/sh' \
  'if [ "$1" = "--json-pages" ]; then' \
  '  /usr/bin/printf "%s %s\n" "$3" "$4" >>"$SAMOSA_EXTRACT_CALLS"' \
  '  case "$3" in' \
  '    1) /usr/bin/printf '\''%s\n'\'' '\''{"ok":true,"text_layer":true,"page_count":3,"page_start":1,"page_end":1,"text":"FIRST PAGE TITLE"}'\'' ;;' \
  '    3) /usr/bin/printf '\''%s\n'\'' '\''{"ok":true,"text_layer":true,"page_count":3,"page_start":3,"page_end":3,"text":"FINAL AFFILIATION"}'\'' ;;' \
  '    *) /usr/bin/printf '\''%s\n'\'' '\''{"ok":true,"text_layer":true,"page_count":3,"page_start":2,"page_end":2,"text":"MIDDLE PAGE BODY"}'\'' ;;' \
  '  esac' \
  '  exit 0' \
  'fi' \
  'exec "$SAMOSA_REAL_EXTRACT" "$@"' >"$TMP/samosa-extract-wrapper"
/bin/chmod +x "$TMP/samosa-extract-wrapper"

# Deliberately expose no external executable through PATH. All utilities used
# below have absolute paths; the gateway/backend receive the same environment.
PATH="$TMP/no-python-bin"
/bin/mkdir "$PATH"
export PATH
if command -v python3 >/dev/null 2>&1; then
  echo "compiled gateway test PATH unexpectedly contains python3" >&2
  exit 1
fi

# The main gateway runs with NO web stub, so the public-fetch SSRF checks hit
# the real resolver (literal blocked IPs resolve offline). launchd is dry-run
# and points at a temp LaunchAgents dir so the suite never touches real launchd.
SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$PORT" \
SAMOSA_BACKEND_PORT="$BACKEND_PORT" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_FS="$TMP/samosa-fs-wrapper" \
SAMOSA_EXTRACT="$TMP/samosa-extract-wrapper" \
SAMOSA_EXTRACT_CALLS="$TMP/extract-calls.log" \
SAMOSA_REAL_EXTRACT="$EXTRACTOR" \
SAMOSA_INTERACTIVE_COOLDOWN_S=0.2 \
SAMOSA_WEB_MIN_INTERVAL=0 \
SAMOSA_LAUNCH_AGENTS_DIR="$TMP/agents" \
SAMOSA_LAUNCHD_DRYRUN=1 \
SAMOSA_BONSAI_MMPROJ="$HOME_DIR/bonsai-mmproj.gguf" \
"$GATEWAY" >"$TMP/gateway.log" 2>&1 &
PID=$!

i=0
while [ "$i" -lt 100 ]; do
  health=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
  printf '%s' "$health" | /usr/bin/grep -q '"ready":true' && break
  kill -0 "$PID" 2>/dev/null || { /bin/cat "$TMP/gateway.log" >&2; exit 1; }
  /bin/sleep 0.05
  i=$((i + 1))
done
printf '%s' "$health" | /usr/bin/grep -q '"compiled":true'
printf '%s' "$health" | /usr/bin/grep -q '"ready":true'
status=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/internal/v1/status")
printf '%s' "$status" | /usr/bin/grep -q '"interactive_active":false'
printf '%s' "$status" | /usr/bin/grep -q '"interactive_cooldown_seconds":0.200'

# Static web app + logo are served (coverage moved here from the retired Python
# tests/test_gateway_web.py when Gate 11 removed the Python gateway).
app_page=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/")
printf '%s' "$app_page" | /usr/bin/grep -q 'Compiled Samosa'
/usr/bin/curl -fsS -o /dev/null -w '%{http_code}' "http://127.0.0.1:$PORT/assets/samosa-chat.png" | /usr/bin/grep -q '200'

# Bonsai reports image support only when its mmproj vision pack is present (the
# fixture above); Ornith never does.
backends=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/v1/backends")
printf '%s' "$backends" | /usr/bin/grep -q '"id":"bonsai","label":"Bonsai 27B 1-bit","model":"bonsai-27b-1bit","supports_images":true'
printf '%s' "$backends" | /usr/bin/grep -q '"id":"ornith","label":"Ornith 9B","model":"ornith-1.0-9b","supports_images":false'

reply=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data-binary '{"messages":[{"role":"user","content":"hello"}],"stream":false}')
printf '%s' "$reply" | /usr/bin/grep -q 'compiled reply'

report=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"report what is here\",\"folder\":\"$TMP/files\"}")
printf '%s' "$report" | /usr/bin/grep -q '"type":"report"'
printf '%s' "$report" | /usr/bin/grep -q '"type":"done"'

# Phase JI find: model triages every filename (Phase A), the verify loop reads
# content and ends with a structured finish() result card (JI.2/JI.4/JI.5). No
# C keyword scoring, no canned question, no prose "answer" as the ending.
find=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find my cat medical records; my cat is named Titli\",\"folder\":\"$TMP/files\"}")
printf '%s' "$find" | /usr/bin/grep -q '"type":"triage_progress"'
printf '%s' "$find" | /usr/bin/grep -q '"type":"index_complete"'
printf '%s' "$find" | /usr/bin/grep -q '"type":"skim_progress"'
printf '%s' "$find" | /usr/bin/grep -q '"tool":"fs_read_text"'
printf '%s' "$find" | /usr/bin/grep -q '"type":"result"'
printf '%s' "$find" | /usr/bin/grep -q 'cat-medical-note.txt'
printf '%s' "$find" | /usr/bin/grep -q 'Titli vaccination record'
printf '%s' "$find" | /usr/bin/grep -q '"type":"done"'
if printf '%s' "$find" | /usr/bin/grep -qi 'what is your pet'; then
  echo "compiled find emitted the demolished canned pet question (RC2)" >&2
  exit 1
fi
if printf '%s' "$find" | /usr/bin/grep -q 'samosa_tool'; then
  echo "compiled find leaked tool protocol" >&2
  exit 1
fi
FIND_JOB=$(printf '%s' "$find" | /usr/bin/sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p' | /usr/bin/head -1)
[ -n "$FIND_JOB" ]
# Durable state persisted: Phase A verdicts, the loop conversation, the result.
[ -f "$HOME_DIR/jobs/$FIND_JOB/verdicts.jsonl" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/skim.jsonl" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/convo.json" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/result.json" ]
/usr/bin/grep -q '"verdict":' "$HOME_DIR/jobs/$FIND_JOB/verdicts.jsonl"
# skim index is the owner's dictionary: filename -> first lines of content.
/usr/bin/grep -q '"first_lines":"Titli vaccination record' "$HOME_DIR/jobs/$FIND_JOB/skim.jsonl"

# Pause == resume (JI.6): a model-authored question pauses; the answer re-enters
# the loop as the tool result. The finish only fires when run-1's read result
# ("Cafe total") survived into the resumed conversation — a live RC4 lock.
paused=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find my receipt\",\"folder\":\"$TMP/files\"}")
printf '%s' "$paused" | /usr/bin/grep -q '"type":"await_user"'
printf '%s' "$paused" | /usr/bin/grep -q 'Which receipt'
JOB_ID=$(printf '%s' "$paused" | /usr/bin/sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p' | /usr/bin/head -1)
[ -n "$JOB_ID" ]
resumed=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/answer" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"job_id\":\"$JOB_ID\",\"answer\":\"the cafe one\"}")
printf '%s' "$resumed" | /usr/bin/grep -q '"type":"result"'
printf '%s' "$resumed" | /usr/bin/grep -q 'receipt-b.txt'
printf '%s' "$resumed" | /usr/bin/grep -q '"type":"done"'

review=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/review" \
  -H 'Content-Type: application/json' --data-binary '{"job_id":"review-native"}')
printf '%s' "$review" | /usr/bin/grep -q '"pending":1'
printf '%s' "$review" | /usr/bin/grep -q 'Coffee Shop'
corrected=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/review/correct" \
  -H 'Content-Type: application/json' \
  --data-binary '{"job_id":"review-native","item":{"unit_id":"u1"},"fields":{"merchant":"Coffee Shop","total":8.37}}')
printf '%s' "$corrected" | /usr/bin/grep -q '"pending":0'
/usr/bin/grep -q '"status":"passed"' "$HOME_DIR/jobs/review-native/results/output.jsonl"
/usr/bin/grep -q '"merchant":"Coffee Shop"' "$HOME_DIR/jobs/review-native/results/output.jsonl"
[ "$(/usr/bin/wc -l <"$HOME_DIR/jobs/review-native/results/output.jsonl" | /usr/bin/tr -d ' ')" = 2 ]

definition="{\"job\":{\"job_id\":\"native-definition\",\"input\":{\"folder\":\"$TMP/files\"},\"instruction\":\"Extract merchant and total.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"merchant\":{\"type\":\"string\"},\"total\":{\"type\":\"number\"}}},\"output\":{\"dir\":\"$TMP/definition-out\"}}}"
preview=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/preview" \
  -H 'Content-Type: application/json' --data-binary "$definition")
printf '%s' "$preview" | /usr/bin/grep -q '"sample_count":1'
[ -f "$TMP/definition-out/preview/output.jsonl" ]
[ ! -f "$TMP/definition-out/output.jsonl" ]
expanded=$(printf '%s' "$definition" | /usr/bin/sed 's/}$/,"expanded":true}/')
preview3=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/preview" \
  -H 'Content-Type: application/json' --data-binary "$expanded")
printf '%s' "$preview3" | /usr/bin/grep -q '"sample_count":3'
run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$definition")
printf '%s' "$run" | /usr/bin/grep -q '"type":"item_complete"'
printf '%s' "$run" | /usr/bin/grep -q '"model_call_seconds":'
printf '%s' "$run" | /usr/bin/grep -q '"active_inference_seconds":'
printf '%s' "$run" | /usr/bin/grep -q '"type":"done"'
[ -f "$TMP/definition-out/output.jsonl" ]
/usr/bin/grep -q '"merchant":"Cafe"' "$TMP/definition-out/output.jsonl"

interlock_definition="{\"job\":{\"job_id\":\"native-definition-interlock\",\"input\":{\"folder\":\"$TMP/interlock-files\"},\"instruction\":\"Interlock definition probe.\",\"resources\":{\"pause_when_user_active\":true},\"output_schema\":{\"type\":\"object\",\"properties\":{\"merchant\":{\"type\":\"string\"},\"total\":{\"type\":\"number\"}}},\"output\":{\"dir\":\"$TMP/definition-interlock-out\"}}}"
/usr/bin/curl -sS -N -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$interlock_definition" \
  >"$TMP/interlock.sse" &
INTERLOCK_CURL=$!
i=0
while [ "$i" -lt 100 ]; do
  status=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/internal/v1/status" 2>/dev/null || true)
  printf '%s' "$status" | /usr/bin/grep -q '"inference_busy":true' && break
  /bin/kill -0 "$INTERLOCK_CURL" 2>/dev/null || { /bin/cat "$TMP/interlock.sse" >&2; exit 1; }
  /bin/sleep 0.02
  i=$((i + 1))
done
printf '%s' "$status" | /usr/bin/grep -q '"inference_busy":true'
/usr/bin/curl -sS -X POST "http://127.0.0.1:$PORT/v1/chat/completions" \
  -H 'Content-Type: application/json' \
  --data-binary '{"messages":[{"role":"user","content":"slow interactive probe"}],"stream":false}' \
  >"$TMP/interactive-chat.out" &
CHAT_CURL=$!
i=0
while [ "$i" -lt 100 ]; do
  status=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/internal/v1/status" 2>/dev/null || true)
  printf '%s' "$status" | /usr/bin/grep -q '"interactive_active":true' && break
  /bin/kill -0 "$CHAT_CURL" 2>/dev/null || { /bin/cat "$TMP/interactive-chat.out" >&2; exit 1; }
  /bin/sleep 0.02
  i=$((i + 1))
done
printf '%s' "$status" | /usr/bin/grep -q '"interactive_active":true'
wait "$CHAT_CURL"
wait "$INTERLOCK_CURL"
interlock_run=$(/bin/cat "$TMP/interlock.sse")
printf '%s' "$interlock_run" | /usr/bin/grep -q '"type":"job_paused"'
printf '%s' "$interlock_run" | /usr/bin/grep -q '"reason":"interactive_chat"'
printf '%s' "$interlock_run" | /usr/bin/grep -q '"type":"job_resumed"'
printf '%s' "$interlock_run" | /usr/bin/grep -q '"model_call_seconds":'
printf '%s' "$interlock_run" | /usr/bin/grep -q '"active_inference_seconds":'
printf '%s' "$interlock_run" | /usr/bin/grep -q '"type":"done"'
[ "$(/usr/bin/wc -l <"$TMP/definition-interlock-out/output.jsonl" | /usr/bin/tr -d ' ')" = 2 ]

budget_definition="{\"job\":{\"job_id\":\"native-definition-budget\",\"input\":{\"folder\":\"$TMP/files\"},\"instruction\":\"Require budget probe.\",\"inference\":{\"max_tokens\":1536},\"output_schema\":{\"type\":\"object\",\"properties\":{\"merchant\":{\"type\":\"string\"},\"total\":{\"type\":\"number\"}}},\"output\":{\"dir\":\"$TMP/definition-budget-out\"}}}"
budget_run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$budget_definition")
printf '%s' "$budget_run" | /usr/bin/grep -q '"type":"item_complete"'
/usr/bin/grep -q '"merchant":"Budget"' "$TMP/definition-budget-out/output.jsonl"

# A model that wraps its JSON object in a ```json markdown fence (Qwen vision
# does this) must still be recovered as a passed record, not review_required.
fenced_definition="{\"job\":{\"job_id\":\"native-definition-fenced\",\"input\":{\"folder\":\"$TMP/files\"},\"instruction\":\"Fenced JSON probe.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"merchant\":{\"type\":\"string\"},\"total\":{\"type\":\"number\"}}},\"output\":{\"dir\":\"$TMP/definition-fenced-out\"}}}"
fenced_run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$fenced_definition")
printf '%s' "$fenced_run" | /usr/bin/grep -q '"type":"item_complete"'
/usr/bin/grep -q '"status":"passed"' "$TMP/definition-fenced-out/output.jsonl"
/usr/bin/grep -q '"merchant":"Fenced"' "$TMP/definition-fenced-out/output.jsonl"
if /usr/bin/grep -q 'invalid_model_output' "$TMP/definition-fenced-out/output.jsonl"; then
  echo "fenced JSON was not recovered (review_required)" >&2; exit 1
fi

# With a text-only backend active (ornith), an image unit must be queued for
# review with a clear reason, not sent to a blind model.
guard_definition="{\"job\":{\"job_id\":\"native-definition-image-guard\",\"input\":{\"folder\":\"$TMP/image-files\"},\"instruction\":\"Image definition probe.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"people\":{\"type\":\"integer\"}}},\"output\":{\"dir\":\"$TMP/definition-image-guard-out\"}}}"
guard_run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$guard_definition")
printf '%s' "$guard_run" | /usr/bin/grep -q '"type":"item_complete"'
/usr/bin/grep -q '"reasons":\["vision_backend_required"\]' "$TMP/definition-image-guard-out/output.jsonl"

# Switch to Bonsai (its mmproj fixture makes it vision-capable); the same image
# job now reaches the backend as image_url content and passes.
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/backends/select" \
  -H 'Content-Type: application/json' --data-binary '{"backend":"bonsai"}' | /usr/bin/grep -q '"accepted":true'
i=0
while [ "$i" -lt 100 ]; do
  /usr/bin/curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | /usr/bin/grep -q '"ready":true' && break
  /bin/sleep 0.05; i=$((i + 1))
done
image_definition="{\"job\":{\"job_id\":\"native-definition-image\",\"input\":{\"folder\":\"$TMP/image-files\"},\"instruction\":\"Image definition probe.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"people\":{\"type\":\"integer\"}}},\"output\":{\"dir\":\"$TMP/definition-image-out\"}}}"
image_run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$image_definition")
printf '%s' "$image_run" | /usr/bin/grep -q '"type":"item_complete"'
/usr/bin/grep -q '"people":2' "$TMP/definition-image-out/output.jsonl"

# Restore the text backend for the remaining tests.
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/backends/select" \
  -H 'Content-Type: application/json' --data-binary '{"backend":"ornith"}' | /usr/bin/grep -q '"accepted":true'
i=0
while [ "$i" -lt 100 ]; do
  /usr/bin/curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null | /usr/bin/grep -q '"ready":true' && break
  /bin/sleep 0.05; i=$((i + 1))
done

/bin/mkdir "$TMP/pdf-files"
/usr/bin/printf '%%PDF-1.4\n' >"$TMP/pdf-files/article.pdf"
pdf_definition="{\"job\":{\"job_id\":\"native-definition-pdf-pages\",\"input\":{\"folder\":\"$TMP/pdf-files\"},\"instruction\":\"PDF first-final page probe.\",\"output_schema\":{\"type\":\"object\",\"properties\":{\"merchant\":{\"type\":\"string\"},\"total\":{\"type\":\"number\"}}},\"output\":{\"dir\":\"$TMP/definition-pdf-out\"}}}"
pdf_run=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/definition/run" \
  -H 'Content-Type: application/json' --data-binary "$pdf_definition")
printf '%s' "$pdf_run" | /usr/bin/grep -q '"type":"item_complete"'
/usr/bin/grep -q '"merchant":"PdfPages"' "$TMP/definition-pdf-out/output.jsonl"
/usr/bin/grep -q '^1 1$' "$TMP/extract-calls.log"
/usr/bin/grep -q '^3 1$' "$TMP/extract-calls.log"
if /usr/bin/grep -q '^1 5$' "$TMP/extract-calls.log"; then
  echo "definition PDF source used the old first-five-page extraction" >&2
  exit 1
fi

# find→move is out of the find loop now (JI.5): find is strictly read-only, and
# organize is a JO follow-up over the same plan/apply/undo machinery. Since find
# no longer stages moves, exercise apply/undo directly from a seeded plan.
MOVE_JOB="move-native"
/bin/mkdir -p "$HOME_DIR/jobs/$MOVE_JOB"
/usr/bin/printf '{"job_id":"%s","goal":"organize","folder":"%s","schema_version":1}\n' \
  "$MOVE_JOB" "$TMP/files" >"$HOME_DIR/jobs/$MOVE_JOB/job.json"
/usr/bin/printf '{"src":"%s/cat-medical-note.txt","dst":"%s/Archive/cat-medical-note.txt"}\n' \
  "$TMP/files" "$TMP/files" >"$HOME_DIR/jobs/$MOVE_JOB/plan.jsonl"
applied=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/apply" \
  -H 'Content-Type: application/json' --data-binary "{\"job_id\":\"$MOVE_JOB\"}")
printf '%s' "$applied" | /usr/bin/grep -q '"applied":1'
[ -f "$TMP/files/Archive/cat-medical-note.txt" ]
[ ! -f "$TMP/files/cat-medical-note.txt" ]
undone=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/undo" \
  -H 'Content-Type: application/json' --data-binary "{\"job_id\":\"$MOVE_JOB\"}")
printf '%s' "$undone" | /usr/bin/grep -q '"undone":1'
[ -f "$TMP/files/cat-medical-note.txt" ]

# --- Native background scheduler: arm, idempotency, window/battery policy, jobsd binary ---
/bin/mkdir "$TMP/sched"
printf 'shift log entry\n' >"$TMP/sched/log-a.txt"
printf 'another note\n' >"$TMP/sched/log-b.txt"

# Arm an overnight (cross-midnight) report job.
SCHED_JOB="{\"job\":{\"job_id\":\"nightly-report\",\"input\":{\"folder\":\"$TMP/sched\"}},\"window_start\":\"22:00\",\"window_end\":\"06:00\",\"missed_policy\":\"skip\"}"
armed=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' --data-binary "$SCHED_JOB")
printf '%s' "$armed" | /usr/bin/grep -q '"ok":true'
printf '%s' "$armed" | /usr/bin/grep -q '"job_id":"nightly-report"'
[ -f "$HOME_DIR/jobs/nightly-report/schedule.json" ]
[ -f "$HOME_DIR/jobs/nightly-report/job.json" ]

# Re-arming the identical definition is idempotent (no rejection).
armed_again=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' --data-binary "$SCHED_JOB")
printf '%s' "$armed_again" | /usr/bin/grep -q '"ok":true'

# Arming a changed definition under the same job_id is rejected, not replaced.
CHANGED_JOB="{\"job\":{\"job_id\":\"nightly-report\",\"input\":{\"folder\":\"$TMP/sched\"},\"instruction\":\"different\"},\"window_start\":\"22:00\",\"window_end\":\"06:00\"}"
rejected=$(/usr/bin/curl -sS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' --data-binary "$CHANGED_JOB")
printf '%s' "$rejected" | /usr/bin/grep -q '"code":"schedule_definition_changed"'

# Outside the window: defer.
outside=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobsd/once" \
  -H 'Content-Type: application/json' --data-binary '{"now_minutes":720,"on_battery":false}')
printf '%s' "$outside" | /usr/bin/grep -q '"job_id":"nightly-report","action":"defer","reason":"outside_window"'

# Inside the window but on battery (run_on_battery defaults false): defer.
battery=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobsd/once" \
  -H 'Content-Type: application/json' --data-binary '{"now_minutes":1380,"on_battery":true}')
printf '%s' "$battery" | /usr/bin/grep -q '"job_id":"nightly-report","action":"defer","reason":"on_battery"'

# Inside the window on AC: it runs to completion across midnight.
ran=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobsd/once" \
  -H 'Content-Type: application/json' --data-binary '{"now_minutes":1380,"on_battery":false}')
printf '%s' "$ran" | /usr/bin/grep -q '"job_id":"nightly-report","action":"run","reason":"inside_window"'
printf '%s' "$ran" | /usr/bin/grep -q '"status":"complete"'
/usr/bin/grep -q '"type":"scheduled_job_complete"' "$HOME_DIR/jobs/nightly-report/events.jsonl"

# One-shot polling is idempotent: a finished schedule does not run again.
again=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobsd/once" \
  -H 'Content-Type: application/json' --data-binary '{"now_minutes":1380,"on_battery":false}')
printf '%s' "$again" | /usr/bin/grep -q '"job_id":"nightly-report","action":"defer"'

# The launchd plist points at the compiled samosa-jobsd one-shot.
plist=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/v1/jobs/launchd-plist")
printf '%s' "$plist" | /usr/bin/grep -q 'samosa-jobsd'
printf '%s' "$plist" | /usr/bin/grep -q '<string>jobsd-once</string>'

# The standalone compiled daemon runs an armed job with python unavailable and no
# listener/backend. Arm a 24h window that ignores battery so it is time/power
# independent, then invoke the binary directly.
ALWAYS_JOB="{\"job\":{\"job_id\":\"always-report\",\"input\":{\"folder\":\"$TMP/sched\"},\"resources\":{\"run_on_battery\":true}},\"window_start\":\"00:00\",\"window_end\":\"00:00\",\"missed_policy\":\"skip\"}"
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' --data-binary "$ALWAYS_JOB" | /usr/bin/grep -q '"ok":true'
jobsd_out=$(SAMOSA_HOME="$HOME_DIR" SAMOSA_FS="$TMP/samosa-fs-wrapper" "$JOBSD" jobsd-once)
printf '%s' "$jobsd_out" | /usr/bin/grep -q '"job_id":"always-report","action":"run"'
/usr/bin/grep -q '"type":"scheduled_job_complete"' "$HOME_DIR/jobs/always-report/events.jsonl"

# --- Missed-window policy: skip retires, run_next_start catches up ---
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"job\":{\"job_id\":\"missed-skip\",\"input\":{\"folder\":\"$TMP/sched\"}},\"window_start\":\"22:00\",\"window_end\":\"06:00\",\"missed_policy\":\"skip\"}" \
  | /usr/bin/grep -q '"ok":true'
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/schedule/arm" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"job\":{\"job_id\":\"missed-run\",\"input\":{\"folder\":\"$TMP/sched\"}},\"window_start\":\"22:00\",\"window_end\":\"06:00\",\"missed_policy\":\"run_next_start\"}" \
  | /usr/bin/grep -q '"ok":true'
# now=12:00 outside the window, now_epoch far in the future => both windows expired.
missed=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobsd/once" \
  -H 'Content-Type: application/json' \
  --data-binary '{"now_minutes":720,"on_battery":false,"now_epoch":4102444800}')
printf '%s' "$missed" | /usr/bin/grep -q '"job_id":"missed-skip","action":"defer","reason":"window_expired"'
printf '%s' "$missed" | /usr/bin/grep -q '"job_id":"missed-run","action":"run","reason":"missed_window"'
/usr/bin/grep -q '"type":"scheduled_job_complete"' "$HOME_DIR/jobs/missed-run/events.jsonl"

# --- launchd lifecycle (dry-run, temp LaunchAgents dir) ---
/usr/bin/curl -fsS "http://127.0.0.1:$PORT/v1/jobs/launchd/status" | /usr/bin/grep -q '"installed":false'
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/launchd/install" | /usr/bin/grep -q '"ok":true'
[ -f "$TMP/agents/com.samosa.jobsd.plist" ]
/usr/bin/grep -q '<string>jobsd-once</string>' "$TMP/agents/com.samosa.jobsd.plist"
/usr/bin/curl -fsS "http://127.0.0.1:$PORT/v1/jobs/launchd/status" | /usr/bin/grep -q '"installed":true'
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/launchd/uninstall" | /usr/bin/grep -q '"removed":true'
[ ! -f "$TMP/agents/com.samosa.jobsd.plist" ]

# --- Public-fetch SSRF + URL validation (real resolver; blocked IPs resolve offline) ---
pub() { /usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/public-inputs/update" \
  -H 'Content-Type: application/json' --data-binary "$1"; }
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://127.0.0.1/x"]}')" \
  | /usr/bin/grep -q '"error":"blocked non-public address"'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://169.254.169.254/latest/meta-data/"]}')" \
  | /usr/bin/grep -q '"error":"blocked non-public address"'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://10.0.0.5/"]}')" \
  | /usr/bin/grep -q '"error":"blocked non-public address"'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://[::1]/"]}')" \
  | /usr/bin/grep -q '"error":"blocked non-public address"'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://example.com:8080/"]}')" \
  | /usr/bin/grep -q '"error":"non-standard URL ports are blocked"'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["ftp://example.com/"]}')" \
  | /usr/bin/grep -q 'only public http'
printf '%s' "$(pub '{"job_id":"ssrf","urls":["http://user:pass@example.com/"]}')" \
  | /usr/bin/grep -q '"error":"credentials in URLs are not allowed"'
# no items written for any rejected URL
[ ! -d "$HOME_DIR/jobs/ssrf/public/items" ] || [ -z "$(/bin/ls -A "$HOME_DIR/jobs/ssrf/public/items")" ]

# --- Public-input change-state, robots, and HTML extraction (stubbed transport) ---
/bin/mkdir "$TMP/stub" "$TMP/localdoc"
printf 'my resume\n' >"$TMP/localdoc/resume.txt"
printf '<html><head><title>Careers</title></head><body><script>secret()</script><h1>Roles</h1><p>Engineer &amp; Designer</p><style>.x{}</style></body></html>' \
  >"$TMP/stub/http-example-com-jobs.html"
printf 'User-agent: *\nDisallow: /private\nAllow: /\n' >"$TMP/stub/robots.txt"
STUB_PORT=18981
STUB_BACKEND_PORT=18982
SAMOSA_HOME="$HOME_DIR" \
SAMOSA_PORT="$STUB_PORT" \
SAMOSA_BACKEND_PORT="$STUB_BACKEND_PORT" \
SAMOSA_APP_HTML="$TMP/app.html" \
SAMOSA_APP_LOGO="$TMP/logo.png" \
SAMOSA_BONSAI_SERVER="$BACKEND" \
SAMOSA_ORNITH_MODEL="$HOME_DIR/models/ornith-9b/Ornith-1.0-9B-Q4_K_M.gguf" \
SAMOSA_FS="$TMP/samosa-fs-wrapper" \
SAMOSA_WEB_STUB_DIR="$TMP/stub" \
SAMOSA_WEB_MIN_INTERVAL=0 \
"$GATEWAY" >"$TMP/gateway-stub.log" 2>&1 &
PID2=$!
i=0
while [ "$i" -lt 100 ]; do
  h=$(/usr/bin/curl -fsS "http://127.0.0.1:$STUB_PORT/healthz" 2>/dev/null || true)
  printf '%s' "$h" | /usr/bin/grep -q '"ready":true' && break
  kill -0 "$PID2" 2>/dev/null || { /bin/cat "$TMP/gateway-stub.log" >&2; exit 1; }
  /bin/sleep 0.05; i=$((i + 1))
done
spub() { /usr/bin/curl -fsS -X POST "http://127.0.0.1:$STUB_PORT/v1/jobs/public-inputs/update" \
  -H 'Content-Type: application/json' --data-binary "$1"; }
# first fetch: new, exactly one changed unit, HTML script/style stripped, entity decoded
first=$(spub '{"job_id":"watch","urls":["http://example.com/jobs"]}')
printf '%s' "$first" | /usr/bin/grep -q '"checked":1,"changed":1'
printf '%s' "$first" | /usr/bin/grep -q '"status":"new"'
printf '%s' "$first" | /usr/bin/grep -q '"title":"Careers"'
if printf '%s' "$first" | /usr/bin/grep -q 'secret'; then echo "html extraction leaked script text" >&2; exit 1; fi
[ "$(/bin/ls "$HOME_DIR/jobs/watch/public/items"/*.txt | /usr/bin/wc -l | /usr/bin/tr -d ' ')" = 1 ]
/usr/bin/grep -q 'Engineer & Designer' "$HOME_DIR/jobs/watch/public/items"/*.txt
# repeat: unchanged, zero new units
printf '%s' "$(spub '{"job_id":"watch","urls":["http://example.com/jobs"]}')" | /usr/bin/grep -q '"checked":1,"changed":0'
# change the page: exactly one new unit
printf '<html><head><title>Careers</title></head><body><p>Two new roles</p></body></html>' \
  >"$TMP/stub/http-example-com-jobs.html"
printf '%s' "$(spub '{"job_id":"watch","urls":["http://example.com/jobs"]}')" | /usr/bin/grep -q '"changed":1'
[ "$(/bin/ls "$HOME_DIR/jobs/watch/public/items"/*.txt | /usr/bin/wc -l | /usr/bin/tr -d ' ')" = 2 ]
# state.json holds exactly one entry for the URL (no duplicate keys)
[ "$(/usr/bin/grep -o 'http://example.com/jobs' "$HOME_DIR/jobs/watch/public/state.json" | /usr/bin/wc -l | /usr/bin/tr -d ' ')" = 1 ]
# robots.txt disallows /private
printf '%s' "$(spub '{"job_id":"watch","urls":["http://example.com/private/listing"]}')" \
  | /usr/bin/grep -q 'robots.txt disallows'
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$STUB_PORT/v1/shutdown" >/dev/null
wait "$PID2"; PID2=""

/usr/bin/curl -sS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"report what is here\",\"folder\":\"$TMP/slow\"}" \
  >"$TMP/slow-result" 2>/dev/null &
SLOW_CURL=$!
i=0
while [ "$i" -lt 100 ] && [ ! -s "$TMP/slow-sidecar.pid" ]; do /bin/sleep 0.02; i=$((i + 1)); done
[ -s "$TMP/slow-sidecar.pid" ]
SIDE_PID=$(/bin/cat "$TMP/slow-sidecar.pid")
/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/kill" >/dev/null
wait "$SLOW_CURL" 2>/dev/null || true
if /bin/kill -0 "$SIDE_PID" 2>/dev/null; then
  echo "kill route left a Jobs sidecar running" >&2
  exit 1
fi
wait "$PID"
PID=""
echo "compiled gateway without python: PASS"
