# Parties Plugins

This document describes the server-side plugin API. Client plugins are out of
scope for v1.

The current implementation supports native server plugins that can:

- Load from a configured plugin directory.
- Register lifecycle and session callbacks.
- Inspect, reject, or replace chat messages.
- Define chat commands that are advertised to clients.
- Receive chat command invocations from normal chat input.

The v1 design target also includes server-owned bot users, bot chat, and bot
audio packets for music-bot style integrations. Those models are described here,
but the current host API only exposes chat commands, logging, and time.

## Loading Model

Plugins are native dynamic libraries. Each plugin is discovered through a
`plugin.toml` manifest.

```toml
id = "parties.example.echo"
name = "Echo"
version = "0.1.0"
api_version = "1.0"
library = "echo.dll"

permissions = [
  "read_chat",
  "moderate_chat",
  "create_chat_commands"
]
```

Platform library extensions:

| Platform | Extension |
| --- | --- |
| Windows | `.dll` |
| Linux | `.so` |
| macOS | `.dylib` |

Server config controls global plugin loading:

```toml
[plugins]
enabled = false
directory = "plugins"

[[plugins.allow]]
id = "parties.example.echo"
enabled = true
permissions = [
  "read_chat",
  "create_chat_commands"
]
```

When enabled, the server recursively scans `directory` for files named
`plugin.toml`. A plugin is skipped if its manifest is invalid, its `api_version`
is unsupported, the library cannot be loaded, or it does not export
`parties_plugin_init`.

Plugin loading is default-deny. A manifest only declares requested permissions;
the server grants permissions through `[[plugins.allow]]`. The effective
permission set is the intersection of manifest-requested permissions and
server-granted permissions.

## ABI Rules

The public API lives in `sdk/include/parties/plugin_api.h`.

All ABI structs begin with:

```cpp
struct AbiHeader {
    uint32_t size;
    uint16_t api_major;
    uint16_t api_minor;
};
```

The header lets the server and plugin validate structure size and API version as
fields are appended in later versions. Plugins should initialize ABI structs
with `make_abi_header<T>()`.

Required export:

```cpp
extern "C" bool parties_plugin_init(const parties::plugin::Host* host,
                                    parties::plugin::Registration* registration);
```

Optional export:

```cpp
extern "C" void parties_plugin_shutdown();
```

The plugin should copy the `Host` table if it needs host calls after init. The
server owns the host context pointer.

## Handles

Plugins receive stable scalar handles rather than internal server objects:

```cpp
using SessionId = uint32_t;
using UserId = uint32_t;
using ChannelId = uint32_t;
using MessageId = uint64_t;

struct Bot;
using BotHandle = Bot*;
```

These are identifiers only. Plugins must not assume they map to pointers,
storage offsets, or authenticated state without querying future host APIs.
`BotHandle` is different: it is an opaque server-owned handle. Plugins may pass
it back to host functions, but must not dereference it or persist it across
server runs.

## Host Model

The current host table is:

```cpp
struct Host {
    AbiHeader abi;
    void* context;

    void (*log)(void* context, uint8_t level, const char* message);
    uint64_t (*now_ms)(void* context);

    bool (*create_chat_commands)(void* context,
                                 const CommandDefinition* commands,
                                 size_t command_count);
};
```

`log` writes through the server logger with the plugin id attached.

`now_ms` returns a monotonic millisecond timestamp.

`create_chat_commands` registers slash-style commands owned by the calling
plugin. It requires the `create_chat_commands` permission.

The host table also exposes the planned bot APIs:

```cpp
bool (*create_bot_user)(void* context,
                        const char* key,
                        const char* display_name,
                        BotHandle* out_bot,
                        UserId* out_user_id);

bool (*destroy_bot_user)(void* context, BotHandle bot);

bool (*set_bot_display_name)(void* context,
                             BotHandle bot,
                             const char* display_name);

bool (*send_bot_chat)(void* context,
                      BotHandle bot,
                      ChannelId text_channel_id,
                      const char* text,
                      MessageId* out_message_id);

bool (*join_bot_voice)(void* context,
                       BotHandle bot,
                       ChannelId voice_channel_id);

bool (*leave_bot_voice)(void* context, BotHandle bot);

bool (*send_bot_voice_packet)(void* context,
                              BotHandle bot,
                              uint16_t sequence,
                              const uint8_t* opus_payload,
                              size_t opus_payload_len);
```

The bot APIs are wired on the server. Bot voice functions validate plugin
ownership, channel existence, permissions, channel capacity, and Opus payload
size before using the normal channel presence and voice fan-out paths.

## Registration Model

