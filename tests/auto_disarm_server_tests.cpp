// The launcher extracts the real dispatch and forwarding branches. Only TLS
// writes are replaced; these tests do not start a server or connect to a DB.
#include "../network/protocol.h"
#include "../sessions.h"
#include <iostream>
#include <cmath>
#include <limits>
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
#include "auto_disarm_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_auto_disarm_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "auto_disarm_handler.inc"
        }
    }
    void poll() {
        do {
#include "auto_disarm_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled, uint32_t on_skill = 1, uint32_t direct = 1,
    float range = chh::wire::game_auto_disarm_range_default) {
    Packet packet(Type::game_auto_disarm_set);
    require(packet.add_number(enabled) && packet.add_number(on_skill) && packet.add_number(direct) &&
        packet.add_number(chh::wire::game_auto_disarm_range_to_bits(range)),
        "fixture encodes three flags and float range bits");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value, uint32_t on_skill = 1, uint32_t direct = 1,
    float range = chh::wire::game_auto_disarm_range_default) {
    const bool control = type == Type::game_auto_disarm_set;
    require(packet.valid() && packet.type() == type && packet.length() == (control ? 16u : 4u), "valid response frame length");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX, decoded_on_skill = UINT32_MAX, decoded_direct = UINT32_MAX;
    require(reader.number(decoded) && decoded == value, "response contains expected first value");
    if (control) require(reader.number(decoded_on_skill) && reader.number(decoded_direct) &&
        decoded_on_skill == on_skill && decoded_direct == direct, "response preserves on_skill and direct flags");
    if (control) {
        uint32_t range_bits = 0;
        require(reader.number(range_bits) && range_bits == chh::wire::game_auto_disarm_range_to_bits(range) &&
            chh::wire::game_auto_disarm_range_from_bits(range_bits) == range, "range preserves exact IEEE 754 float bits");
    }
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
    const auto initial = sessions.auto_disarm_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0, "first off value initializes command");
    require(!sessions.auto_disarm_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_auto_disarm_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.auto_disarm_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto enabled = sessions.auto_disarm_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > initial.revision, "enabling advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_auto_disarm_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_auto_disarm_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate auto-disarm commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.auto_disarm_command_snapshot(own_game).revision == enabled.revision, "status changes preserve auto-disarm settings");

    sessions.leave(own_game);
    ui.receive({request(0), request(1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_auto_disarm_set, 1);
    require(!sessions.auto_disarm_command_snapshot(own_game).initialized, "old game token loses snapshot access");
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
    frame(reconnected.stream.sent.back(), Type::game_auto_disarm_set, 0);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.auto_disarm_command_snapshot(fresh).initialized, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_auto_disarm(primary, 1, 1, 1, chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default)), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.auto_disarm_command_snapshot(fresh).initialized &&
        !sessions.set_game_auto_disarm(primary, 0, 1, 1, chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default)), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.auto_disarm_command_snapshot(fresh_game).initialized, "new account session inherits no revoked setting");
}
void all_switch_combinations() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    for (uint32_t bits = 0; bits < 8; ++bits) {
        const uint32_t enabled = (bits >> 2) & 1, on_skill = (bits >> 1) & 1, direct = bits & 1;
        ui.receive({request(enabled, on_skill, direct)});
        game.poll();
        require(game.stream.sent.size() == bits + 1, "each changed complete card is forwarded once");
        frame(game.stream.sent.back(), Type::game_auto_disarm_set, enabled, on_skill, direct);
        const auto snapshot = sessions.auto_disarm_command_snapshot(game.member.binding);
        require(snapshot.enabled == enabled && snapshot.on_skill == on_skill && snapshot.direct == direct,
            "session applies each complete card atomically");
        ui.receive({request(enabled, on_skill, direct)});
        game.poll();
        require(game.stream.sent.size() == bits + 1, "duplicate card does not resend");
    }
    require(ui.stream.sent.empty(), "all eight control combinations accepted");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_auto_disarm_set) == 136);
    require(request(0).valid() && Packet(Type::game_movement_trace_set).valid() && !Packet(static_cast<Type>(0)).valid() &&
        Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "supported wire types are valid and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX), request(0, 2, 0), request(0, UINT32_MAX, 0),
        request(0, 0, 2), request(0, 0, UINT32_MAX)};
    const auto valid_payload = request(0, 0, 0, 2.5f);
    for (size_t length = 0; length <= 20; ++length) {
        if (length == 16) continue;
        Packet packet(Type::game_auto_disarm_set);
        uint8_t bytes[20]{};
        std::memcpy(bytes, valid_payload.bytes.data() + 8, 16);
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(1)});
        const auto revision = sessions.auto_disarm_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.auto_disarm_command_snapshot(game);
        require(command.enabled == 1 && command.on_skill == 1 && command.direct == 1 &&
            command.range_bits == chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default) &&
            command.revision == revision, "malformed request keeps complete card and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1)});
    error(endpoint);
    require(!sessions.auto_disarm_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_auto_disarm({}, 1, 1, 1, chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default)) && !sessions.set_game_auto_disarm(primary, 2, 1, 1, chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default)),
        "session API also rejects empty binding and non-boolean values");
    require(sessions.set_game_auto_disarm(primary, 0, 1, 1, chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default)), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_auto_disarm_set, 0);
}
void range_values_and_reconnect() {
    using namespace chh::wire;
    require(game_auto_disarm_range_min == 1.0f && game_auto_disarm_range_max == 5.0f &&
        game_auto_disarm_range_default == 5.0f && game_auto_disarm_range_step == 0.1f,
        "range constants preserve configured meters and step");
    const auto known = request(0, 1, 1, 2.5f);
    require(game_auto_disarm_range_to_bits(2.5f) == 0x40200000u && known.bytes[20] == 0x40 &&
        known.bytes[21] == 0x20 && known.bytes[22] == 0 && known.bytes[23] == 0,
        "known fractional range uses IEEE 754 bits in big-endian field order");
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({request(0)});
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary); game.poll();
    auto current = sessions.auto_disarm_command_snapshot(secondary);
    require(current.initialized && !current.enabled && current.on_skill && current.direct &&
        current.range_bits == game_auto_disarm_range_to_bits(game_auto_disarm_range_default),
        "first disabled card before game connection initializes all defaults");
    for (const float range : {game_auto_disarm_range_min, game_auto_disarm_range_max, 2.35f, 3.1f}) {
        const auto before = sessions.auto_disarm_command_snapshot(secondary);
        const auto sent = game.stream.sent.size();
        require(valid_game_auto_disarm_range(range), "finite boundary and fractional values accepted");
        ui.receive({request(0, 1, 1, range)}); game.poll();
        current = sessions.auto_disarm_command_snapshot(secondary);
        require(current.revision > before.revision && current.range_bits == game_auto_disarm_range_to_bits(range) &&
            game.stream.sent.size() == sent + 1, "range-only change advances revision and forwards exact float bits");
        frame(game.stream.sent.back(), Type::game_auto_disarm_set, 0, 1, 1, range);
        ui.receive({request(0, 1, 1, range)}); game.poll();
        require(sessions.auto_disarm_command_snapshot(secondary).revision == current.revision &&
            game.stream.sent.size() == sent + 1, "identical range does not advance revision or resend");
    }
    sessions.leave(secondary);
    ui.receive({request(0, 1, 1, 4.25f)});
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary));
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect sends latest fractional range once");
    frame(reconnected.stream.sent.back(), Type::game_auto_disarm_set, 0, 1, 1, 4.25f);
    require(ui.stream.sent.empty(), "valid float requests never disconnect primary");
}
void invalid_ranges_are_atomic() {
    using namespace chh::wire;
    const float infinity = std::numeric_limits<float>::infinity();
    const std::array<float, 7> invalid{
        std::nextafter(game_auto_disarm_range_min, -infinity),
        std::nextafter(game_auto_disarm_range_max, infinity),
        infinity, -infinity, std::numeric_limits<float>::quiet_NaN(), -1.0f, 0.0f};
    for (const float range : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto secondary = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(0, 1, 1, 2.5f)});
        const auto before = sessions.auto_disarm_command_snapshot(secondary);
        const auto bits = game_auto_disarm_range_to_bits(range);
        require(!valid_game_auto_disarm_range(range) && !sessions.set_game_auto_disarm(primary, 1, 0, 0, bits),
            "invalid range rejected by session API");
        auto after = sessions.auto_disarm_command_snapshot(secondary);
        require(after.revision == before.revision && !after.enabled && after.on_skill && after.direct &&
            after.range_bits == before.range_bits, "invalid range cannot partially update switches or cached float");
        ui.receive({request(1, 0, 0, range), request(1)}); error(ui);
        after = sessions.auto_disarm_command_snapshot(secondary);
        require(after.revision == before.revision && !after.enabled && after.on_skill && after.direct &&
            after.range_bits == before.range_bits, "real handler atomically rejects invalid range and stops dispatch");
    }
}
}
int main() {
    try {
        routing_and_reconnect();
        all_switch_combinations();
        range_values_and_reconnect();
        invalid_ranges_are_atomic();
        malformed_and_roles();
        std::cout << "PASS: " << checks << " auto disarm server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
