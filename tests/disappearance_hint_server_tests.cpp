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
#include "disappearance_hint_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_disappearance_hint_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "disappearance_hint_handler.inc"
        }
    }
    void poll() {
        do {
#include "disappearance_hint_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled, uint32_t duration_seconds = 30, uint32_t show_heroes = 0) {
    Packet packet(Type::game_disappearance_hint_set);
    require(packet.add_number(enabled) && packet.add_number(duration_seconds) && packet.add_number(show_heroes),
        "fixture encodes enabled, duration, and portrait visibility");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value, uint32_t duration_seconds = 30, uint32_t show_heroes = 0) {
    const bool control = type == Type::game_disappearance_hint_set;
    require(packet.valid() && packet.type() == type && packet.length() == (control ? 12u : 4u), "valid response frame length");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX, decoded_duration = 0, decoded_show_heroes = UINT32_MAX;
    require(reader.number(decoded) && decoded == value &&
        (!control || (reader.number(decoded_duration) && decoded_duration == duration_seconds &&
            reader.number(decoded_show_heroes) && decoded_show_heroes == show_heroes)) && reader.done(),
        "response contains exactly expected values");
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
    const auto initial = sessions.disappearance_hint_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0 && initial.duration_seconds == 30 &&
        initial.show_heroes == 0, "first off value initializes command with default duration and portraits off");
    require(!sessions.disappearance_hint_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_disappearance_hint_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.disappearance_hint_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto enabled = sessions.disappearance_hint_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > initial.revision, "enabling advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_disappearance_hint_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_disappearance_hint_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate disappearance commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.disappearance_hint_command_snapshot(own_game).revision == enabled.revision, "status changes preserve disappearance setting");

    sessions.leave(own_game);
    ui.receive({request(0, 45), request(1, 60, 1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_disappearance_hint_set, 1, 60, 1);
    require(!sessions.disappearance_hint_command_snapshot(own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.disappearance_hint_command_snapshot(replacement).show_heroes == 1,
        "primary reconnect preserves the portrait setting for the existing game");
    ui.receive({request(0)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({request(0, 30, 1)});
    reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(reconnected.stream.sent.back(), Type::game_disappearance_hint_set, 0, 30, 1);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    const auto cleared = sessions.disappearance_hint_command_snapshot(fresh);
    require(!cleared.initialized && cleared.enabled == 0 && cleared.duration_seconds == 30 && cleared.show_heroes == 0,
        "both endpoints leaving discards cached setting and restores defaults");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_disappearance_hint(primary, 1, 45, 1), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.disappearance_hint_command_snapshot(fresh).initialized &&
        !sessions.set_game_disappearance_hint(primary, 0, 30), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    const auto after_revocation = sessions.disappearance_hint_command_snapshot(fresh_game);
    require(!after_revocation.initialized && after_revocation.duration_seconds == 30 && after_revocation.show_heroes == 0,
        "new account session inherits no revoked setting, duration, or portrait setting");
}
void setting_changes() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, secondary);
    uint64_t previous_revision = 0;
    size_t expected_packets = 0;
    for (const uint32_t enabled : {0u, 1u}) {
        for (const uint32_t duration_seconds : {30u, 45u, 60u}) {
            for (const uint32_t show_heroes : {0u, 1u}) {
                ui.receive({request(enabled, duration_seconds, show_heroes)});
                const auto changed = sessions.disappearance_hint_command_snapshot(secondary);
                require(changed.initialized && changed.enabled == enabled && changed.duration_seconds == duration_seconds &&
                    changed.show_heroes == show_heroes && changed.revision > previous_revision,
                    "duration or portrait changes advance revision even when enabled is unchanged");
                previous_revision = changed.revision;
                game.poll();
                require(game.stream.sent.size() == ++expected_packets, "each setting change forwards one complete setting");
                frame(game.stream.sent.back(), Type::game_disappearance_hint_set, enabled, duration_seconds, show_heroes);
                ui.receive({request(enabled, duration_seconds, show_heroes)});
                game.poll();
                require(sessions.disappearance_hint_command_snapshot(secondary).revision == previous_revision &&
                    game.stream.sent.size() == expected_packets, "duplicate complete setting does not advance or resend");
            }
        }
    }
    ui.receive({request(0, 45, 1)});
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    require(replacement.stream.sent.size() == 1, "reconnect replays a disabled setting with selected duration and portraits");
    frame(replacement.stream.sent.back(), Type::game_disappearance_hint_set, 0, 45, 1);
}
void independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_jungle_timer(primary, 1), "initialize independent jungle timer");
    const auto jungle = sessions.jungle_timer_command_snapshot(secondary);
    require(sessions.set_game_eye_timer(primary, 1), "initialize independent eye timer");
    const auto eye = sessions.eye_timer_command_snapshot(secondary);
    require(!sessions.disappearance_hint_command_snapshot(secondary).initialized, "jungle timer does not initialize disappearance hint");
    game.poll();
    require(game.stream.sent.empty(), "other controls do not forward a disappearance command");
    ui.receive({request(0)});
    const auto hint_off = sessions.disappearance_hint_command_snapshot(secondary);
    require(hint_off.initialized && hint_off.enabled == 0, "disappearance hint can be off while jungle timer is on");
    require(sessions.jungle_timer_command_snapshot(secondary).enabled == 1 &&
        sessions.jungle_timer_command_snapshot(secondary).revision == jungle.revision,
        "disappearance hint does not change jungle setting or revision");
    game.poll();
    frame(game.stream.sent.back(), Type::game_disappearance_hint_set, 0);
    ui.receive({request(1)});
    const auto hint_on = sessions.disappearance_hint_command_snapshot(secondary);
    require(hint_on.enabled == 1 && hint_on.revision > hint_off.revision, "disappearance hint toggles independently");
    require(sessions.set_game_jungle_timer(primary, 0), "jungle timer can be disabled separately");
    require(sessions.disappearance_hint_command_snapshot(secondary).revision == hint_on.revision &&
        sessions.disappearance_hint_command_snapshot(secondary).enabled == 1,
        "jungle timer does not change disappearance setting or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only disappearance revisions control disappearance packet forwarding");
    frame(game.stream.sent.back(), Type::game_disappearance_hint_set, 1);
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    frame(replacement.stream.sent.back(), Type::game_disappearance_hint_set, 1);
    require(sessions.jungle_timer_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent jungle timer setting");
    const auto eye_after_reconnect = sessions.eye_timer_command_snapshot(replacement.member.binding);
    require(eye_after_reconnect.enabled == 1 && eye_after_reconnect.revision == eye.revision,
        "disappearance hint does not change eye timer setting or revision across reconnect");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_disappearance_hint_set) == 143);
    require(request(0).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "feature packet type is supported and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX), request(0, 30, 2), request(1, 30, UINT32_MAX)};
    for (const uint32_t duration_seconds : {0u, 20u, 29u, 31u, 40u, 44u, 46u, 50u, 59u, 61u, UINT32_MAX}) {
        invalid.push_back(request(0, duration_seconds));
        invalid.push_back(request(1, duration_seconds));
    }
    for (size_t length = 0; length <= 16; ++length) {
        if (length == 12) continue;
        Packet packet(Type::game_disappearance_hint_set);
        const uint8_t bytes[16]{};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    Packet legacy(Type::game_disappearance_hint_set);
    require(legacy.add_number(1) && legacy.add_number(30), "fixture creates valid legacy 8-byte values");
    invalid.push_back(legacy);
    Packet trailing = request(1, 30, 1);
    require(trailing.add_number(0), "fixture appends trailing data to an otherwise valid request");
    invalid.push_back(trailing);
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(1, 60, 1)});
        const auto revision = sessions.disappearance_hint_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.disappearance_hint_command_snapshot(game);
        require(command.enabled == 1 && command.duration_seconds == 60 && command.show_heroes == 1 && command.revision == revision,
            "malformed request keeps all three values and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1)});
    error(endpoint);
    require(!sessions.disappearance_hint_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_disappearance_hint({}, 1, 30) && !sessions.set_game_disappearance_hint(primary, 2, 30),
        "session API also rejects empty binding and non-boolean values");
    require(!sessions.set_game_disappearance_hint(primary, 0, 25) && !sessions.set_game_disappearance_hint(primary, 1, 0),
        "session API also rejects invalid durations while disabled and enabled");
    require(!sessions.set_game_disappearance_hint(primary, 0, 30, 2) &&
        !sessions.set_game_disappearance_hint(primary, 1, 30, UINT32_MAX),
        "session API also rejects non-boolean portrait settings while disabled and enabled");
    require(sessions.set_game_disappearance_hint(primary, 0, 30), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_disappearance_hint_set, 0);
}
}
int main() {
    try {
        routing_and_reconnect();
        setting_changes();
        independent_controls();
        malformed_and_roles();
        std::cout << "PASS: " << checks << " disappearance hint server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
