# Bot Query Input API Proposal

> Draft design for Discord-like command query input and autocomplete.

## Goal

Add an interactive command input flow for bot commands such as `/play`.

The user should be able to type:

```text
/play never gonna
```

and receive live plugin-provided results before pressing Enter. The user can then
choose a result or keep typing. Pressing Enter invokes the command with the final
typed value or the selected result.

This is not only static command autocomplete. It is a live query channel between
the client, server, and the command-owning plugin.

## Current State

The plugin API already supports slash-style chat commands:

- Plugins register commands with `create_chat_commands`.
- The server advertises registered commands to clients with `CHAT_COMMAND_LIST`.
- The client sends normal chat text, for example `/play https://example`.
- The server parses and dispatches the command only after Enter.
- Plugins receive `on_chat_command`.

That model works for direct URLs and simple arguments, but it cannot support
search-as-you-type flows where `/play <query>` needs remote results from YouTube,
SoundCloud, local media indexes, or other providers.

## Proposed UX

1. User types `/play`.
2. Client opens a command suggestion panel using `CHAT_COMMAND_LIST`.
3. User starts typing the first argument.
4. Client sends debounced query updates to the server.
5. Server forwards valid updates to the plugin that owns `play`.
6. Plugin returns a bounded list of results.
7. Client replaces stale result lists only when the response matches the latest
   request id.
8. User picks a result or keeps typing.
9. Pressing Enter sends the normal command invocation.

Example:

```text
input: /play never gonna

results:
1. Rick Astley - Never Gonna Give You Up
2. Never Gonna Give You Up - Official Audio
3. Never Gonna Give You Up - Live
```

If the user selects result 1, the client sends a command invocation containing
the result value, usually a URL or provider-specific opaque token.

## Suggested Usage

The Discord-style model is: autocomplete belongs to one command argument, not to
the whole command. For a music bot, `/play` has a `query` argument that supports
live suggestions.

Plugin command declaration:

```cpp
CommandInputDefinition play_query{};
play_query.abi = make_abi_header<CommandInputDefinition>();
play_query.argument_name = "query";
play_query.mode = static_cast<uint8_t>(CommandInputMode::LiveQuery);
play_query.min_chars = 2;
play_query.debounce_ms = 250;
play_query.max_results = 8;
play_query.placeholder = "Paste a URL or search";

CommandDefinition play{};
play.abi = make_abi_header<CommandDefinition>();
play.name = "play";
play.description = "Play a track in the current voice channel.";
play.usage = "/play {query:string...}";
play.min_role = 3;
play.inputs = &play_query;
play.input_count = 1;

host->create_chat_commands(host->context, &play, 1);
```

Server advertises the existing command list, then sends input metadata in a
separate compatibility-safe message so older clients can keep reading the old
`CHAT_COMMAND_LIST` shape:

```text
CHAT_COMMAND_LIST
  command_count = 1
  command:
    name = "play"
    description = "Play a track in the current voice channel."
    usage = "/play {query:string...}"

CHAT_COMMAND_INPUT_LIST
  command_count = 1
  command:
    name = "play"
    input_count = 1
    input:
      argument_name = "query"
      mode = LiveQuery
      min_chars = 2
      debounce_ms = 250
      max_results = 8
      placeholder = "Paste a URL or search"
```

Client behavior while typing:

```text
User input: /play ne

Client parses:
  command_name = "play"
  focused_argument = "query"
  argument_value = "ne"

Because query.mode == LiveQuery and len("ne") >= min_chars,
client sends CHAT_COMMAND_QUERY.
```

Autocomplete request:

```text
CHAT_COMMAND_QUERY
  channel_id = 10
  request_id = 42
  command_name = "play"
  argument_name = "query"
  query = "ne"
  cursor_pos = 2
```

Plugin autocomplete callback:

```cpp
thread_local std::vector<CommandQueryResult> query_results;

void on_chat_command_query(const CommandQueryRequest* request,
                           CommandQueryResponse* response) {
    if (std::strcmp(request->command_name, "play") != 0 ||
        std::strcmp(request->argument_name, "query") != 0) {
        response->status = static_cast<uint8_t>(CommandQueryStatus::NoResults);
        return;
    }

    // If this is a URL, the plugin can return no suggestions or one direct
    // "Play this URL" suggestion. If it is plain text, search a provider.
    query_results = search_tracks(request->query);

    response->status = static_cast<uint8_t>(CommandQueryStatus::Ok);
    response->results = query_results.data();
    response->result_count = query_results.size();
}
```

