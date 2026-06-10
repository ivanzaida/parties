# Protocol Migration — June 2026 (→ protocol v1.0)

This release changes the **control-plane wire format** and tightens
authentication. **Client and server must be upgraded together** — a pre-change
client cannot talk to a post-change server, or vice versa (the version handshake
rejects the mismatch on both sides). There is no released version pinned in the
wild, so this is a hard cutover, not a staged rollout.

The data plane (voice datagrams, video stream frames, file-transfer streams) is
**unchanged**. Only the items below changed.

---

## TL;DR

| Change | Wire impact | Action required |
|---|---|---|
| Protocol version is now `major.minor` packed in the u16 | `AUTH_IDENTITY` version field value changed `1` → `0x0100` | Send `PROTOCOL_VERSION`; reject only on major mismatch |
| `SERVER_ERROR` gained a `u16` code prefix | `[message]` → `[code(u16)][message]` | Read the code before the message |
| `CHANNEL_KEY` (0x0109) removed | Message no longer sent after `CHANNEL_JOIN` | Stop waiting for it |
| `AUTH_IDENTITY` freshness + replay rules tightened | No layout change | Use a real current unix timestamp (you already do) |
| 0-RTT disabled (`RESUME_ONLY`) | No app-visible change | None |

---

## 1. Protocol version is now `major.minor` (R1)

**Before**
```
constexpr uint16_t PROTOCOL_VERSION = 1;          // AUTH_IDENTITY sent u16 = 1
// server: reject if client_version != PROTOCOL_VERSION   (exact match)
```

**After**
```
PROTOCOL_VERSION_MAJOR = 1
PROTOCOL_VERSION_MINOR = 0
PROTOCOL_VERSION       = (major << 8) | minor = 0x0100   // AUTH_IDENTITY sends u16 = 256
// server: reject if protocol_major(client_version) != PROTOCOL_VERSION_MAJOR
```

The `AUTH_IDENTITY` payload layout is unchanged — still a leading `u16` version —
but **the value and its interpretation changed**. The high byte is the major
version, the low byte is the minor.

- The server now rejects **only on a major mismatch** (`ServerErrorCode::BadVersion`).
  Minor bumps are backwards-compatible: an older client with the same major still
  authenticates.
- **Cross-version behavior:** an old client sends `1` → server sees major `0` ≠ `1`
  → rejected. A new client sends `0x0100` against an old server → old server's
  `!= 1` check fails → rejected. Hence the hard cutover.

**Client/alt-implementation action:** send `protocol::PROTOCOL_VERSION`. If you
parse it, use `protocol_major()` / `protocol_minor()`. Going forward, add
backwards-compatible message types/fields under a **minor** bump; reserve
**major** bumps for breaking changes.

## 2. `SERVER_ERROR` (0x01FF) carries an error code (R4)

**Before**
```
SERVER_ERROR payload: [message: string]
```

**After**
```
SERVER_ERROR payload: [code: u16][message: string]
```
where `string` is the usual `[u16 length][bytes]`, and `code` is
`protocol::ServerErrorCode`:

| Code | Value | Meaning |
|---|---|---|
| `Generic` | 0 | unspecified |
| `BadVersion` | 1 | incompatible protocol major — client must update |
| `BadAuth` | 2 | malformed message / bad signature / stale or replayed timestamp |
| `BadPassword` | 3 | wrong server password — re-prompt |
| `Kicked` | 4 | removed by an admin |
| `Replaced` | 5 | a newer login for the same identity took over |
| `RateLimited` | 6 | reserved (not yet emitted) |
| `NotFound` | 7 | reserved |
| `TooLarge` | 8 | reserved |
| `PermissionDenied` | 9 | action denied |
| `Internal` | 10 | server-side error |

**Client action:** read the `u16` code first, then the message string. A parser
that still reads the old format will interpret the code's two bytes as the
string length and corrupt the rest of the message.

