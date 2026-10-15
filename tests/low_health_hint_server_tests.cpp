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
#include "low_health_hint_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_low_health_hint_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "low_health_hint_handler.inc"
        }
    }
    void poll() {
        do {
#include "low_health_hint_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled, uint32_t threshold_percent = 30) {
    Packet packet(Type::game_low_health_hint_set);
    require(packet.add_number(enabled) && packet.add_number(threshold_percent), "fixture encodes enabled flag and threshold");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value, uint32_t threshold_percent = 30) {
    const bool control = type == Type::game_low_health_hint_set;
    require(packet.valid() && packet.type() == type && packet.length() == (control ? 8u : 4u), "valid response frame length");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX, decoded_threshold = 0;
    require(reader.number(decoded) && decoded == value &&
        (!control || (reader.number(decoded_threshold) && decoded_threshold == threshold_percent)) && reader.done(),
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
    const auto defaults = sessions.low_health_hint_command_snapshot(own_game);
    require(!defaults.initialized && defaults.enabled == 0 && defaults.threshold_percent == 30 && defaults.revision == 0,
        "new pair starts disabled with default threshold and no pending command");
    game.poll();
    require(game.stream.sent.empty(), "no unsolicited command before first initialization");
    ui.receive({request(0)});
    const auto initial = sessions.low_health_hint_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0 && initial.threshold_percent == 30,
        "first off value initializes command with default threshold");
    require(!sessions.low_health_hint_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_low_health_hint_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.low_health_hint_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto enabled = sessions.low_health_hint_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > initial.revision, "enabling advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_low_health_hint_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_low_health_hint_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate low health commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.low_health_hint_command_snapshot(own_game).revision == enabled.revision, "status changes preserve low health setting");

    sessions.leave(own_game);
    ui.receive({request(0, 40), request(1, 50)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_low_health_hint_set, 1, 50);
    require(!sessions.low_health_hint_command_snapshot(own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.low_health_hint_command_snapshot(replacement).enabled == 1 &&
        sessions.low_health_hint_command_snapshot(replacement).threshold_percent == 50,
        "primary reconnect preserves the existing game's complete setting");
    ui.receive({request(0)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({request(0)});
    reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(reconnected.stream.sent.back(), Type::game_low_health_hint_set, 0);
    new_ui.receive({request(1, 40)});
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.low_health_hint_command_snapshot(fresh).initialized &&
        sessions.low_health_hint_command_snapshot(fresh).enabled == 0 &&
        sessions.low_health_hint_command_snapshot(fresh).threshold_percent == 30, "both endpoints leaving restores defaults");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_low_health_hint(primary, 1, 50), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.low_health_hint_command_snapshot(fresh).initialized &&
        !sessions.set_game_low_health_hint(primary, 0), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.low_health_hint_command_snapshot(fresh_game).initialized &&
        sessions.low_health_hint_command_snapshot(fresh_game).enabled == 0 &&
        sessions.low_health_hint_command_snapshot(fresh_game).threshold_percent == 30,
        "new account session inherits no revoked setting or threshold");
}
void threshold_changes() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, secondary);
    uint64_t previous_revision = 0;
    size_t expected_packets = 0;
    for (const uint32_t enabled : {0u, 1u}) {
        for (const uint32_t threshold_percent : {30u, 40u, 50u}) {
            ui.receive({request(enabled, threshold_percent)});
            const auto changed = sessions.low_health_hint_command_snapshot(secondary);
            require(changed.initialized && changed.enabled == enabled && changed.threshold_percent == threshold_percent &&
                changed.revision > previous_revision, "threshold changes advance revision with unchanged enabled state");
            previous_revision = changed.revision;
            game.poll();
            require(game.stream.sent.size() == ++expected_packets, "each threshold change forwards one complete setting");
            frame(game.stream.sent.back(), Type::game_low_health_hint_set, enabled, threshold_percent);
            ui.receive({request(enabled, threshold_percent)});
            game.poll();
            require(sessions.low_health_hint_command_snapshot(secondary).revision == previous_revision &&
                game.stream.sent.size() == expected_packets, "duplicate complete setting does not advance or resend");
        }
    }
    ui.receive({request(0, 40)});
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    require(replacement.stream.sent.size() == 1, "reconnect replays disabled setting with selected threshold");
    frame(replacement.stream.sent.back(), Type::game_low_health_hint_set, 0, 40);
}
void independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_jungle_timer(primary, 1), "initialize independent jungle timer");
    const auto jungle = sessions.jungle_timer_command_snapshot(secondary);
    require(sessions.set_game_movement_trace(primary, 1), "initialize independent movement trace");
    const auto movement = sessions.movement_trace_command_snapshot(secondary);
    require(sessions.set_game_disappearance_hint(primary, 1, 60, 1), "initialize independent disappearance hint");
    const auto disappearance = sessions.disappearance_hint_command_snapshot(secondary);
    require(!sessions.low_health_hint_command_snapshot(secondary).initialized, "jungle timer does not initialize low health hint");
    game.poll();
    require(game.stream.sent.empty(), "jungle timer changes do not forward a low health command");
    ui.receive({request(0)});
    const auto hint_off = sessions.low_health_hint_command_snapshot(secondary);
    require(hint_off.initialized && hint_off.enabled == 0, "low health hint can be off while jungle timer is on");
    require(sessions.jungle_timer_command_snapshot(secondary).enabled == 1 &&
        sessions.jungle_timer_command_snapshot(secondary).revision == jungle.revision,
        "low health hint does not change jungle setting or revision");
    require(sessions.movement_trace_command_snapshot(secondary).enabled == 1 &&
        sessions.movement_trace_command_snapshot(secondary).revision == movement.revision,
        "low health hint does not change movement setting or revision");
    game.poll();
    frame(game.stream.sent.back(), Type::game_low_health_hint_set, 0);
    ui.receive({request(1, 40)});
    const auto hint_on = sessions.low_health_hint_command_snapshot(secondary);
    require(hint_on.enabled == 1 && hint_on.threshold_percent == 40 && hint_on.revision > hint_off.revision,
        "low health hint toggles independently with selected threshold");
    require(sessions.set_game_jungle_timer(primary, 0), "jungle timer can be disabled separately");
    require(sessions.low_health_hint_command_snapshot(secondary).revision == hint_on.revision &&
        sessions.low_health_hint_command_snapshot(secondary).enabled == 1 &&
        sessions.low_health_hint_command_snapshot(secondary).threshold_percent == 40,
        "jungle timer does not change low health setting or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only low health revisions control low health packet forwarding");
    frame(game.stream.sent.back(), Type::game_low_health_hint_set, 1, 40);
    const auto disappearance_after_toggle = sessions.disappearance_hint_command_snapshot(secondary);
    require(disappearance_after_toggle.enabled == 1 && disappearance_after_toggle.duration_seconds == 60 &&
        disappearance_after_toggle.show_heroes == 1 && disappearance_after_toggle.revision == disappearance.revision,
        "low health toggle preserves disappearance enabled, duration, portraits and revision");
    require(sessions.set_game_disappearance_hint(primary, 0, 45, 1), "change disappearance separately");
    require(sessions.set_game_movement_trace(primary, 0), "change movement separately");
    require(sessions.low_health_hint_command_snapshot(secondary).enabled == 1 &&
        sessions.low_health_hint_command_snapshot(secondary).threshold_percent == 40 &&
        sessions.low_health_hint_command_snapshot(secondary).revision == hint_on.revision,
        "disappearance and movement changes preserve low health enabled, threshold and revision");
    game.poll();
    require(game.stream.sent.size() == 2, "disappearance changes do not forward low health commands");
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    frame(replacement.stream.sent.back(), Type::game_low_health_hint_set, 1, 40);
    require(sessions.jungle_timer_command_snapshot(replacement.member.binding).enabled == 0 &&
        sessions.movement_trace_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent controls");
    const auto disappearance_after_reconnect = sessions.disappearance_hint_command_snapshot(replacement.member.binding);
    require(disappearance_after_reconnect.enabled == 0 && disappearance_after_reconnect.duration_seconds == 45 &&
        disappearance_after_reconnect.show_heroes == 1,
        "reconnect independently retains disappearance duration, enabled state and portraits");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_low_health_hint_set) == 146);
    require(request(0).valid() && Packet(Type::game_low_health_hint_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "feature packet type is supported and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX)};
    for (const uint32_t threshold_percent : {0u, 29u, 31u, 39u, 41u, 49u, 51u, 100u, UINT32_MAX}) {
        invalid.push_back(request(0, threshold_percent));
        invalid.push_back(request(1, threshold_percent));
    }
    for (size_t length = 0; length <= 12; ++length) {
        if (length == 8) continue;
        Packet packet(Type::game_low_health_hint_set);
        const uint8_t bytes[12]{};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    Packet legacy(Type::game_low_health_hint_set);
    require(legacy.add_number(1), "fixture creates valid legacy 4-byte value");
    invalid.push_back(legacy);
    Packet trailing = request(1, 40);
    require(trailing.add_number(0), "fixture appends trailing data to valid settings");
    invalid.push_back(trailing);
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(1, 50)});
        const auto revision = sessions.low_health_hint_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.low_health_hint_command_snapshot(game);
        require(command.enabled == 1 && command.threshold_percent == 50 && command.revision == revision,
            "malformed request keeps both values and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1)});
    error(endpoint);
    require(!sessions.low_health_hint_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_low_health_hint({}, 1) && !sessions.set_game_low_health_hint(primary, 2),
        "session API also rejects empty binding and non-boolean values");
    require(!sessions.set_game_low_health_hint(primary, 0, 31) && !sessions.set_game_low_health_hint(primary, 1, 0),
        "session API also rejects invalid thresholds while disabled and enabled");
    require(sessions.set_game_low_health_hint(primary, 0), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_low_health_hint_set, 0);
}
}
int main() {
    try {
        routing_and_reconnect();
        threshold_changes();
        independent_controls();
        malformed_and_roles();
        std::cout << "PASS: " << checks << " low health hint server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
