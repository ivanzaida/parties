#include <server/plugin_manager.h>

#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;
using namespace parties::server;

#define TEST_ASSERT(cond, msg) do {                               \
    if (!(cond)) {                                                \
        std::fprintf(stderr, "FAIL: %s (line %d)\n", msg, __LINE__); \
        return 1;                                                 \
    }                                                             \
} while(0)

static bool has_command(const PluginManager& plugins, const char* name) {
    for (const auto& command : plugins.chat_commands()) {
        if (command.name == name)
            return true;
    }
    return false;
}

int main() {
    auto tmp = fs::temp_directory_path() / "parties_plugin_policy_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp / "plugins");

    fs::path source = fs::path(PARTIES_TEST_PLUGIN_DIR) / "bot_echo";
    fs::path target = tmp / "plugins" / "bot_echo";
    TEST_ASSERT(fs::exists(source / "plugin.toml"), "source plugin manifest exists");
    fs::copy(source, target, fs::copy_options::recursive | fs::copy_options::overwrite_existing);

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        TEST_ASSERT(plugins.load(cfg), "load with no allow config");
        TEST_ASSERT(plugins.chat_commands().empty(), "default-deny skips plugin");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load explicitly allowed plugin");
        TEST_ASSERT(has_command(plugins, "botping"), "allowed plugin registers botping");
        TEST_ASSERT(has_command(plugins, "botjoin"), "allowed plugin registers botjoin");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            true,
            {"create_bot_users", "send_bot_chat"}
        });
        TEST_ASSERT(plugins.load(cfg), "load plugin without command permission");
        TEST_ASSERT(plugins.chat_commands().empty(), "missing create_chat_commands blocks init commands");
    }

    {
        PluginManager plugins;
        PluginConfig cfg;
        cfg.enabled = true;
        cfg.directory = (tmp / "plugins").string();
        cfg.allow.push_back(PluginConfig::Allow{
            "parties.example.bot_echo",
            false,
            {"create_chat_commands"}
        });
        TEST_ASSERT(plugins.load(cfg), "load disabled allow entry");
        TEST_ASSERT(plugins.chat_commands().empty(), "disabled allow entry skips plugin");
    }

    fs::remove_all(tmp);
    std::fprintf(stderr, "plugin_policy_test passed\n");
    return 0;
}