Autocomplete response:

```text
CHAT_COMMAND_QUERY_RESP
  request_id = 42
  command_name = "play"
  argument_name = "query"
  status = Ok
  result_count = 3
  result:
    id = "yt:abc"
    title = "Example Song"
    subtitle = "Example Artist"
    value = "https://youtube.example/watch?v=abc"
    kind = "track"
    duration_ms = 213000
    thumbnail_url = "https://..."
```

Final execution stays separate:

```text
User selects the first result.
Client input becomes:
  /play https://youtube.example/watch?v=abc

User presses Enter.
Client sends existing CHAT_SEND:
  channel_id = 10
  text = "/play https://youtube.example/watch?v=abc"
```

Then the existing command path calls `on_chat_command`:

```cpp
void on_chat_command(const ChatCommandInvocation* invocation) {
    // invocation->command_name == "play"
    // invocation->args == "https://youtube.example/watch?v=abc"
    // parsed query argument contains the selected URL.
}
```

## Design Principles

- Keep command execution and query preview separate.
- Reuse existing slash command ownership and authorization.
- Do not store query preview requests as chat messages.
- Require explicit command opt-in, because many commands do not need live input.
- Make stale responses harmless with client-generated request ids.
- Bound payload sizes and result counts.
- Let plugins return opaque values so providers are not forced into URL-only
  workflows.
- Preserve compatibility with existing plugins and clients.

## Command Registration Extension

Extend `CommandDefinition` with optional interactive input metadata.

```cpp
enum class CommandInputMode : uint8_t {
    None = 0,
    LiveQuery = 1,
};

struct CommandInputDefinition {
    parties::plugin::AbiHeader abi;
    const char* argument_name;
    uint8_t mode;
    uint16_t min_chars;
    uint16_t debounce_ms;
    uint16_t max_results;
    const char* placeholder;
};
```

Add optional fields to `CommandDefinition` in plugin API v1.1:

```cpp
struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
    uint8_t min_role;

    const CommandInputDefinition* inputs;
    size_t input_count;
};
```

Example:

```cpp
CommandInputDefinition play_query{};
play_query.abi = make_abi_header<CommandInputDefinition>();
play_query.argument_name = "query";
play_query.mode = static_cast<uint8_t>(CommandInputMode::LiveQuery);
play_query.min_chars = 2;
play_query.debounce_ms = 250;
play_query.max_results = 8;
play_query.placeholder = "Paste a URL or search";

CommandDefinition play{};
play.abi = make_abi_header<CommandDefinition>();
play.name = "play";
play.description = "Play a track in the current voice channel.";
play.usage = "/play {query:string...}";
play.min_role = 3;
play.inputs = &play_query;
play.input_count = 1;
```

Older plugins keep `abi.size` smaller and simply do not expose interactive input
metadata.

## Protocol Messages

Add these control messages. Exact numeric ids can be assigned during
implementation; the names below describe the contract.

### CHAT_COMMAND_INPUT_LIST

Server to client.

This is sent after `CHAT_COMMAND_LIST` to clients that advertise protocol support
for command input metadata.

```text
[command_count u16]
repeated command:
  [command_name string]
  [input_count u16]
  repeated input:
    [argument_name string]
    [mode u8]
    [min_chars u16]
    [debounce_ms u16]
    [max_results u16]
    [placeholder string]
```

`CHAT_COMMAND_LIST` intentionally remains `[count][name, description, usage]...`
so older clients do not misread per-command extension fields.

### CHAT_COMMAND_QUERY

Client to server.

```text
[channel_id u32]
[request_id u64]
[command_name string]
[argument_name string]
[query string]
[cursor_pos u16]
```

Rules:

- `request_id` is generated by the client and increases per input box.
- `channel_id` is the text channel where the command would be executed.
- `command_name` does not include the leading slash.
- `argument_name` must match a registered live-query input.
- `query` is the current argument text, not the full chat input.
- `cursor_pos` is measured in UTF-8 bytes for the query string.

### CHAT_COMMAND_QUERY_RESP

Server to client.

```text
[request_id u64]
[command_name string]
[argument_name string]
[status u8]
[message string]
[result_count u16]
repeated result:
  [id string]
  [title string]
  [subtitle string]
  [value string]
  [kind string]
  [duration_ms u32]
  [thumbnail_url string]
```

