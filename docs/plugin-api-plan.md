# Parties Server Plugin API Plan

> Draft design proposal -- June 2026

## 1. Goal

Add a server-side plugin API for chat automation and bot users.

The v1 target is to let trusted server plugins:

- Intercept, inspect, reject, or transform chat messages
- Define slash-style chat commands for clients to discover
- Create server-owned bot users
- Send chat messages as bot users
- Join bot users to voice channels
- Send Opus audio packets as bot users, enabling music bots and similar use cases

This is intentionally server-only. Client plugins are out of scope for v1.

## 2. Current Server Fit

The current server already has the pieces needed for the chat half of this plan:

- Chat messages are handled centrally in `Server::handle_message` under
  `CHAT_SEND`.
- Text is parsed before persistence, so plugins can intercept before
  `Database::insert_message`.
- Chat broadcast already uses the normal `CHAT_MESSAGE` payload shape, so bot
  chat can reuse the same path once it has a valid sender user ID.
- `AUTH_RESPONSE` currently carries the authenticated server info, and the server
  already sends related lists immediately after auth. Command metadata should be
  sent in that same post-auth burst, preferably as a separate command-list
  message rather than by growing `AUTH_RESPONSE`.

The bot voice half needs new server state, not just a wrapper around existing
session APIs:

- Voice forwarding currently starts from a real QUIC session ID.
- Channel user lists and channel counts are currently built from live
  authenticated `Session` objects.
- Join and leave broadcasts are currently emitted from `CHANNEL_JOIN`,
  `CHANNEL_LEAVE`, and disconnect handling for real sessions.
- Chat messages have `sender_id INTEGER NOT NULL REFERENCES users(id)`, so bot
  chat needs a persisted synthetic user row or a schema change.

Therefore v1 should add a first-class server-owned bot participant model and
shared helper methods for chat broadcast, channel presence, and voice fan-out.

## 3. Design Principles

- Keep the voice/video transport core owned by the server.
- Expose stable handles and snapshots, not internal `Server`, `Session`, or
  `Database` objects.
- Use a C ABI so plugins can be built independently from the server binary.
- Treat native plugins as trusted code, not sandboxed code.
- Make permissions explicit in both the plugin manifest and server config.
- Dispatch plugin callbacks from the server main loop, not directly from MsQuic
  worker callbacks.
- Keep v1 focused on chat and bot audio. Add wider protocol hooks later only
  when a real plugin needs them.

## 4. Non-Goals for v1

- Client-side plugins
- Replacing QUIC, TLS, authentication, codecs, or database ownership
- Direct database mutation from plugins
- Direct access to internal project objects
- Arbitrary interception of all control messages
- Arbitrary interception of video packets
- Hot reload or unloading while sessions are active
- Running untrusted plugins safely in-process

Native plugins can crash or compromise the process. Permission checks document
operator intent and prevent accidental misuse, but they are not a security
sandbox.

## 5. Packaging

Plugins are native dynamic libraries loaded from a configured plugin directory.

| Platform | Extension |
|----------|-----------|
| Windows | `.dll` |
| Linux | `.so` |
| macOS | `.dylib` |

Each plugin ships with a manifest next to the library.

Example:

```toml
id = "parties.example.musicbot"
name = "Music Bot"
version = "0.1.0"
api_version = "1.0"
side = ["server"]
library = "musicbot.dll"

permissions = [
  "read_sessions",
  "read_chat",
  "moderate_chat",
  "create_chat_commands",
  "create_bot_users",
  "send_bot_chat",
  "join_bot_voice",
  "send_bot_audio"
]
```

The manifest should be parsed before loading the library so unsupported API
versions, disabled plugins, or ungranted permissions can be rejected before any
plugin code executes.

## 6. ABI Shape

The stable boundary should be C, even if most plugins are written in C++.

Required exports:

```cpp
extern "C" bool parties_plugin_init(const PartiesPluginHost* host,
                                    PartiesPluginRegistration* registration);

extern "C" void parties_plugin_shutdown();
```

All ABI structs should start with explicit size and API version fields:

```cpp
struct PartiesAbiHeader {
    uint32_t size;
    uint16_t api_major;
    uint16_t api_minor;
};
```

This allows backwards-compatible fields to be appended later.

## 7. Handles and Snapshots

Plugins receive opaque handles, not internal objects.

```cpp
typedef uint32_t PartiesSessionId;
typedef uint32_t PartiesUserId;
typedef uint32_t PartiesChannelId;
typedef uint64_t PartiesMessageId;

struct PartiesBot;
typedef PartiesBot* PartiesBotHandle;
```

