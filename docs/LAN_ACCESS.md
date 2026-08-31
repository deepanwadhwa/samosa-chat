# Samosa LAN access

LAN access lets a phone, tablet, or second computer use the Samosa app running
on a Mac on the same local network. The browser is remote; the models, model
weights, inference, durable model context, and helper processes remain on the
host Mac.

This is a shared-instance feature, not a multi-user account system. Every
authenticated device controls the same Samosa gateway and the same active
model.

## Architecture and trust boundary

```text
Phone / tablet / other laptop
        |
        | local HTTP, port 8642
        | password login + browser session
        v
Mac: authenticated Samosa gateway (0.0.0.0:8642)
        |
        | loopback only
        v
Mac: selected model backend (127.0.0.1:8643)
```

Only the authenticated gateway is exposed to the LAN. The raw model backend is
forced to `127.0.0.1`, including when the public gateway is in LAN mode. Samosa
does not copy a model or start a model process on the remote device.

For a remote turn:

1. The remote browser sends the prompt and any attachment over the LAN to the
   gateway on the Mac.
2. The gateway passes admitted inference work to the currently selected local
   model.
3. The Mac performs tokenization, inference, attachment processing, and any
   enabled OCR or visual-model work.
4. Generated tokens stream back to the remote browser.

The LAN feature itself does not move inference to a cloud service. Optional web
search and model downloads retain their normal, separately disclosed network
behavior.

## Start LAN access

Run this on the Mac that has Samosa and the models installed:

```sh
samosa app --lan
```

Samosa opens the loopback address on the Mac and prints a clean address for
other devices:

```text
Samosa LAN access is on.
This Mac:     http://127.0.0.1:8642
Other devices: http://192.168.1.20:8642/
Password:      password1234
Stop sharing with: samosa serve --stop
```

The address will contain the Mac's current local IPv4 address. It may differ
from this example.

Use `samosa serve --lan` when the gateway should start without opening a browser
on the Mac. For attached diagnostics, use:

```sh
samosa serve --lan --foreground
```

Always use `--lan` for this feature. Setting `SAMOSA_BIND=0.0.0.0` by itself is
a low-level development configuration and does not enable the LAN password
policy.

## Connect another device

1. Connect the Mac and the other device to the same trusted local network.
2. Send the `Other devices` address through AirDrop, Messages, email, or another
   private channel, or type it into the other device's browser.
3. Open the address. The remote device sees a Samosa sign-in page rather than
   the app.
4. Enter the password printed by the launcher. The temporary default is
   `password1234`.
5. Bookmark the address or add it to the device's home screen if desired.

The address has no secret query string. Authentication is carried by a browser
session cookie after login. A gateway restart changes the internal session
token, so remote browsers must sign in again.

The Mac's own `http://127.0.0.1:8642` address remains directly accessible from
the Mac. Loopback requests do not show the LAN password screen.

### Use a different password

The current default is intentionally temporary and is weak. Before sharing
access with another person, stop the existing service and start it with a
stronger password:

```sh
samosa serve --stop
SAMOSA_LAN_PASSWORD='a-long-password-you-chose' samosa app --lan
```

The launcher prints the configured password in that terminal so it can be
transferred to the other device. Avoid leaving the password in screenshots or
sharing it in the same public channel as the address.

## What is shared between devices

LAN clients are remote controls for one gateway. They do not receive separate
accounts, permissions, model processes, or model-selection state.

The following state is global on the Mac:

- the active primary model;
- model downloads, installation state, and model switching;
- inference and context-capacity settings;
- the active generation and Stop/cancel controls;
- server-side model context, attachment records, folder-memory scopes, and
  other gateway-owned data;
- on-demand OCR, vision, summarization, and voice helper capacity.

Some presentation state is browser-local. In particular, each browser stores
its visible transcript/sidebar records and display preferences in its own
local storage. A phone therefore does not automatically mirror the exact
sidebar shown on the Mac, even though both browsers send work to the same host
and share its global model and server-side resources.

Anyone with the LAN password should be treated as having full use of the Samosa
app. There is currently no read-only role, per-user chat namespace, per-user
model choice, audit identity, or individual-session revocation.

## Concurrent requests

Many devices can keep the app open, browse their local interface state, or
prepare prompts at the same time. Model inference is deliberately serialized:
only one primary-model generation runs at a time. Additional requests wait
rather than causing another copy of the model to load.

If `T` is the average time for one turn and `N` equal-length turns arrive at
once, the approximate behavior is:

```text
last completion time = N × T
average queue wait   = (N - 1) × T / 2
throughput           = 1 / T turns per second
```

For example, with five simultaneous 30-second turns, the first finishes near
30 seconds and the fifth near 150 seconds. Average queue wait is about 60
seconds. Real turns vary because prompt ingestion and output lengths differ.

Queue details currently depend on the selected backend:

- Ornith and Bonsai are launched with one llama.cpp server slot. Extra
  requests are deferred internally until that slot becomes free.
- Native Qwen admits one active generation and, by default, up to four waiting
  requests. A further request receives `429 queue_full`.
- On-demand visual specialists are also globally admitted. On memory-limited
  Macs, Samosa may temporarily stop the primary model while the specialist is
  loaded, then restore it after the visual turn.

There is not yet a user-facing queue position, per-device quota, or fair-share
scheduler. A browser may simply appear to be waiting while another device's
turn is active. Do not rely on closing a waiting tab to cancel work that has
already reached the backend. Stop and model controls should be treated as
shared operational controls.

