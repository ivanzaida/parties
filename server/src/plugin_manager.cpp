#include <server/plugin_manager.h>

#include <parties/log.h>

#include <toml.hpp>

#include <algorithm>
#include <chrono>
#include <cctype>
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#else
#include <dlfcn.h>
#endif

namespace parties::plugin {
struct Bot {};
} // namespace parties::plugin

namespace parties::server {

struct PluginManager::NativeLibrary {
#ifdef _WIN32
    HMODULE handle = nullptr;
#else
    void* handle = nullptr;
#endif

    ~NativeLibrary() { close(); }

    bool open(const std::filesystem::path& path) {
#ifdef _WIN32
        handle = LoadLibraryW(path.wstring().c_str());
#else
        handle = dlopen(path.string().c_str(), RTLD_NOW | RTLD_LOCAL);
#endif
        return handle != nullptr;
    }

    void close() {
        if (!handle) return;
#ifdef _WIN32
        FreeLibrary(handle);
#else
        dlclose(handle);
#endif
        handle = nullptr;
    }

    void* symbol(const char* name) const {
        if (!handle) return nullptr;
#ifdef _WIN32
        return reinterpret_cast<void*>(GetProcAddress(handle, name));
#else
        return dlsym(handle, name);
#endif
    }
};

struct PluginManager::Bot : plugin::Bot {
    Plugin* owner = nullptr;
    plugin::UserId user_id = 0;
    plugin::ChannelId voice_channel_id = 0;
    std::string display_name;
    bool destroyed = false;
};

struct PluginManager::PluginGrant {
    bool enabled = true;
    std::unordered_set<std::string> permissions;
};

struct PluginManager::Plugin {
    PluginManager* manager = nullptr;
    std::string id;
    std::string name;
    std::string version;
    std::filesystem::path manifest_path;
    std::filesystem::path library_path;
    std::unordered_set<std::string> permissions;
    NativeLibrary library;
    plugin::Registration registration{};
    plugin::ShutdownFn shutdown = nullptr;
    std::vector<std::unique_ptr<Bot>> bots;
};

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    shutdown();
}

void PluginManager::set_host_services(HostServices services) {
    services_ = std::move(services);
}

static bool valid_command_name(std::string_view name) {
    if (name.empty() || name.size() > 64) return false;
    for (char c : name) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!(std::isalnum(uc) || c == '_' || c == '-'))
            return false;
    }
    return true;
}

static std::vector<std::string> read_string_array(const toml::value& value,
                                                  const char* key) {
    std::vector<std::string> result;
    if (!value.contains(key)) return result;

    try {
        result = toml::find<std::vector<std::string>>(value, key);
    } catch (const std::exception& e) {
        LOG_WARN("Plugin manifest field '{}' is not a string array: {}", key, e.what());
    }
    return result;
}

bool PluginManager::load(const PluginConfig& cfg) {
    shutdown();
    enabled_ = cfg.enabled;
    if (!enabled_) {
        LOG_INFO("Plugin loading disabled");
        return true;
    }

    grants_.clear();
    for (const auto& allow : cfg.allow) {
        PluginGrant grant;
        grant.enabled = allow.enabled;
        for (const auto& permission : allow.permissions)
            grant.permissions.insert(permission);
        grants_[allow.id] = std::move(grant);
    }
    if (grants_.empty())
        LOG_WARN("Plugin loading enabled, but no plugins are allowed in config");

    std::filesystem::path dir = cfg.directory;
    if (!std::filesystem::exists(dir)) {
        LOG_WARN("Plugin directory '{}' does not exist", dir.string());
        return true;
    }

    size_t loaded = 0;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(dir)) {
        if (!entry.is_regular_file()) continue;
        if (entry.path().filename() != "plugin.toml") continue;
        if (load_manifest(entry.path()))
            ++loaded;
    }

    LOG_INFO("Loaded {} plugin(s), {} chat command(s)", loaded, chat_commands_.size());
    return true;
}

