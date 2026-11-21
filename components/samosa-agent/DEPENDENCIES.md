# Dependencies

C11 compiler and make; libc/POSIX runtime. Native C and shell acceptance tests.
Configured external compatible Sources/Inference executables for interaction.
No private sibling includes, caches or state. The JSON parser is a local mechanical
transport copy, not shared business policy. No Python anywhere in this component.
Current build qualification: macOS arm64. Linux requires actual qualification.

S05 durable Agent state uses system SQLite >=3.24, linked directly with -lsqlite3.
No interpreter/ORM/shared schema package. Conversations/runs/events/semantic
snapshots stay in Agent's private database; clients consume JSON contracts only.

Optional actual hot/cold/compact qualification uses system jq/curl, an external
native engine launcher, compatible llama-server/model and an unused test port.
It launches/restarts/stops only its own engine. No Python or package manager.

S08a native self-exec path discovery uses macOS dyld public executable-path API
or Linux /proc/self/exe. Guardian uses POSIX pipes/poll/posix_spawn/process groups;
no shell/Python/helper-install dependency. Linux still needs executed qualification.

S11b1 undo uses native macOS renameatx_np(RENAME_EXCL) or Linux
renameat2(RENAME_NOREPLACE). Unsupported hosts return a typed refusal. Private
identity hard links require action state and destination on one filesystem;
cross-device publication refuses before changing the destination pathname.
Native ARM64 Linux qualification is recorded in the main verification file.

Optional S14b child-native QA driver links libcurl, with curl-config supplying
compiler/link flags. Model-free Agent builds do not require libcurl. The driver
consumes public external Inference CLI/status and authenticated engine slot
observation; it includes no Inference source. Its shell runner requires an owned
compatible managed provider/engine and a read-only model path.
