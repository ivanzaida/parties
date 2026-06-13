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
};

PluginManager::PluginManager() = default;

PluginManager::~PluginManager() {
    shutdown();
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

    for (auto& perm : read_string_array(manifest, "permissions"))
        plugin->permissions.insert(std::move(perm));

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

bool PluginManager::register_chat_commands(Plugin& plugin,
                                           const plugin::CommandDefinition* commands,
                                           size_t command_count) {
    if (!has_permission(plugin, "create_chat_commands")) {
        LOG_WARN("Plugin '{}' tried to register chat commands without permission", plugin.id);
        return false;
    }
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
