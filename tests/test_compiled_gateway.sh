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

# --- JI.8 fixture folders ---
# (d) Education scenario: diploma + junk + RC1-like name. Goal is "find my
#     education certificates" — the test asserts no event ever mentions "pet".
/bin/mkdir "$TMP/edu-files"
printf "Bachelor of Science in Computer Science, 2020\nUniversity of Example\nConferred June 15, 2020\n" >"$TMP/edu-files/diploma_bsc_2020.txt"
printf "Random notes about nothing relevant.\n" >"$TMP/edu-files/CamScanner_04-22-2024.txt"
printf "Training schedule for Q3 2026.\n" >"$TMP/edu-files/training_schedule.txt"

# (f) Sweep scenario: two planted vet record targets + junk + an image that
#     will park as ocr_unavailable (no samosa-ocr configured in this test).
/bin/mkdir "$TMP/sweep-files"
printf "Miso annual checkup 2026, all vaccinations current.\n" >"$TMP/sweep-files/miso_vet_checkup.txt"
printf "Titli rabies booster 2023, next due 2026.\n" >"$TMP/sweep-files/titli_vaccination_2023.txt"
printf "Grocery list: milk, eggs, bread.\n" >"$TMP/sweep-files/grocery_list.txt"
printf "Wallpaper gallery index.\n" >"$TMP/sweep-files/wallpaper_gallery.txt"
# A real PNG header (1x1 pixel) so samosa-fs detects it as image/png. The
# gateway's doc.read will fail (no OCR pack) → parked with ocr_unavailable.
printf '\x89PNG\r\n\x1a\n' >"$TMP/sweep-files/scan_unknown.png"
# Phase-B checkpoint fixture: one more readable file than the per-run skim
# budget.  The Continue request must add only the final row, not reread 300.
/bin/mkdir "$TMP/checkpoint-files"
i=1
while [ "$i" -le 301 ]; do printf 'checkpoint fixture %s\n' "$i" >"$TMP/checkpoint-files/file-$i.txt"; i=$((i + 1)); done
# JI.2 crash fixture: 510 rows force many 16-file triage batches.  The fake
# backend holds batch two so the test can SIGKILL at a known durable cursor.
/bin/mkdir "$TMP/triage-crash-files"
i=1
while [ "$i" -le 510 ]; do printf 'triage crash fixture %s\n' "$i" >"$TMP/triage-crash-files/file-$i.txt"; i=$((i + 1)); done
/bin/mkdir "$TMP/verify-crash-files"
printf 'durable crash probe content\n' >"$TMP/verify-crash-files/crash-probe.txt"
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
# Keeping this in a function lets the crash tests restart the real binary with
# precisely the same configuration and durable home directory.
launch_main_gateway() {
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
  SAMOSA_FAKE_PID_FILE="$TMP/fake-backend.pid" \
  SAMOSA_FAKE_TRIAGE_FIRST="$TMP/fake-triage-first" \
  SAMOSA_FAKE_TRIAGE_DELAY="$TMP/fake-triage-delay" \
  SAMOSA_FAKE_VERIFY_DELAY="$TMP/fake-verify-delay" \
  "$GATEWAY" >>"$TMP/gateway.log" 2>&1 &
  PID=$!
  i=0
  while [ "$i" -lt 100 ]; do
    health=$(/usr/bin/curl -fsS "http://127.0.0.1:$PORT/healthz" 2>/dev/null || true)
    printf '%s' "$health" | /usr/bin/grep -q '"ready":true' && return 0
    kill -0 "$PID" 2>/dev/null || { /bin/cat "$TMP/gateway.log" >&2; return 1; }
    /bin/sleep 0.05
    i=$((i + 1))
  done
  echo "main gateway did not become ready" >&2
  return 1
}
launch_main_gateway
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
printf '%s' "$find" | /usr/bin/grep -q '"type":"classify_progress"'
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
[ -f "$HOME_DIR/jobs/$FIND_JOB/classify.jsonl" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/convo.json" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/result.json" ]
[ -f "$HOME_DIR/jobs/$FIND_JOB/events.jsonl" ]
/usr/bin/grep -q '"type":"triage_progress"' "$HOME_DIR/jobs/$FIND_JOB/events.jsonl"
/usr/bin/grep -q '"type":"result"' "$HOME_DIR/jobs/$FIND_JOB/events.jsonl"
/usr/bin/grep -q '"verdict":' "$HOME_DIR/jobs/$FIND_JOB/classify.jsonl"
# Phase A assigns confidence (high|medium|low), never a hard drop (E-JI1 lesson).
/usr/bin/grep -q '"confidence":' "$HOME_DIR/jobs/$FIND_JOB/verdicts.jsonl"
if /usr/bin/grep -q '"verdict":"no"' "$HOME_DIR/jobs/$FIND_JOB/verdicts.jsonl"; then
  echo "triage still hard-drops files (should assign confidence, not exclude)" >&2
  exit 1