void PluginManager::shutdown() {
    for (auto it = plugins_.rbegin(); it != plugins_.rend(); ++it) {
        Plugin& plugin = **it;
        if (plugin.registration.on_server_stopping)
            plugin.registration.on_server_stopping();
        if (plugin.shutdown)
            plugin.shutdown();
    }
    plugins_.clear();
    chat_commands_.clear();
    grants_.clear();
    enabled_ = false;
}

bool PluginManager::load_manifest(const std::filesystem::path& manifest_path) {
    auto parsed = toml::try_parse(manifest_path.string());
    if (parsed.is_err()) {
        LOG_WARN("Failed to parse plugin manifest '{}'", manifest_path.string());
        return false;
    }
    auto manifest = std::move(parsed.unwrap());

    auto plugin = std::make_unique<Plugin>();
    plugin->manager = this;
    plugin->manifest_path = manifest_path;
    plugin->id = toml::find_or(manifest, "id", std::string{});
    plugin->name = toml::find_or(manifest, "name", plugin->id);
    plugin->version = toml::find_or(manifest, "version", std::string{});
    std::string api_version = toml::find_or(manifest, "api_version", std::string{});
    std::string library = toml::find_or(manifest, "library", std::string{});

    if (plugin->id.empty() || library.empty()) {
        LOG_WARN("Plugin manifest '{}' is missing id or library", manifest_path.string());
        return false;
    }
    if (api_version != "1.0") {
        LOG_WARN("Plugin '{}' has unsupported api_version '{}'", plugin->id, api_version);
        return false;
    }

    auto grant_it = grants_.find(plugin->id);
    if (grant_it == grants_.end()) {
        LOG_WARN("Plugin '{}' is not allowed by server config", plugin->id);
        return false;
    }
    if (!grant_it->second.enabled) {
        LOG_INFO("Plugin '{}' is disabled by server config", plugin->id);
        return false;
    }

    auto requested_permissions = read_string_array(manifest, "permissions");
    for (auto& perm : requested_permissions) {
        if (grant_it->second.permissions.find(perm) != grant_it->second.permissions.end()) {
            plugin->permissions.insert(std::move(perm));
        } else {
            LOG_WARN("Plugin '{}' requested ungranted permission '{}'", plugin->id, perm);
        }
    }

    plugin->library_path = manifest_path.parent_path() / library;
    if (!plugin->library.open(plugin->library_path)) {
        LOG_WARN("Failed to load plugin '{}' from '{}'", plugin->id, plugin->library_path.string());
        return false;
    }

    auto init = reinterpret_cast<plugin::InitFn>(plugin->library.symbol("parties_plugin_init"));
    plugin->shutdown = reinterpret_cast<plugin::ShutdownFn>(plugin->library.symbol("parties_plugin_shutdown"));
    if (!init) {
        LOG_WARN("Plugin '{}' does not export parties_plugin_init", plugin->id);
        return false;
    }

    plugin::Host host{};
    host.abi = plugin::make_abi_header<plugin::Host>();
    host.context = plugin.get();
    host.log = &PluginManager::host_log;
    host.now_ms = &PluginManager::host_now_ms;
    host.create_chat_commands = &PluginManager::host_create_chat_commands;
    host.create_bot_user = &PluginManager::host_create_bot_user;
    host.destroy_bot_user = &PluginManager::host_destroy_bot_user;
    host.set_bot_display_name = &PluginManager::host_set_bot_display_name;
    host.send_bot_chat = &PluginManager::host_send_bot_chat;
    host.join_bot_voice = &PluginManager::host_join_bot_voice;
    host.leave_bot_voice = &PluginManager::host_leave_bot_voice;
    host.send_bot_voice_packet = &PluginManager::host_send_bot_voice_packet;

    plugin->registration = {};
    plugin->registration.abi = plugin::make_abi_header<plugin::Registration>();

    const size_t commands_before_init = chat_commands_.size();
    if (!init(&host, &plugin->registration)) {
        chat_commands_.resize(commands_before_init);
        LOG_WARN("Plugin '{}' init failed", plugin->id);
        return false;
    }

    LOG_INFO("Loaded plugin '{}' ({})", plugin->id, plugin->name);
    plugins_.push_back(std::move(plugin));
    return true;
}

