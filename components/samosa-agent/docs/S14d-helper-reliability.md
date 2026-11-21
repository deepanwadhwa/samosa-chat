# S14d Linux helper launch reliability

C11/C13: an Agent process must launch its owned guardian from its own running
executable, even when Linux reports the executable's resolved pathname deleted.
Agent owns helper lifetime/control; no provider business logic or shared state
moves. Include src/command.c, standalone native transport regression and build
entry points. Keep current deadlines/process groups/scoped cancellation unchanged.

Reproduction: /private/tmp/samosa-s14d-linux-native.log shows ENOENT/code2 launching
`/qa/samosa-agent/build/source-control-sanitize (deleted)` during the first public
register_pdf call, after0.001s, without an ASan report. Resolved `/proc/self/exe`
pathnames are advisory: once detached, the kernel adds this suffix. Passing the
suffix to exec is incorrect. Use the kernel executable reference itself on Linux.
Do not strip the suffix and potentially launch a different replacement binary.
macOS retains its existing platform path mechanism; this does not claim the same
inode pinning behavior there.

Acceptance: native child transport responds normally; on Linux copy the running
regression executable into a private temporary directory, execute it, unlink its
pathname and perform a helper-mediated `/bin/cat` round trip. The old code must
fail this deterministic scenario; the fixed code and ASan/UBSan must pass. Re-run
Linux native Source control/full instrumentation and Agent control regressions,
then final independently built releases and content comparisons. Focused commands:
`make test-transport sanitize-transport`. A fixed reproduction does not waive the
remaining integration gates or all assembled reliability/load qualification.
This corrects replacement transport; no legacy retirement is justified yet.

The delayed synthetic provider must retain its pre-registration race while
returning a committed terminal state after accepting cancellation. It may poll
its own SQLite row for at most 250ms before replying; it must never synthesize
completion. This avoids an unnecessary third instrumented guardian launch within
the unchanged three-second production observation budget. Strict confirmed
cancellation assertions remain required for this fixture.
