// The launcher extracts the real dispatch and forwarding branches. Only TLS
// writes are replaced; these tests do not start a server or connect to a DB.
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
using Sessions = chh::server::Sessions;
using Role = chh::server::ClientRole;
using Type = chh::wire::Type;
using Packet = chh::wire::Packet;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "skill_hint_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_skill_hint_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "skill_hint_handler.inc"
        }
    }
    void poll() {
        do {
#include "skill_hint_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled, uint32_t ally = 1, uint32_t enemy = 1, uint32_t hero_skills = 0, bool extended = false) {
    Packet packet(Type::game_skill_hint_set);
    require(packet.add_number(enabled) && packet.add_number(ally) && packet.add_number(enemy),
        "fixture encodes all three flags");
    if (hero_skills || extended) require(packet.add_number(hero_skills), "fixture appends optional hero skills flag");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value, uint32_t ally = 1, uint32_t enemy = 1, uint32_t hero_skills = 0) {
    const bool control = type == Type::game_skill_hint_set;
    require(packet.valid() && packet.type() == type && packet.length() == (control ? (hero_skills ? 16u : 12u) : 4u), "valid response frame length");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX, decoded_ally = UINT32_MAX, decoded_enemy = UINT32_MAX;
    require(reader.number(decoded) && decoded == value, "response contains expected first value");
    if (control) require(reader.number(decoded_ally) && reader.number(decoded_enemy) &&
        decoded_ally == ally && decoded_enemy == enemy, "response preserves ally and enemy flags");
    if (control && hero_skills) require(reader.number(decoded) && decoded == hero_skills, "response includes enabled hero skills");
    require(reader.done(), "response contains no trailing fields");
}
void error(const Worker& worker) {
    require(worker.stream.sent.size() == 1, "invalid request produces one protocol error");
    frame(worker.stream.sent[0], Type::error, uint32_t(chh::wire::Error::protocol));
}
void routing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    game.poll();
    require(game.stream.sent.empty(), "no unsolicited command before first initialization");
    ui.receive({request(0)});
    const auto initial = sessions.skill_hint_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0, "first off value initializes command");
    require(!sessions.skill_hint_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_skill_hint_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.skill_hint_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1, 1, 1, 1)});
    const auto enabled = sessions.skill_hint_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.hero_skills == 1 && enabled.revision > initial.revision, "enabling advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_skill_hint_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_skill_hint_set, 1, 1, 1, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate skill-hint commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.skill_hint_command_snapshot(own_game).revision == enabled.revision, "status changes preserve skill-hint settings");

    sessions.leave(own_game);
    ui.receive({request(0), request(1, 1, 1, 1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_skill_hint_set, 1, 1, 1, 1);
    require(!sessions.skill_hint_command_snapshot(own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({request(0)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({request(0)});
    reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(reconnected.stream.sent.back(), Type::game_skill_hint_set, 0);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.skill_hint_command_snapshot(fresh).initialized, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_skill_hint(primary, 1, 1, 1, 1), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.skill_hint_command_snapshot(fresh).initialized &&
        !sessions.set_game_skill_hint(primary, 0, 1, 1), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.skill_hint_command_snapshot(fresh_game).initialized, "new account session inherits no revoked setting");
}
void all_switch_combinations() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    for (uint32_t bits = 0; bits < 16; ++bits) {
        const uint32_t enabled = (bits >> 3) & 1, ally = (bits >> 2) & 1, enemy = (bits >> 1) & 1, hero_skills = bits & 1;
        ui.receive({request(enabled, ally, enemy, hero_skills, true)});
        game.poll();
        require(game.stream.sent.size() == bits + 1, "each changed complete card is forwarded once");
        frame(game.stream.sent.back(), Type::game_skill_hint_set, enabled, ally, enemy, hero_skills);
        const auto snapshot = sessions.skill_hint_command_snapshot(game.member.binding);
        require(snapshot.enabled == enabled && snapshot.ally == ally && snapshot.enemy == enemy && snapshot.hero_skills == hero_skills,
            "session applies each complete card atomically");
        ui.receive({request(enabled, ally, enemy, hero_skills, true)});
        game.poll();
        require(game.stream.sent.size() == bits + 1, "duplicate card does not resend");
    }
    require(ui.stream.sent.empty(), "all sixteen control combinations accepted");
}
void legacy_compatibility() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    require(sessions.skill_hint_command_snapshot(game.member.binding).hero_skills == 0, "hero skills default off");
    ui.receive({request(1, 1, 0)});
    game.poll();
    const auto legacy = sessions.skill_hint_command_snapshot(game.member.binding);
    require(legacy.initialized && legacy.hero_skills == 0, "legacy request defaults hero skills to zero");
    frame(game.stream.sent.back(), Type::game_skill_hint_set, 1, 1, 0);
    ui.receive({request(1, 1, 0, 0, true)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.skill_hint_command_snapshot(game.member.binding).revision == legacy.revision,
        "explicit zero extension is equivalent to legacy request");
    ui.receive({request(1, 1, 0, 1)});
    game.poll();
    frame(game.stream.sent.back(), Type::game_skill_hint_set, 1, 1, 0, 1);
    ui.receive({request(1, 1, 0)});
    game.poll();
    require(game.stream.sent.size() == 3 && sessions.skill_hint_command_snapshot(game.member.binding).hero_skills == 0,
        "legacy request clears an enabled extension and forwards the change");
    frame(game.stream.sent.back(), Type::game_skill_hint_set, 1, 1, 0);
    require(ui.stream.sent.empty(), "both frame formats are accepted without errors");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_skill_hint_set) == 119);
    require(request(0).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "feature packet type is supported and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX), request(0, 2, 0), request(0, UINT32_MAX, 0),
        request(0, 0, 2), request(0, 0, UINT32_MAX), request(0, 0, 0, 2), request(0, 0, 0, UINT32_MAX)};
    for (size_t length = 0; length <= 20; ++length) {
        if (length == 12 || length == 16) continue;
        Packet packet(Type::game_skill_hint_set);
        const uint8_t bytes[20]{};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(1, 1, 1, 1)});
        const auto revision = sessions.skill_hint_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.skill_hint_command_snapshot(game);
        require(command.enabled == 1 && command.hero_skills == 1 && command.revision == revision, "malformed request keeps setting and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1, 1, 1, 1)});
    error(endpoint);
    require(!sessions.skill_hint_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_skill_hint({}, 1, 1, 1) && !sessions.set_game_skill_hint(primary, 2, 1, 1) &&
        !sessions.set_game_skill_hint(primary, 1, 1, 1, 2),
        "session API also rejects empty binding and non-boolean values");
    require(sessions.set_game_skill_hint(primary, 0, 1, 1), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_skill_hint_set, 0);
}
}
int main() {
    try {
        routing_and_reconnect();
        all_switch_combinations();
        legacy_compatibility();
        malformed_and_roles();
        std::cout << "PASS: " << checks << " skill hint server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