`PartiesBotHandle` is opaque and server-owned. Plugins pass it back to host
functions but must not dereference it or persist it across server runs.

Session snapshots are read-only:

```cpp
struct PartiesSessionInfo {
    PartiesAbiHeader abi;
    PartiesSessionId session_id;
    PartiesUserId user_id;
    PartiesChannelId voice_channel_id;
    uint8_t role;
    bool authenticated;
    bool muted;
    bool deafened;
    const char* username;
};
```

Bot snapshots are also read-only:

```cpp
struct PartiesBotInfo {
    PartiesAbiHeader abi;
    PartiesBotHandle bot;
    PartiesUserId user_id;
    PartiesChannelId voice_channel_id;
    const char* display_name;
};
```

Any pointer returned in a snapshot is valid only for the duration documented by
the host function. Plugins must copy data they want to keep.

## 8. Chat Interception

Chat is the primary v1 interception point.

The server should call plugins before committing a user chat message to storage
or broadcasting it to clients.

```cpp
struct PartiesChatMessage {
    PartiesAbiHeader abi;
    PartiesSessionId session_id;
    PartiesUserId author_user_id;
    PartiesChannelId text_channel_id;
    const char* author_name;
    const char* text;
    uint8_t attachment_count;
};

enum PartiesChatDecisionCode : uint8_t {
    PARTIES_CHAT_CONTINUE = 0,
    PARTIES_CHAT_REJECT = 1,
    PARTIES_CHAT_REPLACE_TEXT = 2
};

struct PartiesChatDecision {
    PartiesAbiHeader abi;
    PartiesChatDecisionCode code;
    const char* replacement_text;
    const char* rejection_reason;
};
```

Callback:

```cpp
void (*on_chat_message)(const PartiesChatMessage* message,
                        PartiesChatDecision* decision);
```

Rules:

- `Continue` leaves the message unchanged.
- `Reject` prevents storage and broadcast, then returns an error to the sender.
- `ReplaceText` stores and broadcasts the replacement text.
- Attachments are visible as metadata in v1, but plugin-side file inspection can
  wait until a later API.
- Multiple plugins run in configured order. The current message text should be
  updated after each `ReplaceText` decision before the next plugin sees it.
- If any plugin rejects the message, dispatch stops.

Implementation note:

The current `CHAT_SEND` case validates the text channel and message length, then
immediately inserts the message. The plugin hook should run after core parsing
and validation, but before `Database::insert_message`.

## 9. Chat Commands

Plugins can define slash-style chat commands that the server advertises to
clients after authentication.

Plugin-side shape:

```cpp
struct PartiesCommandDefinition {
    PartiesAbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
};
```

Host API:

```cpp
bool (*create_chat_commands)(void* context,
                             const PartiesCommandDefinition* commands,
                             size_t command_count);
```

Rules:

- Requires the `create_chat_commands` permission.
- Command names are ASCII identifiers without the leading slash, for example
  `play`, `skip`, or `queue`.
- Command names are globally unique after loading all plugins.
- If two plugins define the same command, the later plugin should fail to
  register that command and the server should log a warning.
- `description` is client-displayable text.
- `usage` is a compact usage string, for example `/play <url>`.
- Commands are server-authoritative. The client uses the list for autocomplete
  and help, but the server still validates execution.

Protocol:

Add a server-to-client control message:

```cpp
CHAT_COMMAND_LIST = 0x0509
```

Payload:

```text
count u16
per command:
  name string
  description string
  usage string
```

The server sends `CHAT_COMMAND_LIST` immediately after `AUTH_RESPONSE`, alongside
`CHANNEL_LIST` and `CHAT_CHANNEL_LIST`. This is "server info" timing without
coupling command metadata to the auth payload.

Execution:

- Clients can submit command invocations through normal `CHAT_SEND` text.
- If the message starts with `/`, the server checks the first token against the
  registered command table before normal chat interception/persistence.
- Registered commands are dispatched to the owning plugin and are not stored as
  normal chat unless the plugin explicitly sends a bot chat response.
- Unknown slash commands should return `SERVER_ERROR` or a future ephemeral chat
  response instead of being stored as normal chat.

Callback:

```cpp
struct PartiesChatCommandInvocation {
    PartiesAbiHeader abi;
    PartiesSessionId session_id;
    PartiesUserId user_id;
    PartiesChannelId text_channel_id;
    const char* command_name;
    const char* args;
    const char* raw_text;
};

void (*on_chat_command)(const PartiesChatCommandInvocation* invocation);
```