Result fields:

| Field | Meaning |
| --- | --- |
| `id` | Stable result id within the provider response. |
| `title` | Main display text. |
| `subtitle` | Secondary display text, such as channel or album. |
| `value` | Value inserted into the command if selected. |
| `kind` | Optional category, for example `url`, `track`, `playlist`, `user`. |
| `duration_ms` | Optional media duration; `0` means unknown or not applicable. |
| `thumbnail_url` | Optional preview image URL. |

Status values:

| Value | Name | Meaning |
| --- | --- | --- |
| `0` | `Ok` | Results are usable. |
| `1` | `NoResults` | Query completed with no matches. |
| `2` | `TooShort` | Query is shorter than `min_chars`. |
| `3` | `RateLimited` | Client should slow down. |
| `4` | `PluginError` | Plugin failed but command input can continue. |
| `5` | `PermissionDenied` | User cannot query this command. |
| `6` | `Pending` | Plugin accepted the query and will complete asynchronously. This is a plugin-to-host callback result, not a final client response. |

### CHAT_COMMAND_QUERY_CANCEL

Client to server.

```text
[request_id u64]
[command_name string]
[argument_name string]
```

Cancellation is best-effort. The server may still return a response, but the
client should ignore it if the request is no longer current.

### CHAT_COMMAND_SELECT

Optional client to server signal.

```text
[request_id u64]
[command_name string]
[argument_name string]
[result_id string]
[value string]
```

This is useful for plugin analytics, queue previews, or resolving short-lived
opaque result ids before final command execution. It should not execute the
command by itself.

The simple implementation can skip this message and only send the final
`CHAT_SEND` command after Enter.

## Plugin Callback Extension

Add a callback for live command queries.

```cpp
enum class CommandQueryStatus : uint8_t {
    Ok = 0,
    NoResults = 1,
    TooShort = 2,
    RateLimited = 3,
    PluginError = 4,
    PermissionDenied = 5,
    Pending = 6,
};

struct CommandQueryRequest {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    uint8_t caller_role;
    uint64_t request_id;
    const char* command_name;
    const char* argument_name;
    const char* query;
    uint16_t cursor_pos;
};

struct CommandQueryResult {
    AbiHeader abi;
    const char* id;
    const char* title;
    const char* subtitle;
    const char* value;
    const char* kind;
    uint32_t duration_ms;
    const char* thumbnail_url;
};

struct CommandQueryResponse {
    AbiHeader abi;
    uint8_t status;
    const char* message;
    const CommandQueryResult* results;
    size_t result_count;
};
```

Plugins that need slow provider calls can return `Pending` from
`on_chat_command_query`, then complete later from a plugin-owned worker thread:

```cpp
bool (*respond_to_command_query)(void* context,
                                 SessionId session_id,
                                 uint64_t request_id,
                                 const char* command_name,
                                 const char* argument_name,
                                 const CommandQueryResponse* response);
```

`respond_to_command_query` is a host function. It copies the supplied response
and marshals the send back onto the server thread. The plugin must pass the same
`session_id`, `request_id`, `command_name`, and `argument_name` it received in
the original `CommandQueryRequest`.

Extend `Registration`:

```cpp
void (*on_chat_command_query)(const CommandQueryRequest* request,
                              CommandQueryResponse* response);
```

Callback rules:

- The server calls this only for commands owned by the plugin.
- The same `min_role` authorization as command execution applies.
- The callback must be short. Slow provider calls should return `Pending`, then
  call `respond_to_command_query` from plugin-owned worker state.
- Returned result pointers must remain valid until the host has copied the
  response after the callback returns. Plugin-owned scratch storage is fine; do
  not return pointers to stack-local strings or arrays.
- The server enforces `max_results`, string length limits, and total payload
  size even if the plugin returns more.

## Server Behavior

On `CHAT_COMMAND_QUERY`:

1. Validate that the session is authenticated.
2. Resolve `command_name` to a registered plugin command.
3. Validate that the caller role satisfies `min_role`.
4. Validate that `argument_name` has `LiveQuery` enabled.
5. Enforce per-session rate limits.
6. Enforce `min_chars`, `max_results`, string lengths, and total message size.
7. Dispatch `on_chat_command_query`.
8. If the plugin returns `Pending`, record the pending request with a short TTL.
9. Copy and sanitize plugin results for non-pending responses.
10. Send `CHAT_COMMAND_QUERY_RESP` for non-pending responses.