void PluginManager::on_server_started() {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_server_started)
            plugin->registration.on_server_started();
    }
}

void PluginManager::on_server_stopping() {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_server_stopping)
            plugin->registration.on_server_stopping();
    }
}

void PluginManager::on_session_authenticated(plugin::SessionId session) {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_session_authenticated)
            plugin->registration.on_session_authenticated(session);
    }
}

void PluginManager::on_session_disconnected(plugin::SessionId session,
                                            plugin::UserId user_id,
                                            plugin::ChannelId voice_channel_id) {
    for (auto& plugin : plugins_) {
        if (plugin->registration.on_session_disconnected)
            plugin->registration.on_session_disconnected(session, user_id, voice_channel_id);
    }
}

std::vector<PluginManager::BotVoiceParticipant> PluginManager::bot_voice_participants(
    plugin::ChannelId channel_id) const {
    std::vector<BotVoiceParticipant> result;
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (bot->destroyed || bot->voice_channel_id == 0)
                continue;
            if (channel_id != 0 && bot->voice_channel_id != channel_id)
                continue;
            result.push_back(BotVoiceParticipant{
                bot->user_id,
                bot->voice_channel_id,
                bot->display_name,
            });
        }
    }
    return result;
}

uint32_t PluginManager::bot_voice_count(plugin::ChannelId channel_id) const {
    uint32_t count = 0;
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->voice_channel_id == channel_id)
                ++count;
        }
    }
    return count;
}

void PluginManager::clear_bot_voice_channel(plugin::ChannelId channel_id) {
    for (const auto& owner : plugins_) {
        for (const auto& bot : owner->bots) {
            if (!bot->destroyed && bot->voice_channel_id == channel_id)
                bot->voice_channel_id = 0;
        }
    }
}

bool PluginManager::dispatch_chat_command(plugin::SessionId session,
                                          plugin::UserId user_id,
                                          plugin::ChannelId text_channel_id,
                                          std::string_view command_name,
                                          std::string_view args,
                                          std::string_view raw_text) {
    Plugin* owner = find_command_owner(command_name);
    if (!owner)
        return false;

    if (!owner->registration.on_chat_command) {
        LOG_WARN("Plugin '{}' owns command '{}' but has no on_chat_command callback",
                 owner->id, command_name);
        return true;
    }

    std::string command_name_copy(command_name);
    std::string args_copy(args);
    std::string raw_text_copy(raw_text);

    plugin::ChatCommandInvocation invocation{};
    invocation.abi = plugin::make_abi_header<plugin::ChatCommandInvocation>();
    invocation.session_id = session;
    invocation.user_id = user_id;
    invocation.text_channel_id = text_channel_id;
    invocation.command_name = command_name_copy.c_str();
    invocation.args = args_copy.c_str();
    invocation.raw_text = raw_text_copy.c_str();

    owner->registration.on_chat_command(&invocation);
    return true;
}