Plugins fill a registration table during `parties_plugin_init`:

```cpp
struct Registration {
    AbiHeader abi;

    void (*on_server_started)();
    void (*on_server_stopping)();

    void (*on_session_authenticated)(SessionId session);
    void (*on_session_disconnected)(SessionId session,
                                    UserId user_id,
                                    ChannelId voice_channel_id);

    void (*on_chat_message)(const ChatMessage* message, ChatDecision* decision);
    void (*on_chat_command)(const ChatCommandInvocation* invocation);
};
```

Callbacks are optional. Null callbacks are ignored.

Current callback timing:

- `on_server_started` runs after plugin loading and server startup.
- `on_server_stopping` runs while the plugin manager shuts down.
- `on_session_authenticated` runs after a session has authenticated.
- `on_session_disconnected` runs during disconnect cleanup.
- `on_chat_message` runs before user chat is stored or broadcast.
- `on_chat_command` runs when a registered slash command is invoked.

## Chat Message Model

User chat interception uses:

```cpp
struct ChatMessage {
    AbiHeader abi;
    SessionId session_id;
    UserId author_user_id;
    ChannelId text_channel_id;
    const char* author_name;
    const char* text;
    uint8_t attachment_count;
};
```

The server passes this to `on_chat_message` after core parsing, channel
validation, and length validation, but before database insertion and broadcast.

Pointers in `ChatMessage` are valid only during the callback. Plugins must copy
strings they want to keep.

The plugin returns a decision through:

```cpp
enum class ChatDecisionCode : uint8_t {
    Continue = 0,
    Reject = 1,
    ReplaceText = 2,
};

struct ChatDecision {
    AbiHeader abi;
    uint8_t code;
    const char* replacement_text;
    const char* rejection_reason;
};
```

Decision behavior:

- `Continue` leaves the message unchanged.
- `Reject` prevents storage and broadcast.
- `ReplaceText` stores and broadcasts `replacement_text`.
- `Reject` and `ReplaceText` require `moderate_chat`.
- A plugin with only `read_chat` may observe messages but moderation decisions
  are ignored.
- Multiple plugins run in load order. If one plugin replaces text, later plugins
  see the updated text. If one plugin rejects the message, dispatch stops.

## Chat Command Model

Plugins define commands with:

```cpp
struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
};
```

Rules:

- Requires `create_chat_commands`.
- `name` does not include the leading slash.
- `name` must be non-empty, at most 64 bytes, and contain only ASCII
  alphanumeric characters, `_`, or `-`.
- Command names are globally unique across loaded plugins.
- `description` is client-displayable text.
- `usage` is a compact usage string such as `/play <url>`.

The server advertises commands to clients with:

```cpp
CHAT_COMMAND_LIST = 0x0509
```

Payload:

```text
count u16
repeated count times:
  name string
  description string
  usage string
```

The server sends the list after authentication together with the other initial
server state.

Command invocation uses normal chat input. If a user sends `/name args`, the
server checks whether `name` is registered. Registered commands are dispatched
to the owning plugin and are not stored as normal chat.

Invocation data:

```cpp
struct ChatCommandInvocation {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    const char* command_name;
    const char* args;
    const char* raw_text;
};
```

Pointers in `ChatCommandInvocation` are valid only during the callback.

## Permissions

Current permissions:

| Permission | Status | Meaning |
| --- | --- | --- |
| `read_chat` | Implemented | Allows `on_chat_message` observation. |
| `moderate_chat` | Implemented | Allows chat rejection and replacement. |
| `create_chat_commands` | Implemented | Allows command registration. |

Planned v1 permissions:

| Permission | Meaning |
| --- | --- |
| `read_sessions` | Read authenticated session snapshots. |
| `create_bot_users` | Create and destroy server-owned bot users. |
| `send_bot_chat` | Send text messages as bot users. |
| `join_bot_voice` | Join and leave voice channels as bot users. |
| `send_bot_audio` | Send Opus packets as bot users. |

Native plugins are trusted in-process code. Permissions are server policy and
operator intent, not a sandbox.

## Bot User Model

Bot users are planned v1 server-owned participants. They are not authenticated
QUIC sessions.

Target properties:

- A bot is owned by exactly one plugin.
- A plugin may create zero, one, or many bot users.
- Each successful creation returns a distinct `BotHandle`.
- A bot has an opaque `BotHandle` for plugin control.
- A bot also has a normal `UserId` so chat messages and voice presence can use
  existing client-facing user models.
- Bot users are reused across plugin restarts. With the current API, the stable
  identity key is `plugin_id + key`, so calling `create_bot_user` again with the
  same key reuses the existing synthetic user row.