The devices also share one hot conversation slot in the active text engine.
Two devices using different conversations do not create two copies of the
model: when the queued second conversation starts, it replaces the first hot
K/V or prompt-cache state. Returning to the first conversation performs a cold
checkpoint restore or prompt rebuild. Full-document extraction remains cached,
so a cold switch does not normally rerun OCR. See
[CONVERSATION_CONTEXT.md](CONVERSATION_CONTEXT.md) for the backend matrix and
eviction policy.

## Selecting different models from different devices

There is one global active-model selection. Two devices cannot independently
hold different primary models.

- If Device A selects Ornith, Ornith becomes the active model for all devices.
- If Device B later selects Bonsai, Samosa unloads Ornith, starts Bonsai, and
  all devices then use Bonsai.
- If two model switches arrive close together, one switch is serialized first.
  The other normally receives `409 selection_busy`; if the first switch
  finishes during the short bounded wait, the second can proceed afterward.
  The last successfully completed switch is the global result.
- A switch requested while a response is active receives
  `409 generation_active`. The running response is not intentionally replaced
  by a second model.
- A prompt submitted while a new backend is loading receives a retryable
  `503 backend_loading` response.

Model switching can also expose a conversation/model mismatch. Conversations
are bound to the model that created their server-side context. The selecting
browser starts a fresh conversation; another browser still displaying an old
conversation may need to switch back to its model or start a new conversation.

Closely timed model changes are therefore best coordinated between users. The
current UI polls shared status, so a browser may briefly display stale model
state until its next refresh.

## Authentication behavior

When LAN mode is active, a non-loopback request follows this flow:

1. `GET /` or `GET /index.html` returns the password page until authenticated.
2. `POST /v1/lan/login` validates the password.
3. Success sets a session cookie named `samosa_lan` with `HttpOnly`,
   `SameSite=Strict`, and `Path=/` attributes.
4. The authenticated root page receives Samosa's per-launch UI token and uses
   it for protected API requests.
5. Requests with a foreign browser `Origin` are rejected even if they present
   a valid UI token.

The password page does not expose the UI token. Incorrect passwords receive
`401 invalid_lan_password`; unauthenticated LAN API calls receive
`401 lan_access_denied`.

There is no separate logout or per-device revocation endpoint yet. To revoke
all sessions, stop or restart the gateway. Closing a browser normally removes
its session cookie, depending on that browser's session-restoration behavior.

## Security limitations

LAN access is intended for a trusted home or office network. It is not an
Internet-facing authentication system.

- Traffic uses plain HTTP, not HTTPS. Prompts, responses, attachments, and the
  password are not encrypted on the local network.
- The default `password1234` is convenient for development but unsuitable for
  an untrusted or shared network.
- The shared password grants broad app access, including model controls,
  attachments, Jobs, and folders deliberately made available through Samosa.
- Binding to `0.0.0.0` listens on the Mac's IPv4 network interfaces. A VPN,
  Ethernet adapter, or other connected interface may also make the gateway
  reachable according to that network's routing and the macOS firewall.
- Do not configure router port forwarding, expose port 8642 to the Internet,
  or run LAN mode on public Wi-Fi.
- Guest Wi-Fi commonly isolates clients and may also be an inappropriate trust
  boundary even when the devices can reach one another.

The password gate, loopback-only raw backend, same-origin API checks, and
per-launch session token reduce accidental access. They do not replace TLS,
individual accounts, rate limiting, or an Internet-grade reverse proxy.

## Stop sharing or return to local-only mode

Stop the gateway and invalidate its sessions:

```sh
samosa serve --stop
```

To keep using Samosa only on the Mac, start it again without `--lan`:

```sh
samosa app
```

Starting `samosa app` or `samosa serve` without `--lan` detects a running LAN
gateway, restarts it on `127.0.0.1`, and removes LAN reachability.

## Diagnostics

On the Mac, inspect gateway health with:

```sh
curl -s http://127.0.0.1:8642/healthz | python3 -m json.tool
```

LAN mode reports these fields:

```json
{
  "listen_address": "0.0.0.0",
  "lan_access": true,
  "lan_auth": "password"
}
```

The active model backend should listen only on loopback, normally port 8643:

```sh
lsof -nP -iTCP:8643 -sTCP:LISTEN
```

Its address should be `127.0.0.1:8643`, not `*:8643` or the Mac's LAN address.

For a source checkout, the integration test exercises the real non-loopback
interface, password failure and success, the session cookie, origin rejection,
and private-backend isolation:

```sh
make test-lan-access
```

## Troubleshooting

### The other device cannot connect

- Confirm both devices are on the same non-guest Wi-Fi or wired LAN.
- Keep the Mac awake and connected.
- If macOS Firewall prompts about `samosa-gateway`, choose **Allow**.
- Temporarily disconnect a VPN or verify that it permits local-network access.
- Run `samosa app --lan` again and use the newly printed address. DHCP can
  change the Mac's local IP after reconnecting or rebooting.
- Confirm `/healthz` reports both `listen_address: "0.0.0.0"` and
  `lan_access: true` on the Mac.

### The password worked before but is rejected now

The gateway may have restarted or may have been launched with a different
`SAMOSA_LAN_PASSWORD`. Use the password supplied when the current LAN service
was started; if uncertain, stop it and start it again with the intended
password. A restart also invalidates previously issued browser sessions.

### A response appears stuck before generating

Another device, a background job, or an attachment helper may be using the
single inference slot. Wait for that work to finish. Samosa does not currently
show a shared queue position.

### Model switching fails

Finish or stop the active response first. If another switch is loading, wait
for it to reach Ready and refresh Settings before choosing again.

### The microphone is unavailable on a phone

Many browsers require HTTPS or loopback for microphone capture. The LAN address
uses plain HTTP, so typed chat, attachments, model use, and audio playback can
work while microphone recording remains disabled. HTTPS termination is not
part of the current LAN feature.