fi
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

# JI.2/JI.6: SIGKILL is materially different from a normal checkpoint: no
# cleanup runs.  Stop Phase A after its first durable batch, kill its child
# backend too (the parent normally owns it), then start a fresh gateway over
# the same job home.  Exactly 510 verdict rows proves it resumed at batch two
# instead of re-triaging the already durable first 16 rows.
kill_main_for_crash() {
  crash_backend=$(/bin/cat "$TMP/fake-backend.pid" 2>/dev/null || true)
  /bin/kill -KILL "$PID" 2>/dev/null || true
  wait "$PID" 2>/dev/null || true
  PID=""
  [ -z "$crash_backend" ] || /bin/kill -KILL "$crash_backend" 2>/dev/null || true
  /bin/sleep 0.05
}
/usr/bin/curl -sS -N -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find triage crash fixtures\",\"folder\":\"$TMP/triage-crash-files\"}" \
  >"$TMP/triage-crash.sse" 2>/dev/null &
TRIAGE_CRASH_CURL=$!
i=0
while [ "$i" -lt 200 ] && [ ! -f "$TMP/fake-triage-delay" ]; do
  /bin/kill -0 "$TRIAGE_CRASH_CURL" 2>/dev/null || { /bin/cat "$TMP/triage-crash.sse" >&2; exit 1; }
  /bin/sleep 0.02; i=$((i + 1))
done
[ -f "$TMP/fake-triage-first" ]
[ -f "$TMP/fake-triage-delay" ]
TRIAGE_JOB=$(/bin/ls -dt "$HOME_DIR"/jobs/job-* | /usr/bin/head -1 | /usr/bin/xargs /usr/bin/basename)
[ "$(/usr/bin/grep -c '"rel_path":' "$HOME_DIR/jobs/$TRIAGE_JOB/verdicts.jsonl")" = 16 ]
kill_main_for_crash
wait "$TRIAGE_CRASH_CURL" 2>/dev/null || true
launch_main_gateway
triage_resume=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/continue" \
  -H 'Content-Type: application/json' --data-binary "{\"job_id\":\"$TRIAGE_JOB\"}")
printf '%s' "$triage_resume" | /usr/bin/grep -q '"type":"triage_progress"'
[ "$(/usr/bin/grep -c '"rel_path":' "$HOME_DIR/jobs/$TRIAGE_JOB/verdicts.jsonl")" = 510 ]
[ "$(/usr/bin/grep -c '"rel_path":"file-1.txt"' "$HOME_DIR/jobs/$TRIAGE_JOB/verdicts.jsonl")" = 1 ]

# JI.6 Phase D crash recovery: the fake backend holds the post-read model turn.
# The durable conversation already contains the tool call and result when the
# process dies.  Continue must finish from that conversation without emitting a
# second read/tool event.
/usr/bin/curl -sS -N -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find verify crash fixture\",\"folder\":\"$TMP/verify-crash-files\"}" \
  >"$TMP/verify-crash.sse" 2>/dev/null &
VERIFY_CRASH_CURL=$!
i=0
while [ "$i" -lt 200 ] && [ ! -f "$TMP/fake-verify-delay" ]; do
  /bin/kill -0 "$VERIFY_CRASH_CURL" 2>/dev/null || { /bin/cat "$TMP/verify-crash.sse" >&2; exit 1; }
  /bin/sleep 0.02; i=$((i + 1))
done
[ -f "$TMP/fake-verify-delay" ]
VERIFY_JOB=$(/bin/ls -dt "$HOME_DIR"/jobs/job-* | /usr/bin/head -1 | /usr/bin/xargs /usr/bin/basename)
/usr/bin/grep -q 'durable crash probe content' "$HOME_DIR/jobs/$VERIFY_JOB/convo.json"
kill_main_for_crash
wait "$VERIFY_CRASH_CURL" 2>/dev/null || true
launch_main_gateway
verify_resume=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/continue" \
  -H 'Content-Type: application/json' --data-binary "{\"job_id\":\"$VERIFY_JOB\"}")