Implementation note:

The command dispatch point should live inside the current `CHAT_SEND` path after
core parsing, channel validation, and message length checks, but before
`on_chat_message` and before `Database::insert_message`. That keeps command
invocations out of normal history unless a plugin deliberately emits bot chat.

## 10. Bot Users

Plugins need server-owned bot users so clients can see music bots and automation
as normal participants.

Bots are not authenticated client sessions. They are synthetic server-side users
owned by a plugin.

A plugin can create zero, one, or many bot users. Each successful creation
returns a distinct opaque `PartiesBotHandle`; the API must not assume a
plugin-wide singleton bot.

Bot users should be reused across plugin restarts instead of recreated.
`plugin_id + key` is the stable identity key. `key` is plugin-defined identity
and should not change; `display_name` is client-facing presentation text and can
be changed later.

Because stored chat messages currently require `sender_id` to reference
`users(id)`, v1 bot users are persisted as synthetic rows in the existing
`users` table with explicit bot metadata.

Recommended persistence model:

- Generate a deterministic synthetic public key from `plugin_id + key`.
- Store a fingerprint with a reserved prefix, for example `bot:<plugin_id>:<key>`.
- Store `is_bot = 1`, `bot_owner_plugin = plugin_id`, and `bot_key = key`.
- Use `Role::User` by default.

Host API:

```cpp
bool (*create_bot_user)(const char* key,
                        const char* display_name,
                        PartiesBotHandle* out_bot,
                        PartiesUserId* out_user_id);

bool (*destroy_bot_user)(PartiesBotHandle bot);

bool (*set_bot_display_name)(PartiesBotHandle bot,
                             const char* display_name);
```

Bot behavior:

- A bot gets a normal `UserId`.
- A bot appears in channel user lists after it joins a voice channel.
- A bot leaving or being destroyed emits the same user-left events as a normal
  user where appropriate.
- Bot users should be marked internally as server-owned so they cannot be
  moderated as if they were remote client sessions.
- Bot lifetime is owned by the creating plugin.
- Plugins may destroy individual bots. All bot users from a plugin are destroyed
  when that plugin shuts down.

Presence implementation note:

The current server derives channel membership from `Session::channel_id`. Bot
users need a parallel in-memory participant map, and every channel count,
`CHANNEL_USER_LIST`, `USER_JOINED_CHANNEL`, and `USER_LEFT_CHANNEL` builder must
include both live sessions and bot participants.

## 11. Sending Bot Chat

Plugins can send chat messages as bot users.

Host API:

```cpp
bool (*send_bot_chat)(PartiesBotHandle bot,
                      PartiesChannelId text_channel_id,
                      const char* text,
                      PartiesMessageId* out_message_id);
```

Rules:

- Bot chat follows the normal storage and broadcast path.
- Bot chat should be marked with the bot's `UserId` and display name.
- Multiple bots from the same plugin may send chat independently.
- Bot chat should not recursively call `on_chat_message` by default. If a plugin
  needs bot messages to be intercepted too, add an explicit flag in a later API.
- The host enforces text length and channel existence.

Implementation note:

The existing chat persistence and broadcast logic is embedded directly in the
`CHAT_SEND` switch case. V1 should extract a shared helper, for example
`Server::store_and_broadcast_chat_message(sender_id, sender_name, channel_id,
text, attachments)`, so user messages and bot messages cannot drift.

## 12. Sending Bot Audio

Plugins can send pre-encoded Opus audio as bot users.

Host API:

```cpp
bool (*join_bot_voice)(PartiesBotHandle bot,
                       PartiesChannelId voice_channel_id);

bool (*leave_bot_voice)(PartiesBotHandle bot);

bool (*send_bot_voice_packet)(PartiesBotHandle bot,
                              uint16_t sequence,
                              const uint8_t* opus_payload,
                              size_t opus_payload_len);
```

Packet expectations:

- Payload is Opus, not PCM.
- Sample rate is 48 kHz.
- Frame duration should match the existing voice path, currently 20 ms.
- The host builds the normal forwarded voice packet shape:

```text
VOICE_PACKET_TYPE u8
sender_user_id u32
sequence u16
opus_payload bytes
```

The plugin should not manually construct wire packets for bot voice. It provides
the Opus payload and sequence number; the host validates and forwards.

Rules:

- A bot must be in a voice channel before sending audio.
- Multiple bots from the same plugin may be present in different voice channels,
  or in the same voice channel if normal channel capacity allows it.
