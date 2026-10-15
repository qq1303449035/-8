// Real production dispatch and forwarding, with an offline TLS sink only.
#include "../network/protocol.h"
#include "../sessions.h"
#include <iostream>
#include <stdexcept>

namespace chh::tls {
struct Stream {
    bool writable = true;
    std::vector<wire::Packet> sent;
    bool write_all(const void* bytes, size_t size, int) {
        if (!writable || size != wire::packet_size) return false;
        wire::Packet packet;
        std::memcpy(packet.bytes.data(), bytes, size);
        sent.push_back(packet);
        return true;
    }
};
}
namespace {
using namespace chh::wire;
using Sessions = chh::server::Sessions;
using Role = chh::server::ClientRole;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "selection_shout_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_selection_shout_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "selection_shout_handler.inc"
        }
    }
    void poll() {
        do {
#include "selection_shout_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, Role role, std::string account = "owner", uint32_t ip = 10) {
    const auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters account/IP pair");
    return result;
}
Packet request(uint32_t enabled, std::string_view text) {
    Packet packet;
    require(make_game_selection_shout_config(enabled, text, packet), "fixture encodes configuration");
    return packet;
}
Packet raw_request(uint32_t enabled, std::string_view text) {
    Packet packet(Type::game_selection_shout_set);
    require(packet.add_number(enabled), "raw enabled");
    const uint8_t length[2]{uint8_t(text.size() >> 8), uint8_t(text.size())};
    require(packet.add_bytes(length, 2) && packet.add_bytes(reinterpret_cast<const uint8_t*>(text.data()), text.size()),
        "raw malformed text fixture");
    return packet;
}
void frame(const Packet& packet, uint32_t enabled, std::string_view text) {
    uint32_t value = UINT32_MAX;
    std::string decoded;
    require(packet.valid() && packet.type() == Type::game_selection_shout_set &&
        packet.length() == 6 + text.size() && read_game_selection_shout_config(packet, value, decoded) &&
        value == enabled && decoded == text, "exact switch and UTF-8 text forwarded");
}
void protocol_error(const Worker& worker) {
    require(worker.stream.sent.size() == 1 && worker.stream.sent[0].type() == Type::error &&
        worker.stream.sent[0].length() == 4 && worker.stream.sent[0].number() == uint32_t(Error::protocol),
        "malformed request generates one protocol error");
}
void routing_and_reconnect() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, Role::primary));
    require(sessions.set_game_selection_shout(ui.member.binding, 1, u8"你好，选人中"), "queue before game connects");
    Worker game(sessions, enter(sessions, Role::secondary));
    Worker other_ip(sessions, enter(sessions, Role::secondary, "owner", 20));
    Worker other_account(sessions, enter(sessions, Role::secondary, "other"));
    require(sessions.game_snapshot(ui.member.binding).state == 0, "fixture is before in-game status");
    game.poll(); other_ip.poll(); other_account.poll(); ui.poll();
    require(game.stream.sent.size() == 1 && other_ip.stream.sent.empty() && other_account.stream.sent.empty() &&
        ui.stream.sent.empty(), "configuration routes to the paired game before state four only");
    frame(game.stream.sent[0], 1, u8"你好，选人中");
    const auto first = sessions.selection_shout_command_snapshot(game.member.binding);
    ui.receive({request(1, u8"你好，选人中")}); game.poll();
    require(game.stream.sent.size() == 1 && sessions.selection_shout_command_snapshot(game.member.binding).revision == first.revision,
        "identical configuration is deduplicated");
    ui.receive({request(1, u8"已更新")});
    game.stream.writable = false; game.poll();
    require(game.sent_selection_shout_command_revision == first.revision, "failed forwarding retains pending revision");
    game.stream.writable = true; game.poll(); frame(game.stream.sent.back(), 1, u8"已更新");
    require(sessions.set_game_instant_pick(ui.member.binding, 1) && sessions.set_game_hero_select(ui.member.binding, 339),
        "initialize separate selection controls");
    for (const uint32_t state : {0u, 1u, 4u}) {
        require(sessions.update_game(game.member.binding, state ? 42 : 0, state, "Player"), "update game state");
        ui.receive({request(state == 1 ? 0 : 1, "state " + std::to_string(state))}); game.poll();
        frame(game.stream.sent.back(), state == 1 ? 0 : 1, "state " + std::to_string(state));
    }
    require(sessions.instant_pick_command_snapshot(game.member.binding).enabled == 1 &&
        sessions.hero_select_command_snapshot(game.member.binding).hero_id == 339, "shout configuration preserves other selection controls");
    sessions.leave(game.member.binding);
    ui.receive({request(0, "draft"), request(1, u8"重连内容")});
    Worker reconnect(sessions, enter(sessions, Role::secondary));
    reconnect.poll(); reconnect.poll();
    require(reconnect.stream.sent.size() == 1, "reconnect replays only latest configuration once");
    frame(reconnect.stream.sent[0], 1, u8"重连内容");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty() && !sessions.selection_shout_command_snapshot(game.member.binding).initialized,
        "stale game binding loses read access");
    sessions.leave(ui.member.binding);
    Worker new_ui(sessions, enter(sessions, Role::primary));
    ui.receive({request(0, "stale")}); protocol_error(ui);
    new_ui.receive({request(0, u8"保留文本")}); reconnect.poll(); frame(reconnect.stream.sent.back(), 0, u8"保留文本");
    new_ui.receive({request(1, "")}); reconnect.poll(); frame(reconnect.stream.sent.back(), 1, "");
    sessions.leave(new_ui.member.binding); sessions.leave(reconnect.member.binding);
    Worker fresh(sessions, enter(sessions, Role::secondary));
    const auto defaults = sessions.selection_shout_command_snapshot(fresh.member.binding);
    require(!defaults.initialized && defaults.enabled == 0 && defaults.text.empty(), "fully discarded pair returns to off/empty defaults");
}
void text_boundaries_and_atomic_rejection() {
    static_assert(uint32_t(Type::game_selection_shout_set) == 165 && game_selection_shout_text_limit == 177);
    require(Packet(Type::game_selection_shout_set).valid() && !Packet(static_cast<Type>(182)).valid(), "type boundary updated");
    Sessions sessions;
    Worker ui(sessions, enter(sessions, Role::primary));
    Worker game(sessions, enter(sessions, Role::secondary));
    std::string chinese;
    for (unsigned i = 0; i < 59; ++i) chinese += u8"好";
    for (uint32_t enabled : {0u, 1u}) for (const auto& text : std::vector<std::string>{"", " ", u8"你好 😀", std::string(177, 'a'), chinese}) {
        const auto packet = request(enabled, text);
        ui.receive({packet}); game.poll(); frame(game.stream.sent.back(), enabled, text);
    }
    std::vector<Packet> invalid{raw_request(2, "ok"), raw_request(UINT32_MAX, "ok")};
    for (const auto& text : std::vector<std::string>{std::string(178, 'a'), chinese + "a", "line\nbreak", "\t", "\r", "\x7f",
        std::string("a\0b", 3), std::string("\xc0\xaf", 2), std::string("\xed\xa0\x80", 3), std::string("\xf4\x90\x80\x80", 4),
        std::string("\xe5\xa5", 2), std::string("\x80", 1)}) {
        for (uint32_t enabled : {0u, 1u}) {
            require(!valid_game_selection_shout_config(enabled, text) &&
                !sessions.set_game_selection_shout(ui.member.binding, enabled, text), "invalid text rejected even when disabled");
            Packet packet;
            require(!make_game_selection_shout_config(enabled, text, packet), "invalid text cannot serialize");
            invalid.push_back(raw_request(enabled, text));
        }
    }
    const auto valid = request(1, "abc");
    for (size_t length = 0; length <= valid.length() + 3; ++length) {
        if (length == valid.length()) continue;
        Packet packet(Type::game_selection_shout_set);
        require(packet.add_bytes(valid.bytes.data() + 8, length), "malformed length fixture");
        invalid.push_back(packet);
    }
    for (const uint16_t length : {uint16_t(0), uint16_t(2), uint16_t(4), uint16_t(178), uint16_t(UINT16_MAX)}) {
        auto packet = valid; packet.bytes[12] = uint8_t(length >> 8); packet.bytes[13] = uint8_t(length);
        invalid.push_back(packet);
    }
    ui.receive({request(1, u8"保留这个")});
    const auto saved = sessions.selection_shout_command_snapshot(game.member.binding);
    for (const auto& packet : invalid) {
        uint32_t decoded_enabled = 9; std::string decoded_text = "unchanged";
        require(!read_game_selection_shout_config(packet, decoded_enabled, decoded_text), "malformed helper decode rejected");
        ui.stream.sent.clear(); ui.receive({packet, request(0, "later")}); protocol_error(ui);
        const auto after = sessions.selection_shout_command_snapshot(game.member.binding);
        require(after.revision == saved.revision && after.enabled == saved.enabled && after.text == saved.text,
            "malformed packet atomically preserves switch/text and stops later frame");
    }
    game.stream.sent.clear(); game.poll(); frame(game.stream.sent.back(), 1, u8"保留这个");
}
void permission_checks() {
    Sessions sessions;
    Worker game(sessions, enter(sessions, Role::secondary));
    game.receive({request(1, "not authorized")}); protocol_error(game);
    Worker ui(sessions, enter(sessions, Role::primary));
    require(!sessions.set_game_selection_shout({}, 1, "x") &&
        !sessions.set_game_selection_shout(game.member.binding, 1, "x") &&
        !sessions.set_game_selection_shout(ui.member.binding, 2, "x"), "session API checks binding, role and boolean");
    ui.receive({request(0, "")});
    const auto saved = sessions.selection_shout_command_snapshot(game.member.binding);
    require(saved.initialized && saved.enabled == 0 && saved.text.empty(), "first default initializes configuration");
    require(!sessions.selection_shout_command_snapshot(ui.member.binding).initialized, "primary cannot read secondary command snapshot");
    require(sessions.invalidate_account("owner"), "account invalidation succeeds");
    require(!sessions.set_game_selection_shout(ui.member.binding, 1, "x") &&
        !sessions.selection_shout_command_snapshot(game.member.binding).initialized, "invalidation removes stale control access");
}
}
int main() {
    try {
        routing_and_reconnect(); text_boundaries_and_atomic_rejection(); permission_checks();
        std::cout << "PASS: " << checks << " selection-shout server checks (offline, no chat messages)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