PluginManager::ChatResult PluginManager::process_chat_message(
    plugin::SessionId session,
    plugin::UserId user_id,
    plugin::ChannelId text_channel_id,
    std::string_view author_name,
    std::string& text,
    uint8_t attachment_count) {
    ChatResult result;

    for (auto& owner : plugins_) {
        Plugin& plugin = *owner;
        if (!plugin.registration.on_chat_message)
            continue;
        if (!has_permission(plugin, "read_chat") && !has_permission(plugin, "moderate_chat"))
            continue;

        std::string author_name_copy(author_name);
        plugin::ChatMessage message{};
        message.abi = plugin::make_abi_header<plugin::ChatMessage>();
        message.session_id = session;
        message.author_user_id = user_id;
        message.text_channel_id = text_channel_id;
        message.author_name = author_name_copy.c_str();
        message.text = text.c_str();
        message.attachment_count = attachment_count;

        plugin::ChatDecision decision{};
        decision.abi = plugin::make_abi_header<plugin::ChatDecision>();
        decision.code = static_cast<uint8_t>(plugin::ChatDecisionCode::Continue);

        plugin.registration.on_chat_message(&message, &decision);

        auto code = static_cast<plugin::ChatDecisionCode>(decision.code);
        if (code == plugin::ChatDecisionCode::Continue)
            continue;

        if (!has_permission(plugin, "moderate_chat")) {
            LOG_WARN("Plugin '{}' tried to moderate chat without permission", plugin.id);
            continue;
        }

        if (code == plugin::ChatDecisionCode::Reject) {
            result.code = ChatResultCode::Reject;
            result.error_message = decision.rejection_reason && decision.rejection_reason[0]
                ? decision.rejection_reason
                : "Message rejected";
            return result;
        }

        if (code == plugin::ChatDecisionCode::ReplaceText) {
            if (decision.replacement_text) {
                text = decision.replacement_text;
            }
            continue;
        }

        LOG_WARN("Plugin '{}' returned invalid chat decision {}", plugin.id, decision.code);
    }

    return result;
}

void PluginManager::host_log(void* context, uint8_t level, const char* message) {
    auto* plugin = static_cast<Plugin*>(context);
    const char* id = plugin ? plugin->id.c_str() : "<unknown>";
    const char* msg = message ? message : "";
    switch (static_cast<plugin::LogLevel>(level)) {
    case plugin::LogLevel::Trace:
    case plugin::LogLevel::Debug:
        LOG_DEBUG("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Info:
        LOG_INFO("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Warn:
        LOG_WARN("[plugin:{}] {}", id, msg);
        break;
    case plugin::LogLevel::Error:
    default:
        LOG_ERROR("[plugin:{}] {}", id, msg);
        break;
    }
}

uint64_t PluginManager::host_now_ms(void*) {
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::milliseconds>(now).count());
}

bool PluginManager::host_create_chat_commands(void* context,
                                              const plugin::CommandDefinition* commands,
                                              size_t command_count) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    return plugin->manager->register_chat_commands(*plugin, commands, command_count);
}

bool PluginManager::host_create_bot_user(void* context,
                                         const char* key,
                                         const char* display_name,
                                         plugin::BotHandle* out_bot,
                                         plugin::UserId* out_user_id) {
    if (out_bot) *out_bot = nullptr;
    if (out_user_id) *out_user_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "create_bot_users", "create bot users"))
        return false;

    if (!key || !key[0]) {
        LOG_WARN("Plugin '{}' tried to create a bot user with an empty key", plugin->id);
        return false;
    }
    if (!display_name || !display_name[0]) {
        LOG_WARN("Plugin '{}' tried to create bot user '{}' with an empty display name", plugin->id, key);
        return false;
    }
    if (!plugin->manager->services_.create_bot_user) {
        LOG_WARN("Plugin '{}' tried to create a bot user, but bot user creation is unavailable", plugin->id);
        return false;
    }

    auto user_id = plugin->manager->services_.create_bot_user(plugin->id, key, display_name);
    if (!user_id || *user_id == 0) {
        LOG_WARN("Plugin '{}' failed to create bot user '{}' ('{}')",
                 plugin->id, key, display_name);
        return false;
    }

    auto bot = std::make_unique<Bot>();
    bot->owner = plugin;
    bot->user_id = *user_id;
    bot->display_name = display_name;

    plugin::BotHandle handle = static_cast<plugin::BotHandle>(bot.get());
    plugin->bots.push_back(std::move(bot));

    if (out_bot) *out_bot = handle;
    if (out_user_id) *out_user_id = *user_id;
    LOG_INFO("Plugin '{}' created bot user '{}' (user_id={})",
             plugin->id, display_name, *user_id);
    return true;
}