**Behavioral note:** the client's auto-reconnect now keys off the **code**
(`Kicked` / `Replaced` → do not auto-reconnect) instead of substring-matching the
message text. If you maintain an alternate client with auto-reconnect, switch to
the code.

## 3. `CHANNEL_KEY` (0x0109) removed (S1)

The server used to mint a 32-byte per-channel key and send it after
`CHANNEL_JOIN`:
```
CHANNEL_KEY payload: [channel_id: u32][key: 32 bytes]
```
It was **dead crypto** — voice/video were always sent as raw Opus/frames and the
key was never applied. It has been removed entirely:

- The server no longer sends `CHANNEL_KEY` on join.
- Type id `0x0109` is **retired** — do not reuse it without a protocol bump.
- Media confidentiality is **QUIC TLS hop-by-hop with a trusted SFU**. There is
  no end-to-end media encryption. (If E2E is ever required, it must move key
  agreement off the server; see `docs/analysis-backlog.md` S1.)

**Client action:** remove any logic that waits for / stores `CHANNEL_KEY` after a
channel join. A client that blocks waiting for it will stall.

## 4. Authentication hardening (S3 / S9)

`AUTH_IDENTITY` **layout and signature are unchanged**:
```
[version u16][pubkey 32][display_name string][timestamp u64][signature 64][password string]
signature = Ed25519 over  pubkey(32) ‖ display_name ‖ timestamp(8)
```
New server-side enforcement:

- **Freshness window tightened** from ±60 s to **±30 s**. Keep client and server
  clocks reasonably synced (NTP). A skew > 30 s now yields `BadAuth`.
- **Replay guard:** the server rejects an `AUTH_IDENTITY` whose `timestamp` is not
  **strictly newer** than the last one it accepted for that public key (within
  the window). Use a real current unix timestamp — normal clients already do, and
  re-auth/reconnect naturally advances it. Two logins for the same identity
  within the same one-second tick will see the second rejected (rare; the
  duplicate-session logic would kick one anyway).
- **Server password** is now compared in constant time. No wire change; the
  password is still appended as a trailing `string` and protected only by QUIC
  TLS.

**Client action:** none beyond using a real current timestamp (already the case).

## 5. 0-RTT disabled, session resumption kept (S5)

The server's `ServerResumptionLevel` changed `RESUME_AND_ZERORTT` → `RESUME_ONLY`.
TLS 1.3 0-RTT early data is replayable, and our only early message
(`AUTH_IDENTITY`) must not be. Resumption tickets are still issued and still give
a fast **1-RTT** reconnect. No application-visible change; a client does not send
0-RTT application data.

---

## In-tree C++ API changes (for code in this repo)

These are source-level, not wire-level. Most existing call sites compile
unchanged.

- `parties::random_bytes(uint8_t*, size_t)` now returns **`bool`** (was `void`) —
  `false` on CSPRNG failure (buffer is zeroed). Existing statement-style calls
  still compile; check the result where it matters (tokens, keys).
- New `parties::constant_time_equals(const std::string&, const std::string&)`.
- `protocol::ServerErrorCode`, `protocol::PROTOCOL_VERSION_MAJOR/MINOR`,
  `protocol::protocol_major()/protocol_minor()` added.
- `Server::send_error(session_id, message, code = ServerErrorCode::Generic)` —
  gained the trailing defaulted `code` parameter.
- Removed: `Server::send_channel_key`, `Server::channel_keys_`,
  `AppCore::channel_key_`, `AppCore::on_channel_key`. The `ChannelKey` alias in
  `common/include/parties/types.h` is now unused.
- `QuicServer::on_disconnect` callback was **removed**. Disconnects are now
  delivered via the `QuicServer::disconnects()` `ThreadQueue<SessionDisconnect>`,
  drained on the server main loop (`Server::process_disconnects`). If you hooked
  `on_disconnect`, drain the queue instead.

## Verification

`cmake --build --preset default` clean; the 15-case integration test passes.
The test covers auth, channel join, voice, video, screen-share, and PLI — it does
**not** cover text chat or file transfer, so exercise those manually after
upgrading.
