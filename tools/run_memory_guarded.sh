#!/bin/bash

# Generic process-tree guard. The historical variable name is retained for
# compatibility; the sampled value is resident set size (RSS), in MiB.
MAX_FOOTPRINT_MB=${MAX_FOOTPRINT_MB:-6144} # 6 GiB process-tree RSS hard kill
MAX_SWAP_DELTA_MB=${MAX_SWAP_DELTA_MB:-256}

get_swap_mb() {
    sysctl -n vm.swapusage | awk '{print $6}' | sed 's/M//' | awk '{print int($1)}'
}

get_tree_rss_mb() {
    local pid=$1
    local total_rss=0
    get_rss_recursive() {
        local p=$1
        local rss=$(ps -o rss= -p "$p" 2>/dev/null | tr -d ' ')
        if [ -n "$rss" ]; then
            total_rss=$((total_rss + rss))
        fi
        local children=$(pgrep -P "$p" 2>/dev/null)
        for child in $children; do
            get_rss_recursive "$child"
        done
    }
    get_rss_recursive "$pid"
    echo "$((total_rss / 1024))"
}

get_memory_pressure() {
    sysctl -n kern.memorystatus_vm_pressure_level 2>/dev/null || echo "1"
}

BASELINE_SWAP=$(get_swap_mb)
PEAK_RSS_MB=0
echo "[Memory Guard] Baseline swap: ${BASELINE_SWAP} MB"
echo "[Memory Guard] Limits: ${MAX_FOOTPRINT_MB} MiB process-tree RSS, ${MAX_SWAP_DELTA_MB} MiB swap delta"

# Start requested process
"$@" &
CHILD_PID=$!

# When this guard is backgrounded by a lifecycle test, signals initially land
# on the guard rather than the guarded server.  Always forward shutdown so a
# test cannot leave an unguarded model process behind.
forward_shutdown() {
    trap - INT TERM HUP
    kill -TERM "$CHILD_PID" 2>/dev/null || true
    wait "$CHILD_PID" 2>/dev/null || true
    exit 143
}
trap forward_shutdown INT TERM HUP

# Polling loop
while kill -0 "$CHILD_PID" 2>/dev/null; do
    sleep 1

    CURRENT_SWAP=$(get_swap_mb)
    SWAP_DELTA=$((CURRENT_SWAP - BASELINE_SWAP))
    if [ "$SWAP_DELTA" -lt 0 ]; then SWAP_DELTA=0; fi

    PRESSURE=$(get_memory_pressure)
    if [ "$PRESSURE" -ge 2 ]; then
        echo "[Memory Guard] ABORT: Severe system memory pressure detected (level $PRESSURE)" >&2
        kill -9 "$CHILD_PID" 2>/dev/null
        exit 1
    fi

    RSS_MB=$(get_tree_rss_mb "$CHILD_PID")
    if [ "$RSS_MB" -gt "$PEAK_RSS_MB" ]; then
        PEAK_RSS_MB=$RSS_MB
    fi

    if [ "$RSS_MB" -ge "$MAX_FOOTPRINT_MB" ]; then
        echo "[Memory Guard] ABORT: Process tree exceeded RSS limit (${RSS_MB} MiB >= ${MAX_FOOTPRINT_MB} MiB)" >&2
        kill -9 "$CHILD_PID" 2>/dev/null
        exit 1
    fi

    if [ "$SWAP_DELTA" -gt "$MAX_SWAP_DELTA_MB" ]; then
        echo "[Memory Guard] ABORT: Process exceeded swap delta limit (${SWAP_DELTA} MB > ${MAX_SWAP_DELTA_MB} MB)" >&2
        kill -9 "$CHILD_PID"
        exit 1
    fi
done

wait "$CHILD_PID"
EXIT_CODE=$?

# Report final stats
FINAL_SWAP=$(get_swap_mb)
FINAL_SWAP_DELTA=$((FINAL_SWAP - BASELINE_SWAP))
if [ "$FINAL_SWAP_DELTA" -lt 0 ]; then FINAL_SWAP_DELTA=0; fi

echo "[Memory Guard] Process exited with code ${EXIT_CODE}"
echo "[Memory Guard] Peak process-tree RSS: ${PEAK_RSS_MB} MB"
echo "[Memory Guard] Final swap delta: ${FINAL_SWAP_DELTA} MB"
exit $EXIT_CODE
