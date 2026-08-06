#!/bin/sh
set -eu
# tests/test_backend_limits.c mirrors backend_limits() from the gateway so the
# tier table can be checked for machines this repo cannot run on. A mirror that
# drifts is worse than no test, so the tier constants are compared directly.
GW=${SAMOSA_GATEWAY_SRC:-src/samosa_gateway.c}
T=${SAMOSA_LIMITS_TEST:-tests/test_backend_limits.c}
for f in "$GW" "$T"; do [ -f "$f" ] || { echo "test_backend_limits_match.sh: $f not found" >&2; exit 1; }; done
python3 - "$GW" "$T" <<'PY'
import re, sys

def strip_comments(src):
    # Comments carry numbers too ("beyond 12 threads..."), which are prose, not
    # policy. Compare code only.
    src = re.sub(r'/\*.*?\*/', ' ', src, flags=re.S)
    return re.sub(r'//[^\n]*', ' ', src)

def tiers(path):
    s = strip_comments(open(path).read())
    # The ladder assigning ctx from gb, plus the thread cap, in source order.
    body = re.search(r'if \(gb <= 0\)(.*?)\*out_ctx', s, re.S)
    if not body:
        raise SystemExit(f"could not find the tier ladder in {path}")
    nums = re.findall(r'(\d+(?:\.\d+)?)', body.group(1))
    cap = re.search(r'threads > (\d+)\) threads = (\d+)', s)
    return nums, (cap.group(1), cap.group(2)) if cap else None

gw, test = sys.argv[1], sys.argv[2]
a, acap = tiers(gw)
b, bcap = tiers(test)
if a != b or acap != bcap:
    print("test_backend_limits_match.sh: the mirrored tier table has drifted")
    print(f"  {gw}:   tiers={a} cap={acap}")
    print(f"  {test}: tiers={b} cap={bcap}")
    raise SystemExit(1)
print("test_backend_limits_match.sh: PASS")
PY