bool PluginManager::host_destroy_bot_user(void* context, plugin::BotHandle bot_handle) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "create_bot_users", "destroy bot users"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;

    bot->destroyed = true;
    bot->voice_channel_id = 0;
    LOG_INFO("Plugin '{}' destroyed bot user '{}' (user_id={})",
             plugin->id, bot->display_name, bot->user_id);
    return true;
}

bool PluginManager::host_set_bot_display_name(void* context,
                                              plugin::BotHandle bot_handle,
                                              const char* display_name) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "create_bot_users", "rename bot users"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;
    if (!display_name || !display_name[0]) {
        LOG_WARN("Plugin '{}' tried to rename a bot user to an empty display name", plugin->id);
        return false;
    }
    if (!plugin->manager->services_.set_bot_display_name) {
        LOG_WARN("Plugin '{}' tried to rename a bot user, but bot user updates are unavailable", plugin->id);
        return false;
    }
    if (!plugin->manager->services_.set_bot_display_name(bot->user_id, display_name)) {
        LOG_WARN("Plugin '{}' failed to rename bot user {} to '{}'",
                 plugin->id, bot->user_id, display_name);
        return false;
    }

    bot->display_name = display_name;
    return true;
}

bool PluginManager::host_send_bot_chat(void* context,
                                       plugin::BotHandle bot_handle,
                                       plugin::ChannelId text_channel_id,
                                       const char* text,
                                       plugin::MessageId* out_message_id) {
    if (out_message_id) *out_message_id = 0;

    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "send_bot_chat", "send bot chat"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;
    if (!text) text = "";
    if (!plugin->manager->services_.send_bot_chat) {
        LOG_WARN("Plugin '{}' tried to send bot chat, but bot chat is unavailable", plugin->id);
        return false;
    }

    auto message_id = plugin->manager->services_.send_bot_chat(
        bot->user_id, bot->display_name, text_channel_id, text);
    if (!message_id || *message_id == 0) {
        LOG_WARN("Plugin '{}' failed to send bot chat as '{}' (user_id={})",
                 plugin->id, bot->display_name, bot->user_id);
        return false;
    }

    if (out_message_id) *out_message_id = *message_id;
    return true;
}

bool PluginManager::host_join_bot_voice(void* context,
                                        plugin::BotHandle bot_handle,
                                        plugin::ChannelId voice_channel_id) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "join_bot_voice", "join bot voice"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;
    if (bot->voice_channel_id == voice_channel_id)
        return true;
    if (!plugin->manager->services_.join_bot_voice) {
        LOG_WARN("Plugin '{}' tried to join bot voice, but bot voice is unavailable", plugin->id);
        return false;
    }

    plugin::ChannelId old_channel_id = bot->voice_channel_id;
    if (old_channel_id != 0) {
        if (plugin->manager->services_.leave_bot_voice)
            plugin->manager->services_.leave_bot_voice(bot->user_id, bot->display_name, old_channel_id);
        bot->voice_channel_id = 0;
    }

    if (!plugin->manager->services_.join_bot_voice(bot->user_id, bot->display_name, voice_channel_id))
        return false;

    bot->voice_channel_id = voice_channel_id;
    return true;
}

bool PluginManager::host_leave_bot_voice(void* context, plugin::BotHandle bot_handle) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "join_bot_voice", "leave bot voice"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;
    if (bot->voice_channel_id == 0)
        return true;
    if (!plugin->manager->services_.leave_bot_voice) {
        LOG_WARN("Plugin '{}' tried to leave bot voice, but bot voice is unavailable", plugin->id);
        return false;
    }

    plugin::ChannelId old_channel_id = bot->voice_channel_id;
    if (!plugin->manager->services_.leave_bot_voice(bot->user_id, bot->display_name, old_channel_id))
        return false;

    bot->voice_channel_id = 0;
    return true;
}

