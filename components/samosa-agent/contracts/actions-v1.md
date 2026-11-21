# Checked copy, move and inverse plan contract v1

Host config grants action_root/action_host_id; requests never override them. State
and owner use the existing Agent host configuration. No Sources/Inference command
is needed for checked effects. Public operations:

- plan_create: operation_id,action copy or move,host_id,files1..8. Each entry has source,
  destination (relative paths),version (SHA-256 bytes),file_identity (mechanical
  SHA from qualified inventory). Reject unknown fields, escapes, symlink parents,
  nonregular/oversized sources, duplicate destinations and existing destinations.
- plan_get: plan_id. Public immutable plan revision1 plus approval/progress,
  historical per-file status/error and committed publication inode receipts.
- undo_plan_create: source_plan_id,plan_revision1,operation_id. Creates a separate
  immutable undo_copy or undo_move plan from committed, unreversed receipts. Its own exact
  approval is required. Verifies the private identity anchor and current target.
- plan_approve: plan_id,plan_revision1,operation_id. Approves that exact owner-bound
  immutable plan. Finding/model prose alone provides no authorization.
- plan_execute: plan_id,plan_revision1,operation_id. Same checked executor for every
  future trigger. Refuses unapproved plans before effects. Exact completed retry
  returns persisted result before root access; changed parameters conflict.

Capacity32 plans,128 durable operations,8 files/plan,1MiB/file,16MiB SQLite page
capacity on native4096-byte pages. No eviction. One active action worker,zero queued;
chat uses its own store/lease and remains independent. File contents stream through
64KiB buffers. Staging is bounded by256 item slots, at most256MiB retained payload.
Input<=16KiB,public response<=1MiB. Model weights are not an effect dependency.

Capture source through no-follow descriptors; check version/mechanical identity,
size and timestamps before/after reading and revalidate source pathname. Bind root
and destination-parent inode identities. Stage bytes privately on destination
filesystem. Persist operation/item intent before staging and staged inode identities
before atomic no-clobber hard-link publication. Never replace an existing target.
An owner-private hard-link identity anchor prevents inode reuse from authorizing
deletion of a replacement. Action state and destination must share a filesystem;
cross_device_anchor refuses publication otherwise. Identity anchors remain bounded
by the 256 item slots and retain history after undo. Older unanchored receipts
remain historical but cannot authorize undo.

SIGKILL recovery requires the exclusive kernel lease. Running operations become
interrupted without automatic replay. Prepared receipts inspect destination inode
and version; an exact owned publication becomes a committed copy. A foreign
replacement is failed destination_changed, even if content happens to match.
Absent destination stays prepared until a new explicit execute operation resumes
from verified staging. Unidentified interrupted intent needs_reconciliation; it
is never blindly adopted. Cleanup removes only staged inodes known to the receipt.

copied_items describes committed receipt history, not current filesystem presence.
unreconciled_items explicitly counts uncertain intent/prepared work. effect_status
completed requires every item committed; partial/pending outcomes preserve detail.
Get remains readable during work through a consistent transaction. Owners cannot
look up each other's plans; source/destination scopes are host-local, not global.

Undo atomically captures a target into an owner-private quarantine using native
no-replace rename, verifies inode/version/metadata, then records deletion intent
before unlinking the verified payload. A raced foreign capture is restored only
without overwriting another file. Interrupted capture is inspected without
automatic deletion; a new execute resumes verified work. A completed deletion
whose receipt was interrupted can be reconciled without touching a later target.
An unidentified quarantine keeps its uncertain phase with needs_reconciliation;
unrelated plans remain available and no unknown payload is deleted or adopted.
undone_items and removed inode receipts record inverse history; original copied
items retain their publication receipts and expose reversed=true.

Move binds the source inode and both parent directory identities. Only supported
same-filesystem moves are admitted; cross_device_move refuses before capture.
Persist move_prepared with known quarantine and original inode before native
no-replace capture, verify captured identity/content, persist move_captured, pin
the identity anchor, then publish by native no-replace rename. Source and target
retain the same file inode. A missing publication requires a new explicit execute
to resume known quarantine; get never publishes. A verified anchored publication
is reconciled into moved history without touching a later original-source name.
An inverse move restores the original name only if absent; a foreign file or a
changed parent refuses. Its restored inode receipt and original reversed marker
commit together. moved_items is history, not a live filesystem presence claim.

Schedules and Source receipt/freshness reconciliation are required later
assignments. This checked-copy subset is preparatory for C10/product retirement.
# Automatic Sources receipt observation

With a configured Sources command, successful plan_execute asks Sources to
observe receipt paths after physical effects commit. Its persisted result has
source_reconciliation: status completed/failed/pending/unconfigured, reason and
up to two public observation batches. The expected host/root scope and canonical
owner-bound locator IDs must match the immutable plan; paths, versions and
physical tuples must match committed receipts. Sources independently verifies
files under its own grant. It never gains registration authority from a receipt.

Effect_status and physical receipts retain their meanings when observation fails.
Observations describe checkpoints and committed history; they do not promise
continuous freshness. Exact completed operation retry returns the saved effect
and observation result without Root or Sources access. New execute operations
refresh observations without replaying completed effects. Plan_get describes
physical history; execution results own their separate observation checkpoints.
Worker death during handoff interrupts the operation while committed physical
receipts survive. Existing lease recovery and explicit new-operation rules apply.
Each Sources call is bounded to eight locations, 16KiB request, 64KiB validated
response and10-second command deadline; a move may require two calls for8 items.
