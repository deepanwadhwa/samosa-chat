#!/bin/zsh
# Live demo: model generation and memory footprint, side by side.
# Requires a completed install (~/.jugnu). Usage: ./demo.sh ["prompt"]

set -u
HOME_DIR="${JUGNU_HOME:-$HOME/.jugnu}"
ENGINE="$HOME_DIR/bin/qwen36b"
MODEL="$HOME_DIR/model"
TOKENIZER="$HOME_DIR/tokenizer_qwen36.json"
PROMPT="${1:-Explain in three short paragraphs why the sky is blue.}"

[ -x "$ENGINE" ] || { echo "run the installer first (jugnu doctor)"; exit 1; }

OUT=$(mktemp) ; ERR=$(mktemp)
MODEL_GB=$(du -sh "$MODEL" | awk '{print $1}')

SNAP="$MODEL" "$ENGINE" --chat "$PROMPT" --no-thinking --stream --tokens 220 \
  --tokenizer "$TOKENIZER" >"$OUT" 2>"$ERR" &
PID=$!

bar() { # bar <used> <total> <width>
  local used=$1 total=$2 width=$3 filled
  filled=$(( used * width / total )); [ $filled -gt $width ] && filled=$width
  printf '%s%s' "$(printf '█%.0s' $(seq 1 $((filled>0?filled:1))))" \
                "$(printf '░%.0s' $(seq 1 $((width-filled))))"
}

while kill -0 $PID 2>/dev/null; do
  RSS_KB=$(ps -o rss= -p $PID 2>/dev/null | tr -d ' ')
  RSS_GB=$(awk -v k="${RSS_KB:-0}" 'BEGIN{printf "%.2f", k/1048576}')
  SWAP=$(sysctl -n vm.swapusage | awk '{print $6}')
  TOTAL_GB=$(( $(sysctl -n hw.memsize) / 1073741824 ))
  clear
  printf '  jugnu demo — Qwen3.6-35B-A3B (int4) on this machine\n'
  printf '  model on disk: %s   engine RAM: %s GB / %s GB   swap: %s\n' \
         "$MODEL_GB" "$RSS_GB" "$TOTAL_GB" "$SWAP"
  printf '  RAM [%s]\n' "$(bar ${RSS_KB:-0} $(( TOTAL_GB * 1048576 )) 40)"
  printf '  ────────────────────────────────────────────────────────────\n\n'
  sed -n '/--- risposta ---/,$p' "$OUT" | tail -n +2 | fold -s -w 66 | tail -18
  sleep 0.5
done

wait $PID 2>/dev/null
clear
printf '  jugnu demo — finished\n'
printf '  model on disk: %s   swap: %s\n' "$MODEL_GB" "$(sysctl -n vm.swapusage | awk '{print $6}')"
printf '  ────────────────────────────────────────────────────────────\n\n'
sed -n '/--- risposta ---/,$p' "$OUT" | tail -n +2 | fold -s -w 66
printf '\n  %s\n' "$(grep -oE 'prompt=[0-9]+ .*peak_rss=[0-9.]+ GB' "$ERR" | head -1)"
rm -f "$OUT" "$ERR"
