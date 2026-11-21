#!/bin/sh
# Run a native check with PATH-based Python calls refused and recorded.
# Absolute interpreter paths still require source review; this is not a sandbox.
set -eu
[ "$#" -gt 0 ] || { printf 'usage: %s COMMAND [ARG ...]\n' "$0" >&2; exit 2; }
guard_dir=$(mktemp -d "${TMPDIR:-/tmp}/samosa-no-python.XXXXXX")
trap 'rm -rf "$guard_dir"' EXIT HUP INT TERM
SAMOSA_PYTHON_GUARD_LOG="$guard_dir/invocations"
export SAMOSA_PYTHON_GUARD_LOG
for interpreter in python python2 python3; do
  cat >"$guard_dir/$interpreter" <<'EOF'
#!/bin/sh
printf '%s\n' "$0" >>"$SAMOSA_PYTHON_GUARD_LOG"
printf 'Unexpected Python invocation during a native check: %s\n' "$0" >&2
exit 99
EOF
  chmod 700 "$guard_dir/$interpreter"
done
command_status=0
PATH="$guard_dir:$PATH" "$@" || command_status=$?
if [ -s "$SAMOSA_PYTHON_GUARD_LOG" ]; then
  cat "$SAMOSA_PYTHON_GUARD_LOG" >&2
  exit 1
fi
exit "$command_status"