printf '%s' "$verify_resume" | /usr/bin/grep -q '"type":"result"'
printf '%s' "$verify_resume" | /usr/bin/grep -q 'crash-probe.txt'
[ "$(/usr/bin/grep -c '"type":"tool_call"' "$HOME_DIR/jobs/$VERIFY_JOB/events.jsonl")" = 1 ]
[ "$(/usr/bin/grep -c '"type":"tool_result"' "$HOME_DIR/jobs/$VERIFY_JOB/events.jsonl")" = 1 ]

# --- JI.8: Offline fixture suite (confidence contract regression locks) ---
#
# (a) Well-named target: already covered by the main find test above — it
#     asserts cat-medical-note.txt in the result with "Titli vaccination record"
#     evidence through a structured finish().
# (b) Anonymous scan / RC1+RC5 lock: already covered by the main find test —
#     verdicts.jsonl has no "verdict":"no" (confidence triage, not binary),
#     and skim.jsonl has first_lines for the text files.
# (c) Clutter exclusion: the existing find folder has miso-record.txt and
#     receipt-b.txt as junk; the finish() rejected_count=3 confirms they were
#     excluded from matches. Additional clutter names are tested in (f) below.

# (d) No-canned-question (RC2 lock): goal "find my education certificates" —
#     the word "education" contains substring "cat" which the old C scorer would
#     have treated as pet-related. Assert no event ever mentions "pet".
edu=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find my education certificates\",\"folder\":\"$TMP/edu-files\"}")
printf '%s' "$edu" | /usr/bin/grep -q '"type":"triage_progress"'
printf '%s' "$edu" | /usr/bin/grep -q '"type":"index_complete"'
printf '%s' "$edu" | /usr/bin/grep -q '"type":"skim_progress"'
printf '%s' "$edu" | /usr/bin/grep -q '"type":"classify_progress"'
printf '%s' "$edu" | /usr/bin/grep -q '"type":"result"'
printf '%s' "$edu" | /usr/bin/grep -q 'diploma_bsc_2020.txt'
printf '%s' "$edu" | /usr/bin/grep -q 'Bachelor of Science'
printf '%s' "$edu" | /usr/bin/grep -q '"type":"done"'
if printf '%s' "$edu" | /usr/bin/grep -qi 'pet'; then
  echo "education find mentioned 'pet' (RC2 regression)" >&2
  exit 1
fi
if printf '%s' "$edu" | /usr/bin/grep -qi 'what is your'; then
  echo "education find emitted a canned question (RC2 regression)" >&2
  exit 1
fi
# Confidence assigned (not binary), no hard drops.
EDU_JOB=$(printf '%s' "$edu" | /usr/bin/sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p' | /usr/bin/head -1)
[ -n "$EDU_JOB" ]
/usr/bin/grep -q '"confidence":' "$HOME_DIR/jobs/$EDU_JOB/verdicts.jsonl"
if /usr/bin/grep -q '"verdict":"no"' "$HOME_DIR/jobs/$EDU_JOB/verdicts.jsonl"; then
  echo "education triage still hard-drops files" >&2
  exit 1
fi

# JI.0: no substring router match is required. The three-way model fallback
# may select the read-only find pipeline for an implicit request.
implicit_find=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"my university diploma\",\"folder\":\"$TMP/edu-files\"}")
printf '%s' "$implicit_find" | /usr/bin/grep -q '"type":"intent","kind":"find"'
printf '%s' "$implicit_find" | /usr/bin/grep -q 'diploma_bsc_2020.txt'
printf '%s' "$implicit_find" | /usr/bin/grep -q '"type":"result"'