bool PluginManager::host_send_bot_voice_packet(void* context,
                                               plugin::BotHandle bot_handle,
                                               uint16_t sequence,
                                               const uint8_t* opus_payload,
                                               size_t opus_payload_len) {
    auto* plugin = static_cast<Plugin*>(context);
    if (!plugin || !plugin->manager) return false;
    if (!plugin->manager->check_host_permission(*plugin, "send_bot_audio", "send bot audio"))
        return false;

    Bot* bot = plugin->manager->get_owned_bot(*plugin, bot_handle);
    if (!bot) return false;
    if (bot->voice_channel_id == 0 || !opus_payload || opus_payload_len == 0)
        return false;
    if (!plugin->manager->services_.send_bot_voice_packet) {
        LOG_WARN("Plugin '{}' tried to send bot audio, but bot audio is unavailable", plugin->id);
        return false;
    }

    return plugin->manager->services_.send_bot_voice_packet(
        bot->user_id, bot->voice_channel_id, sequence, opus_payload, opus_payload_len);
}

bool PluginManager::register_chat_commands(Plugin& plugin,
                                           const plugin::CommandDefinition* commands,
                                           size_t command_count) {
    if (!check_host_permission(plugin, "create_chat_commands", "register chat commands"))
        return false;
    if (!commands && command_count != 0)
        return false;

    bool all_ok = true;
    for (size_t i = 0; i < command_count; ++i) {
        const auto& cmd = commands[i];
        if (!cmd.name || !cmd.description || !cmd.usage || !valid_command_name(cmd.name)) {
            LOG_WARN("Plugin '{}' provided invalid chat command at index {}", plugin.id, i);
            all_ok = false;
            continue;
        }
        auto duplicate = std::find_if(chat_commands_.begin(), chat_commands_.end(),
            [&](const ChatCommand& existing) { return existing.name == cmd.name; });
        if (duplicate != chat_commands_.end()) {
            LOG_WARN("Plugin '{}' tried to register duplicate chat command '{}'", plugin.id, cmd.name);
            all_ok = false;
            continue;
        }
        chat_commands_.push_back(ChatCommand{
            plugin.id,
            cmd.name,
            cmd.description,
            cmd.usage,
        });
        LOG_INFO("Plugin '{}' registered chat command /{}", plugin.id, cmd.name);
    }

    return all_ok;
}

PluginManager::Bot* PluginManager::get_owned_bot(Plugin& plugin, plugin::BotHandle bot_handle) const {
    if (!bot_handle) {
        LOG_WARN("Plugin '{}' passed a null bot handle", plugin.id);
        return nullptr;
    }

    auto* bot = static_cast<Bot*>(bot_handle);
    if (bot->owner != &plugin || bot->destroyed) {
        LOG_WARN("Plugin '{}' passed an invalid bot handle", plugin.id);
        return nullptr;
    }
    return bot;
}

bool PluginManager::check_host_permission(Plugin& plugin,
                                          std::string_view permission,
                                          std::string_view action) const {
    if (has_permission(plugin, permission))
        return true;

    LOG_WARN("Plugin '{}' tried to {} without '{}' permission",
             plugin.id, action, permission);
    return false;
}

bool PluginManager::has_permission(const Plugin& plugin, std::string_view permission) const {
    return plugin.permissions.find(std::string(permission)) != plugin.permissions.end();
}

PluginManager::Plugin* PluginManager::find_command_owner(std::string_view command_name) const {
    auto it = std::find_if(chat_commands_.begin(), chat_commands_.end(),
        [&](const ChatCommand& cmd) { return cmd.name == command_name; });
    if (it == chat_commands_.end())
        return nullptr;

    for (const auto& plugin : plugins_) {
        if (plugin->id == it->plugin_id)
            return plugin.get();
    }
    return nullptr;
}

} // namespace parties::server
