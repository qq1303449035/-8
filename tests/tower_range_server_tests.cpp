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
#include "tower_range_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_tower_range_command_revision = UINT64_MAX;
    uint64_t sent_projectile_display_command_revision = UINT64_MAX;
    uint64_t sent_auto_dodge_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "tower_range_handler.inc"
        }
    }
    void poll() {
        do {
#include "tower_range_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t enabled, Type type = Type::game_tower_range_set) {
    Packet packet(type);
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
    require(sessions.tower_range_command_snapshot(own_game).enabled == 0, "tower range defaults off");
    require(sessions.set_game_movement_trace(primary, 1), "initialize independent movement trace");
    const auto movement = sessions.movement_trace_command_snapshot(own_game);
    ui.receive({request(0)});
    const auto initial = sessions.tower_range_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.enabled == 0, "first off value initializes command");
    require(!sessions.tower_range_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first off value");
    frame(game.stream.sent.back(), Type::game_tower_range_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.tower_range_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto enabled = sessions.tower_range_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > initial.revision, "enabling advances revision");
    require(sessions.movement_trace_command_snapshot(own_game).enabled == 1 &&
        sessions.movement_trace_command_snapshot(own_game).revision == movement.revision,
        "tower range preserves the independent movement trace setting");
    game.stream.writable = false;
    game.poll();
    require(game.sent_tower_range_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_tower_range_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate tower range commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.tower_range_command_snapshot(own_game).revision == enabled.revision, "status changes preserve tower range setting");

    sessions.leave(own_game);
    ui.receive({request(0), request(1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_tower_range_set, 1);
    require(!sessions.tower_range_command_snapshot(own_game).initialized, "old game token loses snapshot access");
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
    frame(reconnected.stream.sent.back(), Type::game_tower_range_set, 0);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.tower_range_command_snapshot(fresh).initialized, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_tower_range(primary, 1), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.tower_range_command_snapshot(fresh).initialized &&
        !sessions.set_game_tower_range(primary, 0), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.tower_range_command_snapshot(fresh_game).initialized, "new account session inherits no revoked setting");
}
Packet projectile_request(uint32_t enabled, uint32_t enemy = 1, uint32_t ally = 0) {
    Packet packet(Type::game_projectile_display_set);
    require(packet.add_number(enabled) && packet.add_number(enemy) && packet.add_number(ally),
        "fixture encodes complete projectile settings");
    return packet;
}
void projectile_frame(const Packet& packet, uint32_t enabled, uint32_t enemy = 1, uint32_t ally = 0) {
    require(packet.valid() && packet.type() == Type::game_projectile_display_set && packet.length() == 12,
        "valid twelve-byte projectile response frame");
    chh::wire::Reader reader{packet};
    uint32_t master = UINT32_MAX, enemy_value = UINT32_MAX, ally_value = UINT32_MAX;
    require(reader.number(master) && reader.number(enemy_value) && reader.number(ally_value) && reader.done() &&
        master == enabled && enemy_value == enemy && ally_value == ally,
        "response contains all projectile settings in wire order");
}
void projectile_display() {
    static_assert(uint32_t(Type::game_projectile_display_set) == 172);
    const auto projectile = Type::game_projectile_display_set;
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto defaults = sessions.projectile_display_command_snapshot(own_game);
    require(!defaults.initialized && defaults.enabled == 0 && defaults.enemy == 1 && defaults.ally == 0,
        "projectile display defaults off with enemy selected and ally disabled");
    ui.receive({request(1)});
    game.poll();
    game.stream.sent.clear();
    const auto tower = sessions.tower_range_command_snapshot(own_game);
    ui.receive({projectile_request(0)});
    const auto off = sessions.projectile_display_command_snapshot(own_game);
    require(off.initialized && off.enabled == 0 && off.enemy == 1 && off.ally == 0,
        "first projectile off initializes the full command");
    game.poll();
    projectile_frame(game.stream.sent.back(), 0);
    ui.receive({projectile_request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.projectile_display_command_snapshot(own_game).revision == off.revision,
        "duplicate projectile values do not resend");
    ui.receive({projectile_request(1)});
    const auto enabled = sessions.projectile_display_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > off.revision, "projectile enable advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_projectile_display_command_revision == off.revision, "failed projectile send remains pending");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "projectile update forwarded once");
    projectile_frame(game.stream.sent.back(), 1);
    for (uint32_t master : {0u, 1u}) {
        for (uint32_t enemy : {0u, 1u}) {
            for (uint32_t ally : {0u, 1u}) {
                const auto before = sessions.projectile_display_command_snapshot(own_game);
                const auto count = game.stream.sent.size();
                ui.receive({projectile_request(master, enemy, ally)});
                const auto changed = sessions.projectile_display_command_snapshot(own_game);
                require(changed.revision > before.revision && changed.enabled == master &&
                    changed.enemy == enemy && changed.ally == ally,
                    "each master/team combination is cached, including team options while master is off");
                game.poll();
                require(game.stream.sent.size() == count + 1, "team-only changes forward once");
                projectile_frame(game.stream.sent.back(), master, enemy, ally);
                ui.receive({projectile_request(master, enemy, ally)});
                game.poll();
                require(game.stream.sent.size() == count + 1 &&
                    sessions.projectile_display_command_snapshot(own_game).revision == changed.revision,
                    "projectile deduplication compares the complete configuration");
            }
        }
    }
    ui.receive({projectile_request(1, 0, 1)});
    game.poll();
    const auto selected = sessions.projectile_display_command_snapshot(own_game);
    require(sessions.tower_range_command_snapshot(own_game).revision == tower.revision &&
        sessions.tower_range_command_snapshot(own_game).enabled == 1, "projectile setting preserves tower range");
    require(sessions.update_game(own_game, 42, 1, "Player"), "projectile game status update accepted");
    const auto after_status = sessions.projectile_display_command_snapshot(own_game);
    require(after_status.revision == selected.revision && after_status.enabled == 1 && after_status.enemy == 0 && after_status.ally == 1,
        "game status updates preserve the complete projectile configuration");
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "projectile commands are account and IP isolated");
    std::vector<Packet> invalid;
    for (uint32_t value : {2u, UINT32_MAX}) {
        invalid.push_back(projectile_request(value, 1, 0));
        invalid.push_back(projectile_request(0, value, 0));
        invalid.push_back(projectile_request(0, 1, value));
        require(!sessions.set_game_projectile_display(primary, value, 1, 0) &&
            !sessions.set_game_projectile_display(primary, 0, value, 0) &&
            !sessions.set_game_projectile_display(primary, 0, 1, value),
            "projectile session API rejects non-boolean values in every field");
    }
    for (size_t length = 0; length <= 16; ++length) {
        if (length == 12) continue;
        Packet packet(projectile);
        const uint8_t bytes[16]{};
        require(packet.add_bytes(bytes, length), "encode malformed projectile payload, including retired four-byte format");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Worker invalid_ui(sessions, primary);
        invalid_ui.receive({packet, projectile_request(0)});
        error(invalid_ui);
        const auto unchanged = sessions.projectile_display_command_snapshot(own_game);
        require(unchanged.enabled == 1 && unchanged.enemy == 0 && unchanged.ally == 1 && unchanged.revision == selected.revision,
            "invalid projectile request preserves all cached values and stops dispatch");
    }
    Worker reverse(sessions, own_game);
    reverse.receive({projectile_request(0)});
    error(reverse);
    require(!sessions.projectile_display_command_snapshot(primary).initialized &&
        !sessions.set_game_projectile_display({}, 1), "projectile controls enforce endpoint roles and bindings");
    sessions.leave(own_game);
    ui.receive({projectile_request(0, 1, 1), projectile_request(1, 0, 1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "reconnect replays latest independent tower and projectile settings once");
    projectile_frame(reconnected.stream.sent.back(), 1, 0, 1);
    require(!sessions.projectile_display_command_snapshot(own_game).initialized, "stale game loses projectile access");
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh_primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    const auto cleared = sessions.projectile_display_command_snapshot(fresh_game);
    require(!cleared.initialized && cleared.enabled == 0 && cleared.enemy == 1 && cleared.ally == 0 &&
        !sessions.set_game_projectile_display(primary, 0), "pair removal resets projectile defaults and rejects stale writes");
    require(sessions.set_game_projectile_display(fresh_primary, 1), "initialize projectile before revocation");
    const auto local_default = sessions.projectile_display_command_snapshot(fresh_game);
    require(local_default.enabled == 1 && local_default.enemy == 1 && local_default.ally == 0,
        "local API default arguments preserve enemy-only selection");
    require(sessions.invalidate_account("owner") && !sessions.projectile_display_command_snapshot(fresh_game).initialized &&
        !sessions.set_game_projectile_display(fresh_primary, 0), "revocation invalidates projectile read and write bindings");
}
Packet dodge_request(uint32_t enabled, uint32_t rectangles = 1, uint32_t circles = 1) {
    Packet packet(Type::game_auto_dodge_set);
    require(packet.add_number(enabled) && packet.add_number(rectangles) && packet.add_number(circles),
        "fixture encodes complete dodge settings");
    return packet;
}
void dodge_frame(const Packet& packet, uint32_t enabled, uint32_t rectangles = 1, uint32_t circles = 1) {
    require(packet.valid() && packet.type() == Type::game_auto_dodge_set && packet.length() == 12,
        "valid twelve-byte dodge response frame");
    chh::wire::Reader reader{packet};
    uint32_t master = UINT32_MAX, rectangle = UINT32_MAX, circle = UINT32_MAX;
    require(reader.number(master) && reader.number(rectangle) && reader.number(circle) && reader.done() &&
        master == enabled && rectangle == rectangles && circle == circles, "response contains all dodge settings in wire order");
}
void auto_dodge() {
    static_assert(uint32_t(Type::game_auto_dodge_set) == 173);
    const auto dodge = Type::game_auto_dodge_set;
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto defaults = sessions.auto_dodge_command_snapshot(own_game);
    require(!defaults.initialized && defaults.enabled == 0 && defaults.rectangles == 1 && defaults.circles == 1,
        "auto dodge defaults off with both shape options selected");
    ui.receive({request(1), projectile_request(1, 0, 1)});
    game.poll();
    game.stream.sent.clear();
    const auto tower = sessions.tower_range_command_snapshot(own_game);
    const auto projectile = sessions.projectile_display_command_snapshot(own_game);
    ui.receive({dodge_request(0)});
    const auto off = sessions.auto_dodge_command_snapshot(own_game);
    require(off.initialized && off.enabled == 0 && off.rectangles == 1 && off.circles == 1,
        "first dodge off initializes the full command");
    game.poll();
    dodge_frame(game.stream.sent.back(), 0);
    ui.receive({dodge_request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.auto_dodge_command_snapshot(own_game).revision == off.revision,
        "duplicate dodge values do not resend");
    ui.receive({dodge_request(1)});
    const auto enabled = sessions.auto_dodge_command_snapshot(own_game);
    require(enabled.enabled == 1 && enabled.revision > off.revision, "dodge enable advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_auto_dodge_command_revision == off.revision, "failed dodge send remains pending");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "dodge update forwarded once");
    dodge_frame(game.stream.sent.back(), 1);
    for (uint32_t master : {0u, 1u}) {
        for (uint32_t rectangles : {0u, 1u}) {
            for (uint32_t circles : {0u, 1u}) {
                const auto before = sessions.auto_dodge_command_snapshot(own_game);
                const auto count = game.stream.sent.size();
                ui.receive({dodge_request(master, rectangles, circles)});
                const auto changed = sessions.auto_dodge_command_snapshot(own_game);
                require(changed.revision > before.revision && changed.enabled == master &&
                    changed.rectangles == rectangles && changed.circles == circles,
                    "each master/shape combination is cached, including options while master is off");
                game.poll();
                require(game.stream.sent.size() == count + 1, "shape-only changes forward once");
                dodge_frame(game.stream.sent.back(), master, rectangles, circles);
                ui.receive({dodge_request(master, rectangles, circles)});
                game.poll();
                require(game.stream.sent.size() == count + 1 &&
                    sessions.auto_dodge_command_snapshot(own_game).revision == changed.revision,
                    "deduplication compares the complete configuration");
            }
        }
    }
    ui.receive({dodge_request(1, 0, 0)});
    game.poll();
    const auto selected = sessions.auto_dodge_command_snapshot(own_game);
    require(sessions.tower_range_command_snapshot(own_game).revision == tower.revision &&
        sessions.tower_range_command_snapshot(own_game).enabled == 1, "dodge setting preserves tower range");
    require(sessions.projectile_display_command_snapshot(own_game).revision == projectile.revision &&
        sessions.projectile_display_command_snapshot(own_game).enabled == 1 &&
        sessions.projectile_display_command_snapshot(own_game).enemy == 0 &&
        sessions.projectile_display_command_snapshot(own_game).ally == 1, "auto dodge preserves all projectile settings");
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "dodge commands are account and IP isolated");
    std::vector<Packet> invalid;
    for (uint32_t value : {2u, UINT32_MAX}) {
        invalid.push_back(dodge_request(value, 1, 1));
        invalid.push_back(dodge_request(0, value, 1));
        invalid.push_back(dodge_request(0, 1, value));
        require(!sessions.set_game_auto_dodge(primary, value, 1, 1) &&
            !sessions.set_game_auto_dodge(primary, 0, value, 1) &&
            !sessions.set_game_auto_dodge(primary, 0, 1, value), "session API rejects non-boolean values in every field");
    }
    for (size_t length = 0; length <= 20; ++length) {
        if (length == 12) continue;
        Packet packet(dodge);
        const uint8_t bytes[20]{};
        require(packet.add_bytes(bytes, length), "encode malformed dodge payload, including retired four- and sixteen-byte formats");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Worker invalid_ui(sessions, primary);
        invalid_ui.receive({packet, dodge_request(0)});
        error(invalid_ui);
        const auto unchanged = sessions.auto_dodge_command_snapshot(own_game);
        require(unchanged.enabled == 1 && unchanged.rectangles == 0 && unchanged.circles == 0 &&
            unchanged.revision == selected.revision, "invalid dodge request preserves all cached values and stops dispatch");
    }
    Worker reverse(sessions, own_game);
    reverse.receive({dodge_request(0)});
    error(reverse);
    require(!sessions.auto_dodge_command_snapshot(primary).initialized &&
        !sessions.set_game_auto_dodge({}, 1, 1, 1), "dodge controls enforce endpoint roles and bindings");
    sessions.leave(own_game);
    ui.receive({dodge_request(0, 1, 0), dodge_request(1, 0, 0)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 3, "reconnect replays latest independent tower, projectile and dodge settings once");
    dodge_frame(reconnected.stream.sent.back(), 1, 0, 0);
    require(!sessions.auto_dodge_command_snapshot(own_game).initialized, "stale game loses dodge access");
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh_primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    const auto cleared = sessions.auto_dodge_command_snapshot(fresh_game);
    require(!cleared.initialized && cleared.enabled == 0 && cleared.rectangles == 1 && cleared.circles == 1 &&
        !sessions.set_game_auto_dodge(primary, 0, 1, 1), "pair removal resets all dodge settings and rejects stale writes");
    require(sessions.set_game_auto_dodge(fresh_primary, 1, 0, 0), "initialize dodge before revocation");
    require(sessions.invalidate_account("owner") && !sessions.auto_dodge_command_snapshot(fresh_game).initialized &&
        !sessions.set_game_auto_dodge(fresh_primary, 0, 1, 1), "revocation invalidates dodge read and write bindings");
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_tower_range_set) == 171);
    require(request(0).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "feature packet type is supported and unknown types are rejected");
    std::vector<Packet> invalid{request(2), request(UINT32_MAX)};
    for (size_t length = 0; length <= 8; ++length) {
        if (length == 4) continue;
        Packet packet(Type::game_tower_range_set);
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
        const auto revision = sessions.tower_range_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto command = sessions.tower_range_command_snapshot(game);
        require(command.enabled == 1 && command.revision == revision, "malformed request keeps setting and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(1)});
    error(endpoint);
    require(!sessions.tower_range_command_snapshot(game).initialized, "secondary cannot initialize controls");
    require(!sessions.set_game_tower_range({}, 1) && !sessions.set_game_tower_range(primary, 2),
        "session API also rejects empty binding and non-boolean values");
    require(sessions.set_game_tower_range(primary, 0), "queue initial off for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "off initialization survives game connection reset");
    frame(late_game.stream.sent.back(), Type::game_tower_range_set, 0);
}
}
int main() {
    try {
        routing_and_reconnect();
        malformed_and_roles();
        projectile_display();
        auto_dodge();
        std::cout << "PASS: " << checks << " tower range/projectile display/auto dodge server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
