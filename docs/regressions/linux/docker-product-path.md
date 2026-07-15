# Docker product path — proven working, but there is no product yet

Ran on: 2026-07-15
Question this answers: *"Is the model running in Docker actually connected to
Samosa? How can a user with a Linux box or a Windows machine running Docker use
Samosa?"*

**Short answer: every piece works. None of it is packaged. A user cannot do this
today.**

## What was proven

`samosa serve` — the real product entry point, not the raw engine — running
inside a Debian container, reached from a browser/curl on the **macOS host**
through a published port:

```
$ curl http://127.0.0.1:8642/healthz          # from the HOST, into the container
{"status":"ok","model":"qwen3.6-35b-a3b","rss_gb":2.52,"context_limit_tokens":24576,
 "uptime_seconds":5,"scheduler":{"active":false,"queued":0,"max_queue":4}, ...}

$ curl http://127.0.0.1:8642/                 # the real web app HTML
<!doctype html> ... Samosa Chat ...

$ curl http://127.0.0.1:8642/v1/chat/completions -d '{"messages":[...France?...]}'
ANSWER: The capital of France is **Paris**.
usage: {'prompt_tokens': 19, 'completion_tokens': 9, 'total_tokens': 28}
samosa: {'thinking_closure': 'natural', 'tokens_per_second': 0.96, 'rss_gb': 3.84}
```

`rss_gb: 2.52` fresh matches the macOS 2.51 GiB figure; 3.84 after the turn sits
in the macOS 3.91–4.2 GiB plateau band. **G1's `/proc/self/statm` telemetry is
producing correct, comparable numbers.**

**D-2 (configurable bind) works and is correctly defaulted.**
[samosa_http.h:212-222](../../../src/samosa_http.h#L212-L222) reads `SAMOSA_HOST`
and falls back to `INADDR_LOOPBACK`. The container sets `SAMOSA_HOST=0.0.0.0`;
the host publishes `-p 127.0.0.1:8642:8642`. Native behavior is unchanged.

**G6 (launcher) works** — `dist/samosa:41-44` picks `xdg-open` on non-Darwin.

So the architecture is sound: **a Windows or Linux user running Docker can reach
Samosa in their browser.** That is the end goal, and it is reachable.

## What is missing — this is the actual gap

Everything above was **hand-assembled**. A real user has none of it:

```
- built qwen36b from source inside the container
- created /release/{bin/qwen36b, bin/samosa, app.html, samosa-chat.png, tokenizer_qwen36.json}
- symlinked /release/model -> a 24 GB model bind-mounted from the Mac
- set SAMOSA_RELEASE_DIR / SAMOSA_HOME / SAMOSA_HOST / SAMOSA_PORT by hand
```

| Missing | Status |
|---|---|
| **A Dockerfile** | **Does not exist on any branch.** Verified by `find` + `git log --diff-filter=A`. |
| **The model** | Bind-mounted from the developer's Mac. A user has no model and no way to get one into a container. |
| **A `pull` path** | `install.sh` downloads + SHA-256-verifies 24 GB, but is built for a *host* install (compiles the engine, writes `~/.samosa`, edits PATH). A container needs the download half only, targeting a volume. |
| **A published image** | No registry, no tag. |
| **Docs** | Nothing tells a Windows user what to run. |

This is exactly **D-3** in [TASKS_WINDOWS.md](../../TASKS_WINDOWS.md) (~2 days),
and it is not started.

## Defect: the UI claims "Your Mac" while served from Linux

[assets/app.html:361](../../../assets/app.html#L361) renders **"Your model. Your
Mac."** — served verbatim to a Linux/Windows Docker user. A false claim in the
product, and squarely against the project's accuracy bar.

Note the coupling: **[install.sh:157](../../../dist/install.sh#L157) smoke-tests
by grepping for that exact string.** Change the copy and the installer's smoke
test breaks. Fix both together.

## Model placement decides whether this is usable — confirmed

The bind mount from macOS APFS goes through virtiofs and it is **slow**:
`tokens_per_second: 0.96` against ~5–7 native. This confirms **D3** in
[TASKS_WINDOWS.md](../../TASKS_WINDOWS.md): the model must live in a **named
Docker volume** (inside the VM's ext4), never a host bind mount. A user who
bind-mounts from `C:\Users\...` will get a fraction of the throughput and blame
the model.

**None of the throughput numbers here are valid for the product** — they measure
virtiofs.

## The user flow D-3 must deliver

```sh
docker volume create samosa-model
docker run --rm -v samosa-model:/model ghcr.io/<org>/samosa pull      # 24 GB, verified
docker run -d --name samosa \
  -p 127.0.0.1:8642:8642 \
  -v samosa-model:/model \
  --memory=8g \
  ghcr.io/<org>/samosa serve
# open http://127.0.0.1:8642
```

Notes for whoever builds it:

- **Entrypoint is `serve`, not `app`.** `samosa app` calls `xdg-open`, which is
  meaningless in a container — the user opens the browser on the host.
- **Publish as `-p 127.0.0.1:8642:8642`, never `-p 8642:8642`.** The latter binds
  the host's `0.0.0.0` and exposes a 35B model server to the LAN.
- **Docker Desktop's VM must be ≥ 6 GB.** It defaults to ~2 GB, which cannot load
  the model at all. This blocked every attempt until it was raised.
- Compile the engine from source at image build time to preserve the property
  that users run code they can read.