- `key` is plugin-defined identity and should not change. `display_name` is
  client-facing presentation text and can be changed later.
- Bot users are stored in `users` with `is_bot = 1`, `bot_owner_plugin`, and
  `bot_key` metadata.
- Bot lifetime is tied to the owning plugin. Plugins may destroy individual
  bots, and plugin shutdown destroys all bots owned by that plugin.
- Bots can join voice channels and appear in channel user lists.

Planned host calls:

```cpp
bool (*create_bot_user)(void* context,
                        const char* key,
                        const char* display_name,
                        BotHandle* out_bot,
                        UserId* out_user_id);

bool (*destroy_bot_user)(void* context, BotHandle bot);

bool (*set_bot_display_name)(void* context,
                             BotHandle bot,
                             const char* display_name);
```

Persistence should use synthetic server-owned users so existing message storage
can keep `sender_id` as a normal user reference. A future schema migration may
add an explicit bot marker.

## Bot Chat Model

Bot chat is planned v1 host functionality.

```cpp
bool (*send_bot_chat)(void* context,
                      BotHandle bot,
                      ChannelId text_channel_id,
                      const char* text,
                      MessageId* out_message_id);
```

Rules:

- Requires `send_bot_chat`.
- The bot must belong to the calling plugin.
- Multiple bots from the same plugin may send chat independently.
- The target text channel must exist.
- Text follows the server's normal length validation.
- Bot chat is stored and broadcast like normal chat.
- Bot chat should not recursively invoke `on_chat_message` by default.

## Bot Audio Model

Bot audio is planned v1 host functionality for music bots and similar
integrations.

```cpp
bool (*join_bot_voice)(void* context,
                       BotHandle bot,
                       ChannelId voice_channel_id);

bool (*leave_bot_voice)(void* context, BotHandle bot);

bool (*send_bot_voice_packet)(void* context,
                              BotHandle bot,
                              uint16_t sequence,
                              const uint8_t* opus_payload,
                              size_t opus_payload_len);
```

Packet expectations:

- Payload is Opus, not PCM.
- Sample rate is 48 kHz.
- Frame duration should match the normal voice path, currently 20 ms.
- The plugin provides only the Opus payload and sequence number.
- The server builds and forwards the normal voice packet shape.

Rules:

- `join_bot_voice` and `leave_bot_voice` require `join_bot_voice`.
- `send_bot_voice_packet` requires `send_bot_audio`.
- A bot must be in a voice channel before it sends audio.
- Multiple bots from the same plugin may be present in different voice channels,
  or in the same voice channel if the server's normal channel capacity allows it.
- Audio is forwarded to authenticated users in the same voice channel who are
  not deafened.
- The plugin is responsible for decoding, resampling, Opus encoding, and pacing.

## Failure Handling

Current behavior:

- Global plugin loading can be disabled.
- Missing plugin directory logs a warning and continues server startup.
- One bad plugin manifest or library does not stop other plugins from loading.
- `parties_plugin_init` returning `false` disables that plugin.
- Missing `parties_plugin_init` disables that plugin.
- Missing `parties_plugin_shutdown` is allowed.
- Invalid or duplicate commands are rejected and logged.
- Missing permissions make host calls fail or moderation decisions ignored.

Native plugin crashes can crash the server process. Process isolation is not a
v1 goal.

## Implementation Status

Implemented:

- Plugin API header and ABI version `1.0`.
- Plugin config: `[plugins] enabled`, `[plugins] directory`.
- Per-plugin allow config and granted permissions.
- Manifest discovery through recursive `plugin.toml` scanning.
- Native library loading on Windows, Linux, and macOS.
- Init and optional shutdown exports.
- Lifecycle/session callbacks.
- `read_chat`, `moderate_chat`, and `create_chat_commands`.
- Chat command registration, client advertisement, and dispatch.
- Chat interception with continue, reject, and replace decisions.
- Server-owned bot handle creation.
- Multiple bot users per plugin.
- Reuse of existing synthetic bot users across plugin restarts.
- Bot display-name updates.
- Bot persistence metadata: `users.is_bot`, `users.bot_owner_plugin`,
  `users.bot_key`.
- Bot chat storage and broadcast through the normal `CHAT_MESSAGE` payload.
- Bot voice join and leave presence.
- Bot voice participants in `CHANNEL_LIST` counts and `CHANNEL_USER_LIST`.
- Bot Opus packet fan-out through the normal voice datagram shape.
- Example plugin `parties.example.bot_echo`, which registers `/botping` and
  replies through bot chat.

Not implemented yet:

- Session snapshot host APIs.
- Music bot sample plugin.
