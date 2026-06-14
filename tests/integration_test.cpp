// Integration test: Server + 2 headless clients + voice & screen share
//
// Voice flow:
//   1. Start server with fresh DB + 1 channel
//   2. Client A connects, authenticates via Ed25519 identity, joins channel
//   3. Client B connects, authenticates via Ed25519 identity, joins channel
//   4. Client A Opus-encodes PCM silence and sends as voice datagram
//   5. Client B receives the forwarded voice data
//   6. Verify: packet type, sender user_id, and opus payload match
//
// Screen share flow:
//   7.  Client A sends SCREEN_SHARE_START → both receive SCREEN_SHARE_STARTED
//   8.  Client B subscribes via SCREEN_SHARE_VIEW
//   9.  Client A sends synthetic video frame → Client B receives + verifies
//   10. Client B sends PLI → Client A receives forwarded PLI
//   11. Client A sends SCREEN_SHARE_STOP → both receive SCREEN_SHARE_STOPPED

#include <parties/crypto.h>
#include <parties/types.h>
#include <parties/net_common.h>
#include <parties/quic_common.h>
#include <parties/protocol.h>
#include <parties/serialization.h>
#include <parties/codec.h>
#include <parties/audio_common.h>
#include <parties/video_common.h>

#include <server/config.h>
#include <server/server.h>
#include <server/database.h>

#include <client/net_client.h>

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <dbghelp.h>
#endif

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <thread>
#include <vector>

namespace fs = std::filesystem;

using namespace parties;
using namespace parties::protocol;
using namespace parties::client;
using namespace parties::server;

static constexpr uint16_t TEST_PORT = 17800;
static constexpr int TIMEOUT_MS = 10000;

#define LOG(...) do { std::fprintf(stderr, __VA_ARGS__); std::fflush(stderr); } while(0)

// ── Helpers ──────────────────────────────────────────────────────────────

static bool wait_for_message(NetClient& client, ControlMessageType type,
                             std::vector<uint8_t>& payload, int timeout_ms = TIMEOUT_MS) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto msg = client.incoming().try_pop();
        if (msg) {
            if (msg->type == type) {
                payload = std::move(msg->payload);
                return true;
            }
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Wait for a message, collecting other messages into side_msgs
static bool wait_for_message_collecting(
    NetClient& client, ControlMessageType type,
    std::vector<uint8_t>& payload,
    std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>>& side_msgs,
    int timeout_ms = TIMEOUT_MS)
{
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);

    while (std::chrono::steady_clock::now() < deadline) {
        auto msg = client.incoming().try_pop();
        if (msg) {
            if (msg->type == type) {
                payload = std::move(msg->payload);
                return true;
            }
            side_msgs.emplace_back(msg->type, std::move(msg->payload));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// Find a message of given type in a collected side_msgs vector
[[maybe_unused]] static bool find_side_message(
    const std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>>& msgs,
    ControlMessageType type, std::vector<uint8_t>& payload)
{
    for (auto& [t, p] : msgs) {
        if (t == type) { payload = p; return true; }
    }
    return false;
}

static void drain_messages(NetClient& client) {
    while (auto msg = client.incoming().try_pop()) {}
}

#define TEST_ASSERT(cond, msg) do {                               \
    if (!(cond)) {                                                \
        LOG("FAIL: %s (line %d)\n", msg, __LINE__);              \
        return 1;                                                 \
    }                                                             \
} while(0)

static bool wait_for_connect(NetClient& client, int timeout_ms = TIMEOUT_MS) {
    auto deadline = std::chrono::steady_clock::now() +
                    std::chrono::milliseconds(timeout_ms);
    while (std::chrono::steady_clock::now() < deadline) {
        if (client.is_connected()) return true;
        if (client.connect_failed()) return false;
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
    return false;
}

// ── Crash handler ────────────────────────────────────────────────────────

#ifdef _WIN32
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    auto* rec = ep->ExceptionRecord;
    auto* ctx = ep->ContextRecord;

    // Get module info for faulting address
    HMODULE mod = nullptr;
    GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                       (LPCSTR)rec->ExceptionAddress, &mod);
    char mod_name[MAX_PATH] = "?";
    if (mod) GetModuleFileNameA(mod, mod_name, MAX_PATH);
    auto offset = (uintptr_t)rec->ExceptionAddress - (uintptr_t)mod;

    std::fprintf(stderr, "\n!!! CRASH: exception=0x%08lX\n", rec->ExceptionCode);
    std::fprintf(stderr, "    Faulting addr: %p (%s + 0x%llx)\n",
                 rec->ExceptionAddress, mod_name, (unsigned long long)offset);
    std::fprintf(stderr, "    RIP=%p RSP=%p\n", (void*)ctx->Rip, (void*)ctx->Rsp);
    if (rec->ExceptionCode == EXCEPTION_ACCESS_VIOLATION && rec->NumberParameters >= 2) {
        std::fprintf(stderr, "    Access violation %s address %p\n",
                     rec->ExceptionInformation[0] == 0 ? "reading" : "writing",
                     (void*)rec->ExceptionInformation[1]);
    }

    // Walk the stack manually using RBP chain
    std::fprintf(stderr, "    Stack trace (return addresses):\n");
    auto* rsp = reinterpret_cast<uintptr_t*>(ctx->Rsp);
    for (int i = 0; i < 20 && rsp; i++) {
        uintptr_t addr = *rsp++;
        HMODULE m = nullptr;
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, (LPCSTR)addr, &m) && m) {
            char name[MAX_PATH];
            GetModuleFileNameA(m, name, MAX_PATH);
            std::fprintf(stderr, "      [%d] %p (%s + 0x%llx)\n",
                         i, (void*)addr, name, (unsigned long long)(addr - (uintptr_t)m));
        }
    }

    std::fflush(stderr);
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

