// Tests extract the real server receive/forward branches; no server or DB is started.
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
using Flags = std::array<uint32_t, 11>;
constexpr Flags defaults{0, 1, 1, 1, 1, 1, 1, 1, 300, 0, 0};
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "auto_cleanse_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_auto_cleanse_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "auto_cleanse_handler.inc"
        }
    }
    void poll() {
        do {
#include "auto_cleanse_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    const auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture creates current binding");
    return result;
}
bool set(Sessions& sessions, const Sessions::Binding& binding, const Flags& flags) {
    return sessions.set_game_auto_cleanse(binding, flags[0], flags[1], flags[2], flags[3], flags[4], flags[5], flags[6],
        flags[7], flags[8], flags[9], flags[10]);
}
Packet request(const Flags& flags) {
    Packet packet(Type::game_auto_cleanse_set);
    for (const auto flag : flags) require(packet.add_number(flag), "encode complete control payload");
    return packet;
}
Flags values(const Sessions::AutoCleanseSnapshot& snapshot) {
    return {snapshot.enabled, snapshot.ignite, snapshot.disarm, snapshot.root,
        snapshot.control, snapshot.silence, snapshot.stun, snapshot.taunt, snapshot.delay_ms, snapshot.min_count,
        snapshot.control_time_ms};
}
void frame(const Packet& packet, const Flags& expected) {
    require(packet.valid() && packet.type() == Type::game_auto_cleanse_set && packet.length() == 44,
        "forwarded auto-cleanse frame has exactly 44 bytes");
    chh::wire::Reader reader{packet};
    for (const auto flag : expected) {
        uint32_t actual = UINT32_MAX;
        require(reader.number(actual) && actual == flag, "forwarded field order and value match control card");
    }
    require(reader.done(), "forwarded frame has no trailing fields");
}
void error(const Worker& worker) {
    require(worker.stream.sent.size() == 1 && worker.stream.sent[0].type() == Type::error &&
        worker.stream.sent[0].length() == 4 && worker.stream.sent[0].number() == uint32_t(chh::wire::Error::protocol),
        "invalid request returns one protocol error");
}
void combinations_and_single_fields() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    uint64_t previous_revision = 0;
    for (uint32_t bits = 0; bits < 256; ++bits) {
        Flags flags = defaults;
        for (size_t n = 0; n < 8; ++n) flags[n] = (bits >> (7 - n)) & 1;
        ui.receive({request(flags)});
        const auto snapshot = sessions.auto_cleanse_command_snapshot(game.member.binding);
        require(snapshot.initialized && values(snapshot) == flags && snapshot.revision > previous_revision,
            "each of 256 complete cards updates its consistent snapshot");
        previous_revision = snapshot.revision;
        game.poll();
        require(game.stream.sent.size() == bits + 1, "each changed card forwarded exactly once");
        frame(game.stream.sent.back(), flags);
        ui.receive({request(flags)}); game.poll();
        require(game.stream.sent.size() == bits + 1 &&
            sessions.auto_cleanse_command_snapshot(game.member.binding).revision == previous_revision,
            "repeated eleven-field card does not advance revision or resend");
    }
    for (size_t field = 1; field < 8; ++field) {
        ui.receive({request(defaults)}); game.poll();
        const auto before = sessions.auto_cleanse_command_snapshot(game.member.binding);
        const auto sent = game.stream.sent.size();
        auto changed = defaults; changed[field] = 0;
        ui.receive({request(changed)}); game.poll();
        require(sessions.auto_cleanse_command_snapshot(game.member.binding).revision > before.revision &&
            game.stream.sent.size() == sent + 1, "changing only one child switch advances revision and forwards");
        frame(game.stream.sent.back(), changed);
    }
    require(ui.stream.sent.empty(), "all valid cards accepted without errors");
}
void initialization_pairing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({request(defaults)});
    auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    game.poll(); game.poll(); other_ip.poll(); other_account.poll();
    require(game.stream.sent.size() == 1, "first disabled card initializes late game once");
    frame(game.stream.sent.back(), defaults);
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "commands isolated by account and observed IP");
    require(!sessions.auto_cleanse_command_snapshot(primary).initialized, "primary cannot read game-bound settings");
    auto latest = defaults; latest[0] = 1; latest[2] = 0; latest[5] = 0;
    ui.receive({request(latest)});
    game.stream.writable = false; game.poll();
    require(game.stream.sent.size() == 1, "failed send does not emit a frame");
    game.stream.writable = true; game.poll();
    require(game.stream.sent.size() == 2, "failed send leaves revision pending");
    frame(game.stream.sent.back(), latest);
    sessions.leave(secondary);
    latest[4] = 0; ui.receive({request(latest)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement); reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect sends latest eleven-field snapshot once");
    frame(reconnected.stream.sent.back(), latest);
    require(!sessions.auto_cleanse_command_snapshot(secondary).initialized, "stale game token cannot read new connection");
    sessions.leave(secondary);
    require(sessions.current(replacement), "late disconnect preserves replacement game");
    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({request(defaults)}); error(ui);
    Worker new_ui(sessions, primary); new_ui.receive({request(defaults)}); reconnected.poll();
    frame(reconnected.stream.sent.back(), defaults);
    sessions.leave(primary); sessions.leave(replacement);
    secondary = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.auto_cleanse_command_snapshot(secondary).initialized, "both endpoints leaving discards cached settings");
    primary = enter(sessions, "owner", 10, Role::primary);
    require(set(sessions, primary, defaults), "initialize before account revocation");
    require(sessions.invalidate_account("owner") && !set(sessions, primary, defaults) &&
        !sessions.auto_cleanse_command_snapshot(secondary).initialized, "account invalidation clears settings and rejects stale bindings");
    require(sessions.current(other_account.member.binding), "revocation preserves unrelated account");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_auto_cleanse_set) == 135);
    require(Packet(Type::game_auto_cleanse_set).valid() && Packet(Type::game_movement_trace_set).valid() &&
        Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid(),
        "supported wire types are valid and unknown types are rejected");
    std::vector<Packet> invalid;
    for (size_t field = 0; field < 8; ++field) {
        for (const uint32_t bad : {2u, UINT32_MAX}) {
            auto flags = defaults; flags[field] = bad; invalid.push_back(request(flags));
            Sessions sessions;
            const auto primary = enter(sessions, "owner", 10, Role::primary);
            require(!set(sessions, primary, flags), "session API rejects non-boolean value in each of eight fields");
        }
    }
    for (size_t field = 8; field < 11; ++field) {
        const uint32_t first_invalid = field == 8 ? 301u : field == 9 ? 7u : 1001u;
        for (const uint32_t bad : {first_invalid, UINT32_MAX}) {
            auto fields = defaults; fields[0] = 1; fields[field] = bad;
            invalid.push_back(request(fields));
            Sessions sessions;
            const auto primary = enter(sessions, "owner", 10, Role::primary);
            const auto game = enter(sessions, "owner", 10, Role::secondary);
            require(set(sessions, primary, defaults), "initialize before invalid numeric parameter");
            const auto before = sessions.auto_cleanse_command_snapshot(game);
            require(!set(sessions, primary, fields) &&
                sessions.auto_cleanse_command_snapshot(game).revision == before.revision &&
                values(sessions.auto_cleanse_command_snapshot(game)) == defaults,
                "numeric parameter bounds reject before any partial write");
        }
    }
    for (size_t length = 0; length <= 48; ++length) {
        if (length == 44) continue;
        Packet packet(Type::game_auto_cleanse_set); const uint8_t bytes[48]{};
        require(packet.add_bytes(bytes, length), "build malformed payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        ui.receive({request(defaults)});
        const auto before = sessions.auto_cleanse_command_snapshot(game);
        ui.receive({packet, request(Flags{})}); error(ui);
        const auto after = sessions.auto_cleanse_command_snapshot(game);
        require(after.revision == before.revision && values(after) == defaults,
            "malformed input leaves complete settings unchanged and exits dispatch");
    }
    Sessions sessions;
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.receive({request(defaults)}); error(game);
    require(!sessions.auto_cleanse_command_snapshot(game.member.binding).initialized &&
        !set(sessions, game.member.binding, defaults) && !set(sessions, {}, defaults),
        "game-role and empty bindings cannot initialize settings");
}
void numeric_parameters_and_reconnect() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary);
    ui.receive({request(defaults)}); game.poll();
    const auto initial = sessions.auto_cleanse_command_snapshot(secondary);
    require(initial.delay_ms == 300 && initial.min_count == 0 && initial.control_time_ms == 0 && !initial.enabled,
        "first disabled initialization retains default numeric parameters");
    for (const auto parameters : {std::array<uint32_t, 2>{0, 0}, {300, 0}, {300, 6}, {0, 6}, {0, 0}}) {
        auto changed = defaults; changed[8] = parameters[0]; changed[9] = parameters[1];
        const auto before = sessions.auto_cleanse_command_snapshot(secondary);
        const auto sent = game.stream.sent.size();
        ui.receive({request(changed)}); game.poll();
        const auto after = sessions.auto_cleanse_command_snapshot(secondary);
        require(after.revision > before.revision && values(after) == changed && game.stream.sent.size() == sent + 1,
            "changing only delay or count advances revision and forwards boundary value");
        frame(game.stream.sent.back(), changed);
        ui.receive({request(changed)}); game.poll();
        require(sessions.auto_cleanse_command_snapshot(secondary).revision == after.revision &&
            game.stream.sent.size() == sent + 1, "repeated numeric parameters do not resend");
    }
    auto latest = defaults; latest[8] = 137; latest[9] = 6;
    sessions.leave(secondary); ui.receive({request(latest)});
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary));
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1 &&
        values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == latest,
        "reconnect preserves both numeric parameters together with eight switches");
    frame(reconnected.stream.sent.back(), latest);
    require(ui.stream.sent.empty(), "valid numeric parameter boundaries and interior value are accepted");
}
void taunt_change_reconnect_and_atomic_rejection() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary); Worker game(sessions, secondary);
    ui.receive({request(defaults)}); game.poll();
    const auto initial = sessions.auto_cleanse_command_snapshot(secondary);
    require(initial.taunt == 1 && initial.delay_ms == 300 && initial.min_count == 0,
        "taunt defaults to enabled before numeric parameters");
    auto changed = defaults; changed[7] = 0;
    ui.receive({request(changed)}); game.poll();
    const auto toggled = sessions.auto_cleanse_command_snapshot(secondary);
    require(toggled.revision > initial.revision && values(toggled) == changed && game.stream.sent.size() == 2,
        "changing taunt alone advances revision and forwards all eleven values");
    frame(game.stream.sent.back(), changed);
    sessions.leave(secondary);
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary));
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "taunt-only update survives reconnect and forwards once");
    frame(reconnected.stream.sent.back(), changed);
    for (const uint32_t bad : {2u, UINT32_MAX}) {
        auto invalid = defaults; invalid[0] = 1; invalid[7] = bad; invalid[8] = 0; invalid[9] = 6;
        const auto before = sessions.auto_cleanse_command_snapshot(reconnected.member.binding);
        require(!set(sessions, primary, invalid) &&
            sessions.auto_cleanse_command_snapshot(reconnected.member.binding).revision == before.revision &&
            values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == changed,
            "invalid taunt rejects main and parameter changes atomically at session boundary");
        ui.stream.sent.clear(); ui.receive({request(invalid)}); error(ui);
        require(sessions.auto_cleanse_command_snapshot(reconnected.member.binding).revision == before.revision &&
            values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == changed,
            "invalid taunt leaves all eleven values unchanged at real receive boundary");
    }
}
void control_time_change_and_reconnect() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary); Worker game(sessions, secondary);
    ui.receive({request(defaults)}); game.poll();
    require(sessions.auto_cleanse_command_snapshot(secondary).control_time_ms == 0,
        "control time initializes to zero milliseconds");
    for (const uint32_t control_time : {1000u, 0u, 427u}) {
        auto changed = defaults; changed[10] = control_time;
        const auto before = sessions.auto_cleanse_command_snapshot(secondary);
        const auto sent = game.stream.sent.size();
        ui.receive({request(changed)}); game.poll();
        const auto after = sessions.auto_cleanse_command_snapshot(secondary);
        require(after.revision > before.revision && values(after) == changed && game.stream.sent.size() == sent + 1,
            "control-time-only change advances revision and forwards boundary or interior value");
        frame(game.stream.sent.back(), changed);
        ui.receive({request(changed)}); game.poll();
        require(game.stream.sent.size() == sent + 1 &&
            sessions.auto_cleanse_command_snapshot(secondary).revision == after.revision,
            "identical control time does not advance revision or resend");
    }
    sessions.leave(secondary);
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary));
    reconnected.poll(); reconnected.poll();
    auto latest = defaults; latest[10] = 427;
    require(reconnected.stream.sent.size() == 1 &&
        values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == latest,
        "control-time-only update survives reconnect with all other values unchanged");
    frame(reconnected.stream.sent.back(), latest);
    for (const uint32_t bad : {1001u, UINT32_MAX}) {
        auto invalid = latest; invalid[0] = 1; invalid[7] = 0; invalid[8] = 0; invalid[9] = 6; invalid[10] = bad;
        const auto before = sessions.auto_cleanse_command_snapshot(reconnected.member.binding);
        require(!set(sessions, primary, invalid) &&
            sessions.auto_cleanse_command_snapshot(reconnected.member.binding).revision == before.revision &&
            values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == latest,
            "invalid control time rejects changed switches and parameters atomically");
        ui.stream.sent.clear(); ui.receive({request(invalid)}); error(ui);
        require(sessions.auto_cleanse_command_snapshot(reconnected.member.binding).revision == before.revision &&
            values(sessions.auto_cleanse_command_snapshot(reconnected.member.binding)) == latest,
            "invalid control time leaves all eleven values unchanged at real receive boundary");
    }
}
}
int main() {
    try {
        combinations_and_single_fields(); initialization_pairing_and_reconnect();
        numeric_parameters_and_reconnect(); taunt_change_reconnect_and_atomic_rejection();
        control_time_change_and_reconnect(); malformed_and_roles();
        std::cout << "PASS: " << checks << " auto cleanse server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