- Audio is forwarded only to authenticated users in the bot's channel who are
  not deafened.
- The host enforces the same max Opus packet size as normal client voice.
- The plugin is responsible for pacing audio packets, usually one packet every
  20 ms.
- If a plugin sends too fast, the host may drop packets or rate-limit that bot.

This is enough for a Discord-style music bot if the plugin handles media
decoding, resampling, Opus encoding, and timing internally or through its own
dependencies.

Implementation note:

The current voice path receives a datagram, strips the leading packet type in
`QuicServer`, then forwards `[VOICE_PACKET_TYPE][sender_user_id][client_payload]`
to other sessions in the sender's channel. The client payload already begins
with the 2-byte sequence number. `send_bot_voice_packet` should reuse the same
fan-out logic, but with the sender resolved from bot participant state instead
of `quic_.get_session(pkt.session_id)`.

## 13. Host API

The initial host function table should stay narrow:

```cpp
struct PartiesPluginHost {
    PartiesAbiHeader abi;
    void* context;

    void (*log)(void* context, uint8_t level, const char* message);
    uint64_t (*now_ms)(void* context);

    bool (*get_session_info)(void* context,
                             PartiesSessionId session,
                             PartiesSessionInfo* out_info);

    bool (*create_chat_commands)(void* context,
                                 const PartiesCommandDefinition* commands,
                                 size_t command_count);

    bool (*create_bot_user)(void* context,
                            const char* key,
                            const char* display_name,
                            PartiesBotHandle* out_bot,
                            PartiesUserId* out_user_id);

    bool (*destroy_bot_user)(void* context, PartiesBotHandle bot);

    bool (*set_bot_display_name)(void* context,
                                 PartiesBotHandle bot,
                                 const char* display_name);

    bool (*send_bot_chat)(void* context,
                          PartiesBotHandle bot,
                          PartiesChannelId text_channel_id,
                          const char* text,
                          PartiesMessageId* out_message_id);

    bool (*join_bot_voice)(void* context,
                           PartiesBotHandle bot,
                           PartiesChannelId voice_channel_id);

    bool (*leave_bot_voice)(void* context, PartiesBotHandle bot);

    bool (*send_bot_voice_packet)(void* context,
                                  PartiesBotHandle bot,
                                  uint16_t sequence,
                                  const uint8_t* opus_payload,
                                  size_t opus_payload_len);
};
```

Host functions must fail cleanly when the plugin lacks the required permission.

## 14. Permissions

Recommended v1 permissions:

| Permission | Meaning |
|------------|---------|
| `read_sessions` | Read authenticated session snapshots |
| `read_chat` | Observe chat messages |
| `moderate_chat` | Reject or replace user chat messages |
| `create_chat_commands` | Register slash-style chat commands |
| `create_bot_users` | Create and destroy server-owned bot users |
| `send_bot_chat` | Send text messages as bot users |
| `join_bot_voice` | Join and leave voice channels as bot users |
| `send_bot_audio` | Send Opus packets as bot users |

The plugin manifest declares requested permissions. Server config grants the
subset allowed by the operator. Loading is default-deny: a plugin id must appear
in `[[plugins.allow]]`, and effective permissions are the intersection of the
manifest request and the server grant.

## 15. Registration and Hooks

The v1 registration table can be small:

```cpp
struct PartiesPluginRegistration {
    PartiesAbiHeader abi;

    void (*on_server_started)();
    void (*on_server_stopping)();

    void (*on_session_authenticated)(PartiesSessionId session);
    void (*on_session_disconnected)(PartiesSessionId session,
                                    PartiesUserId user_id,
                                    PartiesChannelId voice_channel_id);

    void (*on_chat_message)(const PartiesChatMessage* message,
                            PartiesChatDecision* decision);

    void (*on_chat_command)(const PartiesChatCommandInvocation* invocation);
};
```

No generic control-message hook is required for v1. Chat and bot audio are the
product surface; generic protocol interception can be added later if needed.

## 16. Threading Model

Plugin callbacks should run on the server main loop thread.

Rules:

- Do not call plugins directly from MsQuic worker callbacks.
- Queue worker-thread events into the main loop before plugin dispatch.
- Host callbacks are synchronous in v1.
- Plugins must return quickly from callbacks.
- Long-running plugin work should happen on plugin-owned worker threads.
- Bot audio pacing can happen on a plugin-owned worker thread, but
  `send_bot_voice_packet` must be documented as thread-safe or marshaled into
  the server main loop by the host.

For music bots, the expected shape is:

