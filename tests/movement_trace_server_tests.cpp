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
#include "movement_trace_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_movement_trace_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "movement_trace_handler.inc"
        }
    }
    void poll() {
        do {
#include "movement_trace_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled) {
    Packet packet(Type::game_movement_trace_set);
    require(packet.add_number(enabled), "fixture encodes enabled flag");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value) {
    require(packet.valid() && packet.type() == type && packet.length() == 4, "valid four-byte response frame");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX;
    require(reader.number(decoded) && reader.done() && decoded == value, "response contains exactly expected value");
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
    const auto initial = sessions.movement_trace_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0, "first off value initializes command");
    require(!sessions.movement_trace_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_movement_trace_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.movement_trace_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto enabled = sessions.movement_trace_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > initial.revision, "enabling advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_movement_trace_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_movement_trace_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate movement commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.movement_trace_command_snapshot(own_game).revision == enabled.revision, "status changes preserve movement setting");

    sessions.leave(own_game);
    ui.receive({request(0), request(1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_movement_trace_set, 1);
    require(!sessions.movement_trace_command_snapshot(own_game).initialized, "old game token loses snapshot access");
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
    frame(reconnected.stream.sent.back(), Type::game_movement_trace_set, 0);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.movement_trace_command_snapshot(fresh).initialized, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_movement_trace(primary, 1), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.movement_trace_command_snapshot(fresh).initialized &&
        !sessions.set_game_movement_trace(primary, 0), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.movement_trace_command_snapshot(fresh_game).initialized, "new account session inherits no revoked setting");
}
void independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_jungle_timer(primary, 1), "initialize independent jungle timer");
    const auto jungle = sessions.jungle_timer_command_snapshot(secondary);
    require(sessions.set_game_disappearance_hint(primary, 1, 60), "initialize independent disappearance hint");
    const auto disappearance = sessions.disappearance_hint_command_snapshot(secondary);
    require(!sessions.movement_trace_command_snapshot(secondary).initialized, "jungle timer does not initialize movement trace");
    game.poll();
    require(game.stream.sent.empty(), "jungle timer changes do not forward a movement command");
    ui.receive({request(0)});
    const auto trace_off = sessions.movement_trace_command_snapshot(secondary);
    require(trace_off.initialized && trace_off.enabled == 0, "movement trace can be off while jungle timer is on");
    require(sessions.jungle_timer_command_snapshot(secondary).enabled == 1 &&
        sessions.jungle_timer_command_snapshot(secondary).revision == jungle.revision,
        "movement trace does not change jungle setting or revision");
    game.poll();
    frame(game.stream.sent.back(), Type::game_movement_trace_set, 0);
    ui.receive({request(1)});
    const auto trace_on = sessions.movement_trace_command_snapshot(secondary);
    require(trace_on.enabled == 1 && trace_on.revision > trace_off.revision, "movement trace toggles independently");
    require(sessions.set_game_jungle_timer(primary, 0), "jungle timer can be disabled separately");
    require(sessions.movement_trace_command_snapshot(secondary).revision == trace_on.revision &&
        sessions.movement_trace_command_snapshot(secondary).enabled == 1,
        "jungle timer does not change movement setting or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only movement revisions control movement packet forwarding");
    frame(game.stream.sent.back(), Type::game_movement_trace_set, 1);
    const auto disappearance_after_toggle = sessions.disappearance_hint_command_snapshot(secondary);
    require(disappearance_after_toggle.enabled == 1 && disappearance_after_toggle.duration_seconds == 60 &&
        disappearance_after_toggle.revision == disappearance.revision,
        "movement toggle preserves disappearance enabled, duration and revision");
    require(sessions.set_game_disappearance_hint(primary, 0, 45), "change disappearance separately");
    require(sessions.movement_trace_command_snapshot(secondary).enabled == 1 &&
        sessions.movement_trace_command_snapshot(secondary).revision == trace_on.revision,
        "disappearance changes preserve movement enabled and revision");
    game.poll();
    require(game.stream.sent.size() == 2, "disappearance changes do not forward movement commands");
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    frame(replacement.stream.sent.back(), Type::game_movement_trace_set, 1);
    require(sessions.jungle_timer_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent controls");
    const auto disappearance_after_reconnect = sessions.disappearance_hint_command_snapshot(replacement.member.binding);
    require(disappearance_after_reconnect.enabled == 0 && disappearance_after_reconnect.duration_seconds == 45,
        "reconnect independently retains disappearance duration and enabled state");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_movement_trace_set) == 144);
    require(request(0).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "feature packet type is supported and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX)};
    for (size_t length = 0; length <= 8; ++length) {
        if (length == 4) continue;
        Packet packet(Type::game_movement_trace_set);
        const uint8_t bytes[8]{};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(1)});
        const auto revision = sessions.movement_trace_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.movement_trace_command_snapshot(game);
        require(command.enabled == 1 && command.revision == revision, "malformed request keeps setting and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1)});
    error(endpoint);
    require(!sessions.movement_trace_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_movement_trace({}, 1) && !sessions.set_game_movement_trace(primary, 2),
        "session API also rejects empty binding and non-boolean values");
    require(sessions.set_game_movement_trace(primary, 0), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_movement_trace_set, 0);
}
}
int main() {
    try {
        routing_and_reconnect();
        independent_controls();
        malformed_and_roles();
        std::cout << "PASS: " << checks << " movement trace server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