// ── Main ─────────────────────────────────────────────────────────────────

int main() {
#ifdef _WIN32
    SetUnhandledExceptionFilter(crash_handler);
#endif
    LOG("=== Parties Integration Test ===\n\n");

    // ── Init subsystems ──
    TEST_ASSERT(crypto_init(), "crypto_init");
    TEST_ASSERT(net_init(), "net_init");
    TEST_ASSERT(quic_init() != nullptr, "quic_init");
    LOG("[1/15] Subsystems initialized\n");

    // ── Create temp directory for test artifacts ──
    auto tmp = fs::temp_directory_path() / "parties_integration_test";
    fs::remove_all(tmp);
    fs::create_directories(tmp);

    std::string db_path   = (tmp / "test.db").string();
    std::string cert_path = (tmp / "server.der").string();
    std::string key_path  = (tmp / "server.key.der").string();

    // ── Configure and start server ──
    Config cfg;
    cfg.listen_ip      = "127.0.0.1";
    cfg.port           = TEST_PORT;
    cfg.cert_file      = cert_path;
    cfg.key_file       = key_path;
    cfg.db_path        = db_path;
    cfg.max_clients    = 8;
    cfg.plugins.enabled = true;
    cfg.plugins.directory = PARTIES_TEST_PLUGIN_DIR;
    cfg.plugins.allow.push_back(PluginConfig::Allow{
        "parties.example.bot_echo",
        true,
        {"create_chat_commands", "create_bot_users", "send_bot_chat",
         "join_bot_voice", "send_bot_audio"}
    });

    Server server;
    TEST_ASSERT(server.start(cfg), "server start");

    std::thread server_thread([&]() { server.run(); });
    std::this_thread::sleep_for(std::chrono::milliseconds(200));
    LOG("[2/15] Server started on port %u\n", TEST_PORT);

    NetClient client_a;
    NetClient client_b;

    std::mutex voice_mutex;
    std::vector<uint8_t> received_voice;
    std::atomic<bool> voice_received{false};

    auto cleanup = [&]() {
        client_a.disconnect();
        client_b.disconnect();
        server.stop();
        if (server_thread.joinable()) server_thread.join();
        quic_cleanup();
        net_cleanup();
        crypto_cleanup();
        fs::remove_all(tmp);
    };

    // ── Generate Ed25519 keypairs for both test clients ──
    SecretKey sk_a{}, sk_b{};
    PublicKey pk_a{}, pk_b{};
    TEST_ASSERT(derive_keypair("abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon abandon about", sk_a, pk_a),
                "derive keypair A");
    TEST_ASSERT(derive_keypair("zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo zoo wrong", sk_b, pk_b),
                "derive keypair B");

    // ── Client A: connect + auth identity ──
    LOG("[3/15] Client A connecting...\n");
    TEST_ASSERT(client_a.connect("127.0.0.1", TEST_PORT), "client A connect");
    TEST_ASSERT(wait_for_connect(client_a), "client A wait for connect");
    LOG("[3/15] Client A connected\n");

    uint32_t user_a_id = 0;
    uint32_t plugin_bot_user_id = 0;
    {
        auto now = static_cast<uint64_t>(std::time(nullptr));

        // Build message to sign: pubkey + display_name + timestamp
        BinaryWriter sign_buf;
        sign_buf.write_bytes(pk_a.data(), 32);
        sign_buf.write_string("test_user_a");
        sign_buf.write_u64(now);
        Signature sig_a{};
        TEST_ASSERT(ed25519_sign(sign_buf.data().data(), sign_buf.data().size(),
                    sk_a, pk_a, sig_a), "client A sign");

        // AUTH_IDENTITY: [version(2)][pubkey(32)][name][timestamp(8)][sig(64)][password]
        BinaryWriter w;
        w.write_u16(protocol::PROTOCOL_VERSION);
        w.write_bytes(pk_a.data(), 32);
        w.write_string("test_user_a");
        w.write_u64(now);
        w.write_bytes(sig_a.data(), 64);
        w.write_string("");  // no password

        TEST_ASSERT(client_a.send_message(ControlMessageType::AUTH_IDENTITY,
                    w.data().data(), w.data().size()), "client A send auth");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::AUTH_RESPONSE, payload),
                    "client A auth response");
        BinaryReader r(payload.data(), payload.size());
        user_a_id = r.read_u32();
        TEST_ASSERT(user_a_id > 0, "client A user_id > 0");

        std::vector<uint8_t> command_payload;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::CHAT_COMMAND_LIST, command_payload),
                    "client A chat command list");
        BinaryReader cr(command_payload.data(), command_payload.size());
        uint16_t command_count = cr.read_u16();
        bool found_botping = false;
        for (uint16_t i = 0; i < command_count; ++i) {
            std::string name = cr.read_string();
            std::string description = cr.read_string();
            std::string usage = cr.read_string();
            (void)description;
            (void)usage;
            if (name == "botping")
                found_botping = true;
        }
        TEST_ASSERT(!cr.error(), "client A command list parse");
        TEST_ASSERT(found_botping, "botping command advertised");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_a);
    }
    LOG("[3/15] Client A authenticated (user_id=%u)\n", user_a_id);

    // ── Client B: connect + register + auth ──
    LOG("[4/15] Client B connecting...\n");
    TEST_ASSERT(client_b.connect("127.0.0.1", TEST_PORT), "client B connect");
    TEST_ASSERT(wait_for_connect(client_b), "client B wait for connect");
    LOG("[4/15] Client B connected\n");

    client_b.on_data_received = [&](const uint8_t* data, size_t len) {
        LOG("[callback] Client B on_data_received: %zu bytes, first=0x%02x\n",
            len, len > 0 ? data[0] : 0);
        std::lock_guard<std::mutex> lock(voice_mutex);
        received_voice.assign(data, data + len);
        voice_received = true;
        LOG("[callback] Voice data stored\n");
    };

    uint32_t user_b_id = 0;
    {
        auto now = static_cast<uint64_t>(std::time(nullptr));

        BinaryWriter sign_buf;
        sign_buf.write_bytes(pk_b.data(), 32);
        sign_buf.write_string("test_user_b");
        sign_buf.write_u64(now);
        Signature sig_b{};
        TEST_ASSERT(ed25519_sign(sign_buf.data().data(), sign_buf.data().size(),
                    sk_b, pk_b, sig_b), "client B sign");

        // AUTH_IDENTITY: [version(2)][pubkey(32)][name][timestamp(8)][sig(64)][password]
        BinaryWriter w;
        w.write_u16(protocol::PROTOCOL_VERSION);
        w.write_bytes(pk_b.data(), 32);
        w.write_string("test_user_b");
        w.write_u64(now);
        w.write_bytes(sig_b.data(), 64);
        w.write_string("");  // no password

        TEST_ASSERT(client_b.send_message(ControlMessageType::AUTH_IDENTITY,
                    w.data().data(), w.data().size()), "client B send auth");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::AUTH_RESPONSE, payload),
                    "client B auth response");
        BinaryReader r(payload.data(), payload.size());
        user_b_id = r.read_u32();
        TEST_ASSERT(user_b_id > 0, "client B user_id > 0");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_b);
    }
    LOG("[4/15] Client B authenticated (user_id=%u)\n", user_b_id);

    // ── Plugin command: /botping should create/reuse a bot and send bot chat ──
    LOG("[plugin] Testing /botping command...\n");
    {
        BinaryWriter w;
        w.write_u32(1); // text channel id
        w.write_string("/botping hello from plugin test");
        w.write_u8(0); // attachments
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    w.data().data(), w.data().size()), "client A send /botping");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::CHAT_MESSAGE, payload),
                    "client A receives bot chat");

        BinaryReader r(payload.data(), payload.size());
        uint64_t msg_id = r.read_u64();
        uint32_t channel_id = r.read_u32();
        uint32_t sender_id = r.read_u32();
        std::string sender_name = r.read_string();
        uint64_t timestamp = r.read_u64();
        std::string text = r.read_string();
        uint8_t pinned = r.read_u8();
        uint8_t attachment_count = r.read_u8();

        TEST_ASSERT(!r.error(), "bot chat parse");
        TEST_ASSERT(msg_id > 0, "bot chat message id");
        TEST_ASSERT(channel_id == 1, "bot chat channel id");
        TEST_ASSERT(sender_id != 0 && sender_id != user_a_id && sender_id != user_b_id,
                    "bot chat sender id is bot");
        plugin_bot_user_id = sender_id;
        TEST_ASSERT(sender_name == "Bot Echo", "bot chat sender name");
        TEST_ASSERT(timestamp > 0, "bot chat timestamp");
        TEST_ASSERT(text == "hello from plugin test", "bot chat text");
        TEST_ASSERT(pinned == 0, "bot chat not pinned");
        TEST_ASSERT(attachment_count == 0, "bot chat no attachments");
    }
    LOG("[plugin] /botping produced bot chat\n");

    LOG("[plugin] Testing bot user reuse after handle reset...\n");
    {
        BinaryWriter reset;
        reset.write_u32(1);
        reset.write_string("/botreset");
        reset.write_u8(0);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    reset.data().data(), reset.data().size()), "client A send /botreset");
        std::this_thread::sleep_for(std::chrono::milliseconds(50));

        BinaryWriter ping;
        ping.write_u32(1);
        ping.write_string("/botping reused");
        ping.write_u8(0);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    ping.data().data(), ping.data().size()), "client A send second /botping");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::CHAT_MESSAGE, payload),
                    "client A receives reused bot chat");

        BinaryReader r(payload.data(), payload.size());
        (void)r.read_u64();
        TEST_ASSERT(r.read_u32() == 1, "reused bot chat channel id");
        uint32_t sender_id = r.read_u32();
        TEST_ASSERT(sender_id == plugin_bot_user_id, "bot user id reused");
        TEST_ASSERT(r.read_string() == "Bot Echo", "reused bot sender name");
        (void)r.read_u64();
        TEST_ASSERT(r.read_string() == "reused", "reused bot chat text");
        TEST_ASSERT(!r.error(), "reused bot chat parse");
    }
    LOG("[plugin] Bot user reuse verified (user_id=%u)\n", plugin_bot_user_id);

    // ── Both join channel 1 ──
    LOG("[5/15] Joining channel...\n");
    {
        BinaryWriter w;
        w.write_u32(1);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHANNEL_JOIN,
                    w.data().data(), w.data().size()), "client A send channel join");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_a, ControlMessageType::CHANNEL_USER_LIST,
                    payload, side), "client A channel user list");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_a);
    }
    LOG("[5/15] Client A joined channel 1\n");

    {
        BinaryWriter w;
        w.write_u32(1);
        TEST_ASSERT(client_b.send_message(ControlMessageType::CHANNEL_JOIN,
                    w.data().data(), w.data().size()), "client B send channel join");

        std::vector<uint8_t> payload;
        std::vector<std::pair<ControlMessageType, std::vector<uint8_t>>> side;
        TEST_ASSERT(wait_for_message_collecting(client_b, ControlMessageType::CHANNEL_USER_LIST,
                    payload, side), "client B channel user list");

        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        drain_messages(client_b);
    }
    LOG("[6/15] Client B joined channel 1\n");

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    drain_messages(client_a);

    LOG("[plugin] Testing bot voice join/audio/leave...\n");
    {
        BinaryWriter join;
        join.write_u32(1);
        join.write_string("/botjoin");
        join.write_u8(0);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    join.data().data(), join.data().size()), "client A send /botjoin");

        std::vector<uint8_t> payload;
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::USER_JOINED_CHANNEL, payload),
                    "client B receives bot joined channel");
        BinaryReader jr(payload.data(), payload.size());
        TEST_ASSERT(jr.read_u32() == plugin_bot_user_id, "bot join user id");
        TEST_ASSERT(jr.read_string() == "Bot Echo", "bot join display name");
        TEST_ASSERT(jr.read_u32() == 1, "bot join channel id");
        TEST_ASSERT(jr.read_u8() == static_cast<uint8_t>(Role::User), "bot join role");
        TEST_ASSERT(!jr.error(), "bot join parse");

        {
            std::lock_guard<std::mutex> lock(voice_mutex);
            received_voice.clear();
        }
        voice_received = false;

        BinaryWriter voice;
        voice.write_u32(1);
        voice.write_string("/botvoice");
        voice.write_u8(0);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    voice.data().data(), voice.data().size()), "client A send /botvoice");

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!voice_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }
        TEST_ASSERT(voice_received.load(), "client B received bot voice data");
        {
            std::lock_guard<std::mutex> lock(voice_mutex);
            TEST_ASSERT(received_voice.size() == 10, "bot voice packet size");
            TEST_ASSERT(received_voice[0] == VOICE_PACKET_TYPE, "bot voice packet type");
            uint32_t sender_id = 0;
            std::memcpy(&sender_id, received_voice.data() + 1, 4);
            TEST_ASSERT(sender_id == plugin_bot_user_id, "bot voice sender id");
            uint16_t seq = 0;
            std::memcpy(&seq, received_voice.data() + 5, 2);
            TEST_ASSERT(seq == 0, "bot voice sequence");
            TEST_ASSERT(received_voice[7] == 0xf8 &&
                        received_voice[8] == 0xff &&
                        received_voice[9] == 0xfe, "bot voice payload");
            received_voice.clear();
        }
        voice_received = false;

        BinaryWriter leave;
        leave.write_u32(1);
        leave.write_string("/botleave");
        leave.write_u8(0);
        TEST_ASSERT(client_a.send_message(ControlMessageType::CHAT_SEND,
                    leave.data().data(), leave.data().size()), "client A send /botleave");

        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::USER_LEFT_CHANNEL, payload),
                    "client B receives bot left channel");
        BinaryReader lr(payload.data(), payload.size());
        TEST_ASSERT(lr.read_u32() == plugin_bot_user_id, "bot leave user id");
        TEST_ASSERT(lr.read_u32() == 1, "bot leave channel id");
        TEST_ASSERT(!lr.error(), "bot leave parse");
    }
    LOG("[plugin] Bot voice join/audio/leave verified\n");

    // ── Client A: encode PCM silence and send as voice ──
    LOG("[7/15] Sending voice data...\n");
    std::vector<uint8_t> original_opus;
    {
        OpusCodec codec;
        TEST_ASSERT(codec.init_encoder(audio::SAMPLE_RATE, audio::CHANNELS,
                    audio::OPUS_BITRATE), "opus encoder init");

        float pcm[audio::OPUS_FRAME_SIZE] = {};
        uint8_t opus_buf[audio::MAX_OPUS_PACKET];
        int opus_len = codec.encode(pcm, audio::OPUS_FRAME_SIZE,
                                    opus_buf, audio::MAX_OPUS_PACKET);
        TEST_ASSERT(opus_len > 0, "opus encode");

        original_opus.assign(opus_buf, opus_buf + opus_len);

        std::vector<uint8_t> voice_pkt;
        voice_pkt.push_back(VOICE_PACKET_TYPE);
        uint16_t seq = 0;
        voice_pkt.insert(voice_pkt.end(), reinterpret_cast<uint8_t*>(&seq),
                         reinterpret_cast<uint8_t*>(&seq) + 2);
        voice_pkt.insert(voice_pkt.end(), opus_buf, opus_buf + opus_len);

        TEST_ASSERT(client_a.send_data(voice_pkt.data(), voice_pkt.size()),
                    "client A send voice");
    }
    LOG("[7/15] Client A sent %zu bytes of Opus data\n", original_opus.size());

    // ── Wait for Client B to receive forwarded voice ──
    LOG("[8/15] Waiting for Client B to receive voice...\n");
    {
        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!voice_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(voice_received.load(), "client B received voice data");

        std::lock_guard<std::mutex> lock(voice_mutex);

        TEST_ASSERT(received_voice.size() >= 7, "voice packet minimum size");
        TEST_ASSERT(received_voice[0] == VOICE_PACKET_TYPE, "voice packet type byte");

        uint32_t sender_id;
        std::memcpy(&sender_id, received_voice.data() + 1, 4);
        TEST_ASSERT(sender_id == user_a_id,
                    "voice packet sender_id matches client A");

        uint16_t recv_seq;
        std::memcpy(&recv_seq, received_voice.data() + 5, 2);
        TEST_ASSERT(recv_seq == 0, "voice packet sequence number");

        size_t opus_len = received_voice.size() - 7;
        TEST_ASSERT(opus_len == original_opus.size(), "opus payload length matches");
        TEST_ASSERT(std::memcmp(received_voice.data() + 7, original_opus.data(),
                    opus_len) == 0, "opus payload content matches");

        LOG("[8/15] Voice verified: sender=%u, opus=%zu bytes\n", sender_id, opus_len);
    }

    // ── Verify Opus decode ──
    {
        OpusCodec decoder;
        TEST_ASSERT(decoder.init_decoder(audio::SAMPLE_RATE, audio::CHANNELS),
                    "opus decoder init");

        float pcm_out[audio::OPUS_FRAME_SIZE];
        int decoded = decoder.decode(original_opus.data(),
                                     static_cast<int>(original_opus.size()),
                                     pcm_out, audio::OPUS_FRAME_SIZE);
        TEST_ASSERT(decoded == audio::OPUS_FRAME_SIZE,
                    "opus decode produces correct frame size");
    }
    LOG("[9/15] Opus decode OK\n");

    // ══════════════════════════════════════════════════════════════════════
    // SCREEN SHARING TESTS
    // ══════════════════════════════════════════════════════════════════════

    // ── Client A: start screen share ──
    LOG("[10/15] Screen share start...\n");
    {
        BinaryWriter w;
        w.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
        w.write_u16(1920);
        w.write_u16(1080);
        TEST_ASSERT(client_a.send_message(ControlMessageType::SCREEN_SHARE_START,
                    w.data().data(), w.data().size()), "client A send share start");

        // Both clients should receive SCREEN_SHARE_STARTED
        std::vector<uint8_t> payload_a, payload_b;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::SCREEN_SHARE_STARTED, payload_a),
                    "client A receive SCREEN_SHARE_STARTED");
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::SCREEN_SHARE_STARTED, payload_b),
                    "client B receive SCREEN_SHARE_STARTED");

        // Verify payload: [user_id(4)][codec(1)][width(2)][height(2)]
        BinaryReader ra(payload_a.data(), payload_a.size());
        TEST_ASSERT(ra.read_u32() == user_a_id, "share started user_id (A)");
        TEST_ASSERT(ra.read_u8() == static_cast<uint8_t>(VideoCodecId::AV1), "share started codec (A)");
        TEST_ASSERT(ra.read_u16() == 1920, "share started width (A)");
        TEST_ASSERT(ra.read_u16() == 1080, "share started height (A)");

        BinaryReader rb(payload_b.data(), payload_b.size());
        TEST_ASSERT(rb.read_u32() == user_a_id, "share started user_id (B)");

        drain_messages(client_a);
        drain_messages(client_b);
    }
    LOG("[10/15] Screen share started, both clients notified\n");

    // ── Client B: subscribe to Client A's share ──
    LOG("[11/15] Client B subscribing to screen share...\n");
    {
        BinaryWriter w;
        w.write_u32(user_a_id);
        TEST_ASSERT(client_b.send_message(ControlMessageType::SCREEN_SHARE_VIEW,
                    w.data().data(), w.data().size()), "client B send share view");
        // Give server time to process subscription
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    LOG("[11/15] Client B subscribed\n");

    // ── Client A: send synthetic video frame (plaintext, QUIC TLS encrypts in transit) ──
    LOG("[12/15] Sending video frame...\n");
    std::vector<uint8_t> original_video_payload;
    {
        // Build synthetic video header + data:
        // [frame_number(4)][timestamp(4)][flags(1)][width(2)][height(2)][codec_id(1)][data(N)]
        BinaryWriter vw;
        vw.write_u32(1);       // frame_number
        vw.write_u32(0);       // timestamp
        vw.write_u8(VIDEO_FLAG_KEYFRAME);
        vw.write_u16(1920);
        vw.write_u16(1080);
        vw.write_u8(static_cast<uint8_t>(VideoCodecId::AV1));
        // Fake AV1 payload (32 bytes of pattern data)
        for (uint8_t i = 0; i < 32; i++) vw.write_u8(0xAA ^ i);
        original_video_payload = vw.data();

        // Wire packet: [type(1)][payload(N)]
        std::vector<uint8_t> pkt;
        pkt.push_back(VIDEO_FRAME_PACKET_TYPE);
        pkt.insert(pkt.end(), original_video_payload.begin(), original_video_payload.end());

        TEST_ASSERT(client_a.send_video(pkt.data(), pkt.size()),
                    "client A send video frame");
    }
    LOG("[12/15] Client A sent video frame (%zu bytes)\n",
        original_video_payload.size());

    // ── Client B: receive and verify video frame ──
    LOG("[13/15] Waiting for Client B to receive video frame...\n");
    {
        // Reset the datagram callback for video
        std::mutex video_mutex;
        std::vector<uint8_t> received_video;
        std::atomic<bool> video_received{false};

        client_b.on_data_received = [&](const uint8_t* data, size_t len) {
            if (len > 0 && data[0] == VIDEO_FRAME_PACKET_TYPE) {
                std::lock_guard<std::mutex> lock(video_mutex);
                received_video.assign(data, data + len);
                video_received = true;
            }
        };

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!video_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(video_received.load(), "client B received video frame");

        std::lock_guard<std::mutex> lock(video_mutex);

        // Expected: [type(1)][sender_id(4)][payload(N)]
        TEST_ASSERT(received_video.size() >= 19, "video packet minimum size");
        TEST_ASSERT(received_video[0] == VIDEO_FRAME_PACKET_TYPE, "video packet type byte");

        uint32_t sender_id;
        std::memcpy(&sender_id, received_video.data() + 1, 4);
        TEST_ASSERT(sender_id == user_a_id, "video sender_id matches client A");

        // Verify payload matches original (after sender_id)
        const uint8_t* payload = received_video.data() + 5;
        size_t payload_len = received_video.size() - 5;

        TEST_ASSERT(payload_len == original_video_payload.size(),
                    "video payload length matches");
        TEST_ASSERT(std::memcmp(payload, original_video_payload.data(),
                    payload_len) == 0, "video payload content matches");

        LOG("[13/15] Video frame verified: sender=%u, %zu bytes OK\n",
            sender_id, payload_len);
    }

    // ── PLI test: Client B requests keyframe from Client A ──
    LOG("[14/15] Testing PLI forwarding...\n");
    {
        std::mutex pli_mutex;
        std::vector<uint8_t> received_pli;
        std::atomic<bool> pli_received{false};

        client_a.on_data_received = [&](const uint8_t* data, size_t len) {
            if (len >= 6 && data[0] == VIDEO_CONTROL_TYPE && data[1] == VIDEO_CTL_PLI) {
                std::lock_guard<std::mutex> lock(pli_mutex);
                received_pli.assign(data, data + len);
                pli_received = true;
            }
        };

        // Client B sends PLI: [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][target_user_id(4)]
        std::vector<uint8_t> pli_pkt(6);
        pli_pkt[0] = VIDEO_CONTROL_TYPE;
        pli_pkt[1] = VIDEO_CTL_PLI;
        std::memcpy(pli_pkt.data() + 2, &user_a_id, 4);
        TEST_ASSERT(client_b.send_data(pli_pkt.data(), pli_pkt.size()),
                    "client B send PLI");

        auto deadline = std::chrono::steady_clock::now() +
                        std::chrono::milliseconds(TIMEOUT_MS);
        while (!pli_received && std::chrono::steady_clock::now() < deadline) {
            std::this_thread::sleep_for(std::chrono::milliseconds(5));
        }

        TEST_ASSERT(pli_received.load(), "client A received forwarded PLI");

        std::lock_guard<std::mutex> lock(pli_mutex);
        // Server forwards: [VIDEO_CONTROL_TYPE][VIDEO_CTL_PLI][requester_user_id(4)]
        TEST_ASSERT(received_pli.size() >= 6, "PLI packet size");
        TEST_ASSERT(received_pli[0] == VIDEO_CONTROL_TYPE, "PLI type byte");
        TEST_ASSERT(received_pli[1] == VIDEO_CTL_PLI, "PLI subtype byte");
        uint32_t requester_id;
        std::memcpy(&requester_id, received_pli.data() + 2, 4);
        TEST_ASSERT(requester_id == user_b_id, "PLI requester_id matches client B");

        LOG("[14/15] PLI verified: requester=%u\n", requester_id);
    }

    // ── Client A: stop screen share ──
    LOG("[15/15] Screen share stop...\n");
    {
        TEST_ASSERT(client_a.send_message(ControlMessageType::SCREEN_SHARE_STOP,
                    nullptr, 0), "client A send share stop");

        std::vector<uint8_t> payload_a, payload_b;
        TEST_ASSERT(wait_for_message(client_a, ControlMessageType::SCREEN_SHARE_STOPPED, payload_a),
                    "client A receive SCREEN_SHARE_STOPPED");
        TEST_ASSERT(wait_for_message(client_b, ControlMessageType::SCREEN_SHARE_STOPPED, payload_b),
                    "client B receive SCREEN_SHARE_STOPPED");

        BinaryReader ra(payload_a.data(), payload_a.size());
        TEST_ASSERT(ra.read_u32() == user_a_id, "share stopped user_id (A)");
        BinaryReader rb(payload_b.data(), payload_b.size());
        TEST_ASSERT(rb.read_u32() == user_a_id, "share stopped user_id (B)");
    }
    LOG("[15/15] Screen share stopped, both clients notified\n");

    LOG("\n=== ALL TESTS PASSED ===\n");
    cleanup();
    return 0;
}
