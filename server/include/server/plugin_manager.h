#pragma once

#include <server/config.h>
#include <parties/plugin_api.h>

#include <filesystem>
#include <memory>
#include <string>
#include <string_view>
#include <unordered_set>
#include <vector>

namespace parties::server {

class PluginManager {
public:
    PluginManager();
    ~PluginManager();

    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

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

    const std::vector<ChatCommand>& chat_commands() const { return chat_commands_; }
    bool enabled() const { return enabled_; }

private:
    struct NativeLibrary;
    struct Plugin;

    static void host_log(void* context, uint8_t level, const char* message);
    static uint64_t host_now_ms(void* context);
    static bool host_create_chat_commands(void* context,
                                          const plugin::CommandDefinition* commands,
                                          size_t command_count);

    bool load_manifest(const std::filesystem::path& manifest_path);
    bool register_chat_commands(Plugin& plugin,
                                const plugin::CommandDefinition* commands,
                                size_t command_count);
    bool has_permission(const Plugin& plugin, std::string_view permission) const;
    Plugin* find_command_owner(std::string_view command_name) const;

    bool enabled_ = false;
    std::vector<std::unique_ptr<Plugin>> plugins_;
    std::vector<ChatCommand> chat_commands_;
};

} // namespace parties::server