On `respond_to_command_query`:

1. Validate that the responding plugin owns `command_name`.
2. Validate that `argument_name` is a live-query input on that command.
3. Consume a matching pending request for `session_id + request_id + command_name + argument_name`.
4. Drop responses for expired, disconnected, or unknown pending requests.
5. Copy and sanitize plugin results.
6. Send `CHAT_COMMAND_QUERY_RESP`.

Recommended server limits:

| Limit | Value |
| --- | --- |
| Query length | 512 bytes |
| Results per response | 10 default, 25 hard max |
| Title/subtitle/value/id/kind | 512 bytes each |
| Thumbnail URL | 2048 bytes |
| Response payload | 64 KiB |
| Per-session query rate | 5 request burst, 2 requests/sec refill |
| Pending async TTL | 10 seconds |
| Plugin callback warning | log if over 50 ms |

## Client Behavior

The client owns the interactive UI state.

- Parse the local input as the user types.
- Use `CHAT_COMMAND_LIST` to detect commands and interactive arguments.
- Debounce query messages using the command-provided `debounce_ms`.
- Do not send live queries before `min_chars`.
- Increment `request_id` for every new query.
- Cancel or ignore old requests when input changes command or argument.
- Ignore responses whose `request_id` is not the latest for the active input.
- Show `TooShort`, `NoResults`, and `PluginError` as non-blocking inline states.
- On result selection, insert `value` into the command argument.
- On Enter, send the existing `CHAT_SEND` command text.

For `/play`, selected result values should preferably be stable URLs or opaque
tokens that the owning plugin can resolve during `on_chat_command`.

## `/play` URL vs Search Handling

The music bot can support both cases with one command:

```text
/play {query:string...}
```

Recommended interpretation:

- If `query` parses as a supported URL, skip search and play/queue the URL.
- If `query` is plain text during live input, return search results.
- If the user presses Enter without selecting a result, run a best-match search
  and queue the top result, or ask the user to choose depending on bot policy.
- If a selected `value` is an opaque token, resolve it in `on_chat_command` and
  fail cleanly if the token expired.

## Backward Compatibility

This should be a plugin ABI minor-version extension:

- Existing plugins compiled for API 1.0 continue to load.
- Existing clients ignore the extra command metadata and keep sending final
  command text only.
- Servers should include interactive metadata in `CHAT_COMMAND_LIST` only when
  clients advertise support during auth or feature negotiation.
- Without client support, `/play <url>` still works exactly as today.

## Implementation Plan

1. Add command input metadata structs to `sdk/include/parties/plugin_api.h`. Done.
2. Teach `PluginManager` to copy optional input metadata from larger
   `CommandDefinition` structs. Done.
3. Extend the wire protocol with command query request/response messages. Done.
4. Add command query dispatch, async completion tracking, and rate limits on the server. Done.
5. Send `CHAT_COMMAND_INPUT_LIST` so clients know which arguments support live query. Done.
6. Add client API/model support for request ids, stale response filtering, and result storage. Done.
7. Update the example bot plugin with a live-query command and async sample response. Done.
8. Add tests for authorization, stale responses, rate limiting, payload limits,
   old ABI behavior, and normal `/play <url>` execution. Partially done; broaden as real plugins adopt the API.

## Open Questions

- Should `CHAT_COMMAND_LIST` grow to include interactive metadata, or should
  clients request details for one command at a time?
- Should result `value` be visible text inserted into the input, or should the
  client display one value while sending a hidden provider token?
- Should plugin query callbacks be synchronous only for v1.1, or should we add
  async completion handles immediately?
- Should the server cache query results per session to reduce provider load?
- Should command query support be exposed to all plugins, or require a new
  permission such as `answer_command_queries`?

## Implemented First Cut

The current API supports:

- One live-query argument per command.
- Debounced client requests.
- Synchronous `on_chat_command_query` for fast local results.
- Async completion with `Pending` plus `respond_to_command_query` for slow provider calls.
- Server-side pending request tracking with TTL.
- Per-session server rate limiting.
- Bounded result list copied by the server.
- No `CHAT_COMMAND_SELECT` until there is a concrete need.
- Commands such as `/play` can support direct URLs and plain-text search with
  selected result values inserted as URLs or opaque plugin tokens.

This gives the Discord-like `/play` experience while keeping final execution on
the existing command path.
