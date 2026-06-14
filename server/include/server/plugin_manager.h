#pragma once

#include <server/config.h>
#include <parties/plugin_api.h>

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace parties::server {

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    struct HostServices {
        std::function<std::optional<plugin::UserId>(std::string_view plugin_id,
                                                    std::string_view key,
                                                    std::string_view display_name)> create_bot_user;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name)> set_bot_display_name;
        std::function<std::optional<plugin::MessageId>(plugin::UserId user_id,
                                                       std::string_view display_name,
                                                       plugin::ChannelId text_channel_id,
                                                       std::string_view text)> send_bot_chat;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name,
                           plugin::ChannelId voice_channel_id)> join_bot_voice;
        std::function<bool(plugin::UserId user_id,
                           std::string_view display_name,
                           plugin::ChannelId voice_channel_id)> leave_bot_voice;
        std::function<bool(plugin::UserId user_id,
                           plugin::ChannelId voice_channel_id,
                           uint16_t sequence,
                           const uint8_t* opus_payload,
                           size_t opus_payload_len)> send_bot_voice_packet;
    };

    void set_host_services(HostServices services);

    bool load(const PluginConfig& cfg);
    void shutdown();

    void on_server_started();
    void on_server_stopping();
    void on_session_authenticated(plugin::SessionId session);
    void on_session_disconnected(plugin::SessionId session,
                                 plugin::UserId user_id,
                                 plugin::ChannelId voice_channel_id);

    bool dispatch_chat_command(plugin::SessionId session,
                               plugin::UserId user_id,
                               plugin::ChannelId text_channel_id,
                               std::string_view command_name,
                               std::string_view args,
                               std::string_view raw_text);

    enum class ChatResultCode {
        Continue,
        Reject,
    };

    struct ChatResult {
        ChatResultCode code = ChatResultCode::Continue;
        std::string error_message;
    };

    ChatResult process_chat_message(plugin::SessionId session,
                                    plugin::UserId user_id,
                                    plugin::ChannelId text_channel_id,
                                    std::string_view author_name,
                                    std::string& text,
                                    uint8_t attachment_count);

    struct ChatCommand {
        std::string plugin_id;
        std::string name;
        std::string description;
        std::string usage;
    };

    struct BotVoiceParticipant {
        plugin::UserId user_id = 0;
        plugin::ChannelId voice_channel_id = 0;
        std::string display_name;
    };

    const std::vector<ChatCommand>& chat_commands() const { return chat_commands_; }
    bool enabled() const { return enabled_; }
    std::vector<BotVoiceParticipant> bot_voice_participants(plugin::ChannelId channel_id = 0) const;
    uint32_t bot_voice_count(plugin::ChannelId channel_id) const;
    void clear_bot_voice_channel(plugin::ChannelId channel_id);

private:
    struct NativeLibrary;
    struct Plugin;
    struct Bot;
    struct PluginGrant;

    static void host_log(void* context, uint8_t level, const char* message);
    static uint64_t host_now_ms(void* context);
    static bool host_create_chat_commands(void* context,
                                          const plugin::CommandDefinition* commands,
                                          size_t command_count);
    static bool host_create_bot_user(void* context,
                                     const char* key,
                                     const char* display_name,
                                     plugin::BotHandle* out_bot,
                                     plugin::UserId* out_user_id);
    static bool host_destroy_bot_user(void* context, plugin::BotHandle bot);
    static bool host_set_bot_display_name(void* context,
                                          plugin::BotHandle bot,
                                          const char* display_name);
    static bool host_send_bot_chat(void* context,
                                   plugin::BotHandle bot,
                                   plugin::ChannelId text_channel_id,
                                   const char* text,
                                   plugin::MessageId* out_message_id);
    static bool host_join_bot_voice(void* context,
                                    plugin::BotHandle bot,
                                    plugin::ChannelId voice_channel_id);
    static bool host_leave_bot_voice(void* context, plugin::BotHandle bot);
    static bool host_send_bot_voice_packet(void* context,
                                           plugin::BotHandle bot,
                                           uint16_t sequence,
                                           const uint8_t* opus_payload,
                                           size_t opus_payload_len);

    bool load_manifest(const std::filesystem::path& manifest_path);
    bool register_chat_commands(Plugin& plugin,
                                const plugin::CommandDefinition* commands,
                                size_t command_count);
    Bot* get_owned_bot(Plugin& plugin, plugin::BotHandle bot) const;
    bool check_host_permission(Plugin& plugin, std::string_view permission, std::string_view action) const;
    bool has_permission(const Plugin& plugin, std::string_view permission) const;
    Plugin* find_command_owner(std::string_view command_name) const;

    HostServices services_;
    std::unordered_map<std::string, PluginGrant> grants_;
    bool enabled_ = false;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<ChatCommand> chat_commands_;
};

} // namespace parties::server
