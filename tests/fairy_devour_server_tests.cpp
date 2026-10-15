// The launcher extracts the real receive and forward branches. No live server or DB.
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
using namespace chh::wire;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "fairy_devour_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_auto_fairy_command_revision = UINT64_MAX;
    uint64_t sent_auto_devour_command_revision = UINT64_MAX;
    uint64_t sent_auto_glove_command_revision = UINT64_MAX;
    uint64_t sent_auto_lingzhou_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "fairy_devour_handler.inc"
        }
    }
    void poll() {
        do {
#include "fairy_devour_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    const auto binding = sessions.enter(account, ip, role);
    require(bool(binding), "fixture enters current session");
    return binding;
}
struct State {
    uint64_t revision = 0;
    uint32_t enabled = 0, minions = 1;
    bool initialized = false;
};
State snapshot(Sessions& sessions, const Sessions::Binding& binding, Type type) {
    if (type == Type::game_auto_lingzhou_set) {
        const auto value = sessions.auto_lingzhou_command_snapshot(binding);
        return {value.revision, value.enabled, 1, value.initialized};
    }
    if (type == Type::game_auto_glove_set) {
        const auto value = sessions.auto_glove_command_snapshot(binding);
        return {value.revision, value.enabled, 1, value.initialized};
    }
    if (type == Type::game_auto_fairy_set) {
        const auto value = sessions.auto_fairy_command_snapshot(binding);
        return {value.revision, value.enabled, 1, value.initialized};
    }
    const auto value = sessions.auto_devour_command_snapshot(binding);
    return {value.revision, value.enabled, value.minions, value.initialized};
}
bool set(Sessions& sessions, const Sessions::Binding& binding, Type type, uint32_t enabled, uint32_t minions) {
    if (type == Type::game_auto_lingzhou_set) return sessions.set_game_auto_lingzhou(binding, enabled);
    if (type == Type::game_auto_glove_set) return sessions.set_game_auto_glove(binding, enabled);
    return type == Type::game_auto_fairy_set ? sessions.set_game_auto_fairy(binding, enabled) :
        sessions.set_game_auto_devour(binding, enabled, minions);
}
Packet request(Type type, uint32_t enabled, uint32_t minions = 1) {
    Packet packet(type);
    require(packet.add_number(enabled) && (type != Type::game_auto_devour_set || packet.add_number(minions)),
        "fixture encodes complete control");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t enabled, uint32_t minions = 1) {
    const auto length = type == Type::game_auto_devour_set ? 8u : 4u;
    require(packet.valid() && packet.type() == type && packet.length() == length, "response type and exact length");
    Reader reader{packet}; uint32_t actual_enabled = UINT32_MAX, actual_minions = UINT32_MAX;
    require(reader.number(actual_enabled) && actual_enabled == enabled, "response enabled value");
    if (type == Type::game_auto_devour_set)
        require(reader.number(actual_minions) && actual_minions == minions, "response minions value in second field");
    require(reader.done(), "response has no trailing fields");
}
void error(const Worker& worker) {
    require(worker.stream.sent.size() == 1, "one protocol error per rejected request");
    frame(worker.stream.sent.front(), Type::error, uint32_t(Error::protocol));
}
void lifecycle(Type type) {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, secondary);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto defaults = snapshot(sessions, secondary, type);
    require(!defaults.initialized && defaults.revision == 0 && defaults.enabled == 0 && defaults.minions == 1,
        "defaults preserve main off and devour child on");
    game.poll(); require(game.stream.sent.empty(), "no command before LA initialization");
    ui.receive({request(type, 0)}); game.poll();
    const auto first = snapshot(sessions, secondary, type);
    require(first.initialized && first.revision != 0 && game.stream.sent.size() == 1, "first off initializes and forwards");
    frame(game.stream.sent.back(), type, 0);
    require(!snapshot(sessions, primary, type).initialized, "primary cannot consume game snapshot");
    ui.receive({request(type, 0)}); game.poll();
    require(snapshot(sessions, secondary, type).revision == first.revision && game.stream.sent.size() == 1,
        "complete duplicate has no revision or extra packet");
    ui.receive({request(type, 1)}); game.stream.writable = false; game.poll();
    require(game.stream.sent.size() == 1, "failed forwarding preserves pending state");
    game.stream.writable = true; game.poll();
    require(game.stream.sent.size() == 2, "changed state forwards once after retry"); frame(game.stream.sent.back(), type, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "same account IP pair isolates commands");
    const auto enabled = snapshot(sessions, secondary, type);
    require(sessions.update_game(secondary, 42, 1, "Player") &&
        snapshot(sessions, secondary, type).revision == enabled.revision, "game status preserves controls");
    sessions.leave(secondary);
    ui.receive({request(type, 0, 0), request(type, 1, 0)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker new_game(sessions, replacement); new_game.poll(); new_game.poll();
    require(new_game.stream.sent.size() == 1, "reconnect receives latest complete value once");
    frame(new_game.stream.sent.back(), type, 1, 0);
    require(!snapshot(sessions, secondary, type).initialized, "old game binding cannot read replacement");
    game.stream.sent.clear(); game.poll(); require(game.stream.sent.empty(), "old game forwards nothing");
    sessions.leave(secondary); require(sessions.current(replacement), "late old disconnect preserves replacement");
    sessions.leave(primary); primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({request(type, 0)}); error(ui);
    Worker new_ui(sessions, primary); new_ui.receive({request(type, 0)}); new_game.poll();
    frame(new_game.stream.sent.back(), type, 0);
    sessions.leave(primary); sessions.leave(replacement);
    secondary = enter(sessions, "owner", 10, Role::secondary);
    require(!snapshot(sessions, secondary, type).initialized, "both endpoints leaving clears cache");
    primary = enter(sessions, "owner", 10, Role::primary);
    require(set(sessions, primary, type, 1, 0) && sessions.invalidate_account("owner"), "fixture revokes initialized account");
    require(!snapshot(sessions, secondary, type).initialized && !set(sessions, primary, type, 0, 1),
        "revoked bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation keeps other accounts");
}
void devour_combinations_and_independence() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    // Initialize before the game enters, including the disabled main and enabled child.
    ui.receive({request(Type::game_auto_fairy_set, 0), request(Type::game_auto_devour_set, 0, 1)});
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary); game.poll();
    require(game.stream.sent.size() == 2, "both controls initialize after first game connection");
    frame(game.stream.sent[0], Type::game_auto_fairy_set, 0);
    frame(game.stream.sent[1], Type::game_auto_devour_set, 0, 1);
    const auto fairy = sessions.auto_fairy_command_snapshot(secondary);
    require(sessions.set_game_auto_ignite(primary, 1), "initialize existing independent ignite");
    const auto ignite = sessions.auto_ignite_command_snapshot(secondary);
    uint64_t revision = sessions.auto_devour_command_snapshot(secondary).revision;
    for (uint32_t bits = 0; bits < 4; ++bits) {
        const auto enabled = (bits >> 1) & 1, minions = bits & 1;
        const auto count = game.stream.sent.size();
        ui.receive({request(Type::game_auto_devour_set, enabled, minions)}); game.poll();
        const auto value = sessions.auto_devour_command_snapshot(secondary);
        require(value.revision > revision && value.enabled == enabled && value.minions == minions &&
            game.stream.sent.size() == count + 1, "every devour combination advances complete snapshot including child-only change");
        frame(game.stream.sent.back(), Type::game_auto_devour_set, enabled, minions);
        revision = value.revision;
        ui.receive({request(Type::game_auto_devour_set, enabled, minions)}); game.poll();
        require(sessions.auto_devour_command_snapshot(secondary).revision == revision && game.stream.sent.size() == count + 1,
            "duplicate two-field snapshot remains unchanged");
    }
    require(sessions.auto_fairy_command_snapshot(secondary).revision == fairy.revision &&
        sessions.auto_ignite_command_snapshot(secondary).revision == ignite.revision, "devour changes leave fairy and ignite independent");
    ui.receive({request(Type::game_auto_fairy_set, 1)}); game.poll();
    require(sessions.auto_devour_command_snapshot(secondary).revision == revision, "fairy changes do not alter devour");
    frame(game.stream.sent.back(), Type::game_auto_fairy_set, 1);
    sessions.leave(secondary);
    ui.receive({request(Type::game_auto_devour_set, 1, 0)});
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary)); reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "reconnect initializes both separate controls");
    frame(reconnected.stream.sent[0], Type::game_auto_fairy_set, 1);
    frame(reconnected.stream.sent[1], Type::game_auto_devour_set, 1, 0);
}
void malformed_and_roles(Type type) {
    std::vector<Packet> invalid{request(type, 2), request(type, UINT32_MAX)};
    if (type == Type::game_auto_devour_set) {
        invalid.push_back(request(type, 0, 2)); invalid.push_back(request(type, 0, UINT32_MAX));
    }
    const auto valid = request(type, 0, 0);
    for (size_t length = 0; length <= valid.length() + 4; ++length) {
        if (length == valid.length()) continue;
        Packet packet(type);
        require(packet.add_bytes(valid.bytes.data() + 8, length), "fixture truncates or appends valid payload");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto secondary = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary); ui.receive({request(type, 1, 1)});
        const auto before = snapshot(sessions, secondary, type);
        ui.receive({packet, request(type, 0, 0)}); error(ui);
        const auto after = snapshot(sessions, secondary, type);
        require(after.enabled == 1 && after.minions == 1 && after.revision == before.revision,
            "invalid request is atomic and stops processing following frame");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary); game.receive({request(type, 1)}); error(game);
    require(!snapshot(sessions, secondary, type).initialized, "game cannot write primary-owned controls");
    require(!set(sessions, {}, type, 1, 1) && !set(sessions, primary, type, 2, 1), "setter validates binding and enabled");
    if (type == Type::game_auto_devour_set)
        require(!sessions.set_game_auto_devour(primary, 0, 2) && !sessions.set_game_auto_devour(primary, 0, UINT32_MAX),
            "setter rejects invalid minions before initializing any state");
    require(!snapshot(sessions, secondary, type).initialized, "rejected direct setters leave uninitialized state");
}
void lingzhou_independence() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({request(Type::game_auto_lingzhou_set, 1)});
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary); game.poll();
    require(game.stream.sent.size() == 1, "lingzhou queued before game login forwards alone");
    frame(game.stream.sent.back(), Type::game_auto_lingzhou_set, 1);
    const auto lingzhou = sessions.auto_lingzhou_command_snapshot(secondary);
    ui.receive({request(Type::game_auto_glove_set, 1)}); game.poll();
    require(game.stream.sent.size() == 2 && sessions.auto_lingzhou_command_snapshot(secondary).revision == lingzhou.revision,
        "glove changes do not resend or change lingzhou");
    const auto glove = sessions.auto_glove_command_snapshot(secondary);
    ui.receive({request(Type::game_auto_lingzhou_set, 0)}); game.poll();
    require(game.stream.sent.size() == 3 && sessions.auto_glove_command_snapshot(secondary).enabled == 1 &&
        sessions.auto_glove_command_snapshot(secondary).revision == glove.revision,
        "lingzhou disables independently of glove");
    frame(game.stream.sent.back(), Type::game_auto_lingzhou_set, 0);
}
}
int main() {
    try {
        static_assert(uint32_t(Type::game_auto_fairy_set) == 140 && uint32_t(Type::game_auto_devour_set) == 141 &&
            uint32_t(Type::game_auto_glove_set) == 142 && uint32_t(Type::game_auto_lingzhou_set) == 167);
        require(Packet(Type::game_auto_fairy_set).valid() && Packet(Type::game_auto_devour_set).valid() &&
            Packet(Type::game_movement_trace_set).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() &&
            !Packet(static_cast<Type>(0)).valid(), "supported wire types are valid and unknown types are rejected");
        for (const auto type : {Type::game_auto_fairy_set, Type::game_auto_devour_set,
            Type::game_auto_glove_set, Type::game_auto_lingzhou_set}) {
            lifecycle(type); malformed_and_roles(type);
        }
        devour_combinations_and_independence();
        lingzhou_independence();
        std::cout << "PASS: " << checks << " fairy/devour/glove/lingzhou server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