# (f) Sweep contract: two planted vet record targets + junk + an image file.
#     The image has no OCR sidecar → parked as ocr_unavailable. Assert:
#     - both matches present in result with evidence
#     - the image file in unreadable with its error code
#     - junk files NOT in matches
sweep=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find all vet records for my pets\",\"folder\":\"$TMP/sweep-files\"}")
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"triage_progress"'
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"index_complete"'
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"skim_progress"'
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"classify_progress"'
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"result"'
# Both targets in matches.
printf '%s' "$sweep" | /usr/bin/grep -q 'miso_vet_checkup.txt'
printf '%s' "$sweep" | /usr/bin/grep -q 'Miso annual checkup'
printf '%s' "$sweep" | /usr/bin/grep -q 'titli_vaccination_2023.txt'
printf '%s' "$sweep" | /usr/bin/grep -q 'Titli rabies booster'
# Unreadable with error code.
printf '%s' "$sweep" | /usr/bin/grep -q 'scan_unknown.png'
printf '%s' "$sweep" | /usr/bin/grep -q 'ocr_unavailable'
printf '%s' "$sweep" | /usr/bin/grep -q '"type":"done"'
# Clutter exclusion: junk files must not be in matches.
SWEEP_JOB=$(printf '%s' "$sweep" | /usr/bin/sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p' | /usr/bin/head -1)
[ -n "$SWEEP_JOB" ]
[ -f "$HOME_DIR/jobs/$SWEEP_JOB/result.json" ]
# Junk files must not appear in the matches array of result.json.
# The fake backend's finish payload has exactly {miso_vet_checkup, titli_vaccination_2023}
# in matches — verify the gateway accepted it and no extra paths leaked.
if /usr/bin/grep 'grocery_list' "$HOME_DIR/jobs/$SWEEP_JOB/result.json" | /usr/bin/grep -q '"path"'; then
  echo "sweep result included junk file grocery_list in matches" >&2
  exit 1
fi
if /usr/bin/grep 'wallpaper_gallery' "$HOME_DIR/jobs/$SWEEP_JOB/result.json" | /usr/bin/grep -q '"path"'; then
  echo "sweep result included junk file wallpaper_gallery in matches" >&2
  exit 1
fi
# Confidence contract: no hard drops, skim has entries for all readable files.
/usr/bin/grep -q '"confidence":' "$HOME_DIR/jobs/$SWEEP_JOB/verdicts.jsonl"
if /usr/bin/grep -q '"verdict":"no"' "$HOME_DIR/jobs/$SWEEP_JOB/verdicts.jsonl"; then
  echo "sweep triage still hard-drops files" >&2
  exit 1
fi
# Skim index recorded the planted targets' content.
/usr/bin/grep -q '"first_lines":"Miso annual checkup' "$HOME_DIR/jobs/$SWEEP_JOB/skim.jsonl"
/usr/bin/grep -q '"first_lines":"Titli rabies booster' "$HOME_DIR/jobs/$SWEEP_JOB/skim.jsonl"
# The parked image appears in skim with parked:true.
/usr/bin/grep -q '"parked":true' "$HOME_DIR/jobs/$SWEEP_JOB/skim.jsonl"

# JI.3/JI.6: a skim budget is an honest mechanical checkpoint. The first run
# stops at 300, /continue reads the remaining file, and durable skim rows are
# never appended a second time.
checkpoint=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/run" \
  -H 'Content-Type: application/json' \
  --data-binary "{\"goal\":\"find checkpoint fixtures\",\"folder\":\"$TMP/checkpoint-files\"}")
printf '%s' "$checkpoint" | /usr/bin/grep -q '"type":"await_continue"'
printf '%s' "$checkpoint" | /usr/bin/grep -q '"skimmed":300'
printf '%s' "$checkpoint" | /usr/bin/grep -q '"remaining":1'
CHECKPOINT_JOB=$(printf '%s' "$checkpoint" | /usr/bin/sed -n 's/.*"job_id":"\([^"]*\)".*/\1/p' | /usr/bin/head -1)
[ -n "$CHECKPOINT_JOB" ]
[ "$(/usr/bin/grep -c '"path":' "$HOME_DIR/jobs/$CHECKPOINT_JOB/skim.jsonl")" = 300 ]
checkpoint_resume=$(/usr/bin/curl -fsS -X POST "http://127.0.0.1:$PORT/v1/jobs/continue" \
  -H 'Content-Type: application/json' --data-binary "{\"job_id\":\"$CHECKPOINT_JOB\"}")
[ "$(/usr/bin/grep -c '"path":' "$HOME_DIR/jobs/$CHECKPOINT_JOB/skim.jsonl")" = 301 ]
if printf '%s' "$checkpoint_resume" | /usr/bin/grep -q '"type":"await_continue"'; then
  echo "skim resume incorrectly checkpointed after completing the final file" >&2; exit 1
fi

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