1. Plugin creates a bot user.
2. Plugin joins the bot to a voice channel.
3. Plugin worker decodes/resamples/encodes media to 48 kHz Opus frames.
4. Plugin sends one Opus packet every 20 ms through `send_bot_voice_packet`.
5. Host forwards those packets using the normal voice fan-out path.

## 17. Configuration

Plugin loading should be disabled by default.

Server config example:

```toml
[plugins]
enabled = false
directory = "plugins"

[[plugins.allow]]
id = "parties.example.musicbot"
enabled = true
permissions = [
  "read_chat",
  "create_chat_commands",
  "create_bot_users",
  "send_bot_chat",
  "join_bot_voice",
  "send_bot_audio"
]
```

Operators should be able to disable all plugins globally regardless of per-plugin
settings.

## 18. Failure Handling

Recommended behavior:

- Failure to load one plugin logs an error and continues loading others.
- Failure from `parties_plugin_init` disables that plugin.
- Host calls with missing permissions fail and log at warning level.
- Invalid chat decisions are treated as `Continue` plus a warning.
- Duplicate or invalid command definitions are rejected and logged.
- If a plugin rejects a chat message without a reason, the host uses a generic
  rejection message.
- If a plugin crashes, the server process may crash. Native plugin isolation is
  not a v1 goal.

## 19. Implementation Plan

1. Add `common/include/parties/plugin_api.h` with ABI structs, handles, chat
   message structs, decisions, host function table, and registration table.
2. Add a server-side `PluginManager` that loads manifests, checks API versions,
   grants configured permissions, loads dynamic libraries, calls
   `parties_plugin_init`, and stores callbacks.
3. Add plugin config to `server/res/server.default.toml` and `server::Config`.
4. Add `CHAT_COMMAND_LIST` to the protocol and client parsing/storage for
   command metadata.
5. Add `create_chat_commands` host support, command validation, duplicate-name
   rejection, and post-auth command-list sending.
6. Dispatch registered slash commands from `CHAT_SEND` before normal chat
   persistence.
7. Add persisted synthetic bot user creation using the existing `users` table.
8. Add server-owned in-memory bot participant state, separate from QUIC
   sessions.
9. Extract shared helpers for channel user-list building, channel counts,
   join/leave broadcasts, chat persistence/broadcast, and voice fan-out.
10. Wire lifecycle hooks into `Server::start`, `Server::stop`, authentication,
   and disconnect cleanup.
11. Wire `on_chat_message` before chat persistence and broadcast.
12. Implement `send_bot_chat` through the normal chat persistence and broadcast
   path.
13. Implement `join_bot_voice` and `leave_bot_voice` using the normal channel
   presence broadcast path.
14. Implement `send_bot_voice_packet` using the normal server voice fan-out path.
15. Add one sample plugin under `examples/plugins/musicbot_stub` that registers
    `/play`, creates a bot, responds to the command, joins voice, and sends
    generated Opus test packets.
16. Add tests for plugin load, permission rejection, command registration,
    command dispatch, chat reject/replace, bot chat delivery, bot voice
    presence, and bot voice packet forwarding.

## 20. Open Questions

- Should v1 add a `users.is_bot` migration immediately, or use reserved
  synthetic fingerprints first?
- Should bot chat messages be stored exactly like normal user messages, or should
  `messages` also get an explicit bot/source flag?
- Should bot users appear in admin moderation UI?
- Should `send_bot_voice_packet` accept Opus only, or should a later helper API
  accept PCM and encode through the server?
- Should chat commands be a first-class helper API or just implemented by
  plugins using `on_chat_message`? The current v1 plan makes them first-class.
- Should command responses have an ephemeral server-to-user message type, or is
  bot chat enough for v1?
- Should command metadata be resent on demand later, or only once after auth?
- Should plugin callbacks run in configured order only, or should manifests allow
  explicit ordering dependencies?

## 21. Recommended v1 Scope

Ship server plugins with chat and bot audio support only.

The useful first milestone is:

- Load configured native server plugins
- Dispatch server lifecycle hooks
- Advertise plugin-defined chat commands to clients after auth
- Dispatch registered slash commands before normal chat persistence
- Intercept user chat before storage and broadcast
- Allow plugins to reject or replace chat text
- Allow plugins to create server-owned bot users
- Allow bot users to send chat messages
- Allow bot users to join voice channels
- Allow bot users to send Opus voice packets through the normal fan-out path
- Include a music-bot-style sample plugin and focused integration coverage

That scope is narrow enough to implement without bending the whole app around
plugins, but powerful enough for the Discord-style bot use case.
