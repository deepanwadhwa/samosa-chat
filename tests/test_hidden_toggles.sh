#!/bin/sh
set -eu
# A CSS author `display` rule beats the UA `[hidden] { display: none }` rule at
# equal specificity. An element whose class sets a *visible* display therefore
# ignores hidden=true unless the stylesheet also carries a `[hidden]` guard.
#
# This shipped twice. Once for .chutni-view, fixed at the time with a comment
# explaining it. Then again for .web-consent, where it made the internet
# consent card impossible to dismiss: clicking Allow saved the choice
# correctly and the card stayed on screen regardless, which read as a dead
# button and cost the owner a long debugging session.
#
# The signal used here is the markup, not the JavaScript. An element written
# with a `hidden` attribute is one the page intends to show and hide, however
# that hiding is later expressed in code -- an earlier version of this test
# looked for `els.X.hidden =` and missed the very bug it was written for,
# because the assignment went through a local variable.
APP=${SAMOSA_APP_HTML:-assets/app.html}
[ -f "$APP" ] || { echo "test_hidden_toggles.sh: $APP not found" >&2; exit 1; }
python3 - "$APP" <<'PY'
import re, sys
s = open(sys.argv[1]).read()

# Classes whose own rule leaves the element visible. A class whose rule is
# `display: none` (revealed later by a modifier class) is fine -- that is how
# the chooser dialogs work.
visible = set()
for m in re.finditer(r'\.([a-z0-9-]+)\s*\{([^}]*)\}', s):
    decls = re.findall(r'(?:^|;)\s*display\s*:\s*([a-z-]+)', m.group(2))
    if decls and decls[-1] != 'none':
        visible.add(m.group(1))

bad = []
for tag in re.finditer(r'<[a-z]+[^>]*\bhidden\b[^>]*>', s):
    text = tag.group(0)
    cls = re.search(r'\bclass="([^"]+)"', text)
    ident = re.search(r'\bid="([^"]+)"', text)
    if not cls:
        continue
    for c in cls.group(1).split():
        if c in visible and f'.{c}[hidden]' not in s:
            who = '#' + ident.group(1) if ident else '.' + c
            bad.append(f"  {who} (.{c}) sets a visible display but has no .{c}[hidden] guard")

if bad:
    print("test_hidden_toggles.sh: elements that cannot actually be hidden:")
    print("\n".join(sorted(set(bad))))
    raise SystemExit(1)
print("test_hidden_toggles.sh: PASS")
PY
