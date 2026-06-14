#pragma once

#include <cstddef>
#include <cstdint>

namespace parties::plugin {

constexpr uint16_t API_VERSION_MAJOR = 1;
constexpr uint16_t API_VERSION_MINOR = 0;

struct AbiHeader {
    uint32_t size;
    uint16_t api_major;
    uint16_t api_minor;
};

using SessionId = uint32_t;
using UserId = uint32_t;
using ChannelId = uint32_t;
using MessageId = uint64_t;

struct Bot;
using BotHandle = Bot*;

enum class LogLevel : uint8_t {
    Trace = 0,
    Debug = 1,
    Info = 2,
    Warn = 3,
    Error = 4,
};

struct CommandDefinition {
    AbiHeader abi;
    const char* name;
    const char* description;
    const char* usage;
};

struct ChatCommandInvocation {
    AbiHeader abi;
    SessionId session_id;
    UserId user_id;
    ChannelId text_channel_id;
    const char* command_name;
    const char* args;
    const char* raw_text;
};

struct ChatMessage {
    AbiHeader abi;
    SessionId session_id;
    UserId author_user_id;
    ChannelId text_channel_id;
    const char* author_name;
    const char* text;
    uint8_t attachment_count;
};

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

struct Host {
    AbiHeader abi;
    void* context;

    void (*log)(void* context, uint8_t level, const char* message);
    uint64_t (*now_ms)(void* context);

    bool (*create_chat_commands)(void* context,
                                 const CommandDefinition* commands,
                                 size_t command_count);

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
};

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

using InitFn = bool (*)(const Host* host, Registration* registration);
using ShutdownFn = void (*)();

template <typename T>
constexpr AbiHeader make_abi_header() {
    return AbiHeader{static_cast<uint32_t>(sizeof(T)), API_VERSION_MAJOR, API_VERSION_MINOR};
}

} // namespace parties::plugin
