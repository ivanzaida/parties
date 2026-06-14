#include <parties/plugin_api.h>

#include <cstring>

#ifdef _WIN32
#define PARTIES_PLUGIN_EXPORT extern "C" __declspec(dllexport)
#else
#define PARTIES_PLUGIN_EXPORT extern "C" __attribute__((visibility("default")))
#endif

namespace {

parties::plugin::Host g_host{};
parties::plugin::BotHandle g_bot = nullptr;
parties::plugin::UserId g_bot_user_id = 0;

void log_info(const char* message) {
    if (g_host.log)
        g_host.log(g_host.context, static_cast<uint8_t>(parties::plugin::LogLevel::Info), message);
}

bool ensure_bot() {
    if (g_bot)
        return true;
    if (!g_host.create_bot_user)
        return false;
    return g_host.create_bot_user(g_host.context, "default", "Bot Echo", &g_bot, &g_bot_user_id);
}

void on_chat_command(const parties::plugin::ChatCommandInvocation* invocation) {
    if (!invocation || !invocation->command_name)
        return;
    if (std::strcmp(invocation->command_name, "botping") == 0) {
        if (!ensure_bot() || !g_host.send_bot_chat)
            return;

        const char* text = invocation->args && invocation->args[0]
            ? invocation->args
            : "pong";
        parties::plugin::MessageId message_id = 0;
        g_host.send_bot_chat(g_host.context, g_bot, invocation->text_channel_id, text, &message_id);
        return;
    }

    if (std::strcmp(invocation->command_name, "botreset") == 0) {
        if (g_host.destroy_bot_user && g_bot)
            g_host.destroy_bot_user(g_host.context, g_bot);
        g_bot = nullptr;
        g_bot_user_id = 0;
        return;
    }

    if (std::strcmp(invocation->command_name, "botjoin") == 0) {
        if (!ensure_bot() || !g_host.join_bot_voice)
            return;
        g_host.join_bot_voice(g_host.context, g_bot, 1);
        return;
    }

    if (std::strcmp(invocation->command_name, "botleave") == 0) {
        if (g_bot && g_host.leave_bot_voice)
            g_host.leave_bot_voice(g_host.context, g_bot);
        return;
    }

    if (std::strcmp(invocation->command_name, "botvoice") == 0) {
        if (!ensure_bot() || !g_host.send_bot_voice_packet)
            return;
        static uint16_t sequence = 0;
        const uint8_t opus_payload[] = {0xf8, 0xff, 0xfe};
        g_host.send_bot_voice_packet(g_host.context, g_bot, sequence++,
                                     opus_payload, sizeof(opus_payload));
        return;
    }
}

} // namespace

PARTIES_PLUGIN_EXPORT bool parties_plugin_init(const parties::plugin::Host* host,
                                               parties::plugin::Registration* registration) {
    if (!host || !registration || !host->create_chat_commands)
        return false;

    g_host = *host;

    parties::plugin::CommandDefinition commands[5]{};
    commands[0].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[0].name = "botping";
    commands[0].description = "Send a test message as a server bot.";
    commands[0].usage = "/botping [text]";
    commands[1].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[1].name = "botreset";
    commands[1].description = "Drop the runtime bot handle.";
    commands[1].usage = "/botreset";
    commands[2].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[2].name = "botjoin";
    commands[2].description = "Join the test bot to voice channel 1.";
    commands[2].usage = "/botjoin";
    commands[3].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[3].name = "botleave";
    commands[3].description = "Leave the test bot voice channel.";
    commands[3].usage = "/botleave";
    commands[4].abi = parties::plugin::make_abi_header<parties::plugin::CommandDefinition>();
    commands[4].name = "botvoice";
    commands[4].description = "Send one test Opus packet from the bot.";
    commands[4].usage = "/botvoice";

    if (!g_host.create_chat_commands(g_host.context, commands, 5))
        return false;

    registration->abi = parties::plugin::make_abi_header<parties::plugin::Registration>();
    registration->on_chat_command = &on_chat_command;

    log_info("bot_echo initialized");
    return true;
}

PARTIES_PLUGIN_EXPORT void parties_plugin_shutdown() {
    if (g_host.destroy_bot_user && g_bot)
        g_host.destroy_bot_user(g_host.context, g_bot);
    g_bot = nullptr;
    g_bot_user_id = 0;
    log_info("bot_echo shutdown");
}
