// Uses the real server receive and send branches; TLS is replaced by an offline sink.
#include "../network/protocol.h"
#include "../sessions.h"
#include <functional>
#include <iostream>
#include <stdexcept>

namespace chh::tls {
struct Stream {
    bool writable = true;
    std::vector<wire::Packet> sent;
    std::function<void(const wire::Packet&)> on_write;
    bool write_all(const void* bytes, size_t size, int) {
        if (!writable || size != wire::packet_size) return false;
        wire::Packet packet;
        std::memcpy(packet.bytes.data(), bytes, size);
        sent.push_back(packet);
        if (on_write) on_write(packet);
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
#include "auto_heal_roster_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_game_revision = UINT64_MAX;
    uint64_t sent_auto_heal_command_revision = UINT64_MAX;
    uint64_t sent_auto_heal_roster_revision = UINT64_MAX;
    uint64_t sent_auto_heal_exclusions_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "auto_heal_roster_handler.inc"
        }
    }
    void poll() {
        do {
            if (member.binding.role == Role::primary) {
#include "auto_heal_roster_primary.inc"
            } else {
#include "auto_heal_roster_game.inc"
            }
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, std::string account = "owner", uint32_t ip = 10,
    Role role = Role::primary) {
    const auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters current session");
    return result;
}
GameAutoDisarmRoster roster(uint64_t match = 100) {
    return {match, {{10, 1}, {11, 2}, {12, 2}, {13, 65535}}, {2}};
}
Packet roster_packet(const GameAutoDisarmRoster& value) {
    Packet packet;
    require(make_game_auto_heal_roster(value, packet), "fixture encodes roster");
    return packet;
}
Packet command(uint64_t match, uint64_t request, std::vector<uint32_t> heroes) {
    Packet packet;
    require(make_game_auto_heal_exclusions({match, request, std::move(heroes)}, packet), "fixture encodes exclusions");
    return packet;
}
GameAutoDisarmExclusions exclusions(const Packet& packet) {
    GameAutoDisarmExclusions value;
    require(read_game_auto_heal_exclusions(packet, value), "valid exclusions response");
    return value;
}
void confirm(const Packet& packet, uint64_t match, uint64_t request, std::vector<uint32_t> heroes) {
    const auto value = exclusions(packet);
    require(value.match_id == match && value.request_id == request && value.hero_ids == heroes,
        "response preserves match, request and complete exclusions");
}
void protocol_error(const Worker& worker) {
    require(worker.stream.sent.size() == 1 && worker.stream.sent.front().type() == Type::error &&
        worker.stream.sent.front().length() == 4 &&
        worker.stream.sent.front().number() == uint32_t(Error::protocol), "invalid packet gets one protocol error");
}
void initialization_and_routing() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::primary));
    Worker other_game(sessions, enter(sessions, "other", 10, Role::secondary));
    ui.poll(); game.poll();
    require(ui.stream.sent.size() == 1 && ui.stream.sent[0].type() == Type::game_status && game.stream.sent.empty(),
        "no roster or command before current game initialization");
    ui.stream.sent.clear();
    game.receive({roster_packet(roster())});
    ui.poll(); game.poll(); other_ip.poll(); other_account.poll(); other_game.poll();
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_auto_heal_roster,
        "primary receives roster followed by exclusions");
    GameAutoDisarmRoster received;
    require(read_game_auto_heal_roster(ui.stream.sent[0], received) && received.players.size() == 4 &&
        received.excluded_hero_ids == std::vector<uint32_t>{2}, "all UIDs retained including duplicate hero");
    confirm(ui.stream.sent[1], 100, 0, {2});
    require(game.stream.sent.size() == 1, "game receives its initial exclusions once");
    confirm(game.stream.sent.back(), 100, 0, {2});
    require(other_ip.stream.sent.empty() && other_game.stream.sent.empty() && other_account.stream.sent.size() == 1,
        "account and IP pairs isolate dynamic roster");
    ui.receive({command(100, 1, {65535, 1, 2})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    confirm(game.stream.sent.back(), 100, 1, {1, 2, 65535});
    const auto before = sessions.auto_heal_roster_snapshot(game.member.binding);
    game.receive({roster_packet(roster())}); game.poll(); ui.poll();
    require(sessions.auto_heal_roster_snapshot(game.member.binding).exclusions_revision == before.exclusions_revision &&
        game.stream.sent.size() == 2, "same-match game report cannot overwrite cached exclusions");
    ui.receive({command(100, 1, {2, 65535, 1})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    require(game.stream.sent.size() == 2, "identical request is acknowledged without game duplicate");
    ui.receive({command(100, 2, {1})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 2, {1});
    ui.stream.sent.clear();
    ui.receive({command(100, 1, {2})}); ui.poll();
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_auto_heal_roster,
        "stale request refreshes current roster and confirmation");
    confirm(ui.stream.sent.back(), 100, 2, {1});
    ui.receive({command(100, 3, {})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 3, {});
}
void pruning_matches_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions);
    auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary), game(sessions, secondary);
    game.receive({roster_packet(roster())});
    ui.receive({command(100, 7, {1, 2})}); ui.poll(); game.poll();
    auto next = roster(); next.players.erase(next.players.begin() + 1);
    const auto before = sessions.auto_heal_roster_snapshot(secondary);
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    auto after = sessions.auto_heal_roster_snapshot(secondary);
    require(after.roster.players.size() == 3 && after.exclusions.hero_ids == std::vector<uint32_t>({1, 2}) &&
        after.exclusions_revision == before.exclusions_revision, "deleting one duplicate-hero UID keeps hero exclusion");
    next.players.erase(next.players.begin() + 1); next.excluded_hero_ids.clear();
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    after = sessions.auto_heal_roster_snapshot(secondary);
    require(after.exclusions_revision > before.exclusions_revision, "last hero UID deletion advances exclusion revision");
    confirm(game.stream.sent.back(), 100, 7, {1});
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 7, {1, 2})}); ui.poll();
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 8, {2, 65535})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});

    sessions.leave(secondary);
    require(!sessions.auto_heal_roster_snapshot(primary).ready, "disconnect hides retained old roster");
    secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, secondary);
    ui.stream.sent.clear(); ui.receive({command(100, 9, {1})}); ui.poll(); reconnected.poll();
    require(ui.stream.sent.size() == 1 && ui.stream.sent[0].type() == Type::game_status && reconnected.stream.sent.empty(),
        "no stale dynamic initialization before reconnect roster");
    reconnected.receive({roster_packet(roster())}); ui.poll(); reconnected.poll();
    confirm(reconnected.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});
    require(!sessions.auto_heal_roster_snapshot(game.member.binding).ready &&
        !sessions.update_game_auto_heal_roster(game.member.binding, roster(900)), "old secondary cannot read or change replacement");
    sessions.leave(game.member.binding);
    require(sessions.current(secondary), "late old disconnect cannot reset replacement");
    sessions.leave(primary); primary = enter(sessions);
    require(sessions.set_game_auto_heal_exclusions(ui.member.binding, {100, 99, {1}}) ==
        Sessions::AutoHealExclusionsResult::invalid, "replaced primary loses write access");
    Worker new_ui(sessions, primary); new_ui.poll();
    require(new_ui.stream.sent.size() == 3 && new_ui.stream.sent[0].type() == Type::game_status &&
        new_ui.stream.sent[1].type() == Type::game_auto_heal_roster, "primary reconnect initializes status then roster then confirmation");
    confirm(new_ui.stream.sent.back(), 100, 8, {65535});
    new_ui.receive({command(100, 9, {1, 2})}); new_ui.poll(); reconnected.poll();
    confirm(reconnected.stream.sent.back(), 100, 9, {1, 2});
    reconnected.receive({roster_packet(roster(101))}); new_ui.poll(); reconnected.poll();
    confirm(new_ui.stream.sent.back(), 101, 0, {2});
    confirm(reconnected.stream.sent.back(), 101, 0, {2});
    new_ui.stream.sent.clear(); new_ui.receive({command(100, 99, {1})}); new_ui.poll();
    require(new_ui.stream.sent.size() == 2, "old match rejected without disconnecting primary");
    confirm(new_ui.stream.sent.back(), 101, 0, {2});
    new_ui.receive({command(101, 1, {1})}); new_ui.poll(); reconnected.poll();
    confirm(reconnected.stream.sent.back(), 101, 1, {1});
    sessions.leave(primary); sessions.leave(secondary);
    primary = enter(sessions); secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker fresh_game(sessions, secondary); fresh_game.receive({roster_packet(roster(101))});
    confirm(command(101, sessions.auto_heal_roster_snapshot(primary).exclusions.request_id,
        sessions.auto_heal_roster_snapshot(primary).exclusions.hero_ids), 101, 0, {2});
}
void snapshot_and_write_races() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.receive({roster_packet(roster())});
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() == Type::game_auto_heal_roster)
            require(sessions.set_game_auto_heal_exclusions(ui.member.binding, {100, 1, {1}}) ==
                Sessions::AutoHealExclusionsResult::applied, "simulate concurrent state update between response frames");
    };
    ui.poll();
    confirm(ui.stream.sent.back(), 100, 0, {2});
    ui.stream.on_write = {}; ui.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1});
    game.stream.writable = false; game.poll();
    require(game.sent_auto_heal_exclusions_revision == UINT64_MAX, "failed game send remains pending");
    game.stream.writable = true; game.poll(); confirm(game.stream.sent.back(), 100, 1, {1});
    ui.sent_auto_heal_roster_revision = UINT64_MAX; ui.stream.sent.clear();
    ui.stream.writable = false; ui.poll();
    require(ui.sent_auto_heal_roster_revision == UINT64_MAX, "failed roster send remains pending");
    ui.stream.writable = true; ui.poll();
    require(ui.stream.sent.size() == 2, "roster retry still sends both coherent frames");
    confirm(ui.stream.sent.back(), 100, 1, {1});

    ui.sent_game_revision = UINT64_MAX; ui.stream.sent.clear();
    Sessions::Binding replacement;
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() != Type::game_status) return;
        sessions.leave(game.member.binding);
        replacement = enter(sessions, "owner", 10, Role::secondary);
        require(sessions.update_game_auto_heal_roster(replacement, roster(102)), "replacement initialized during status write");
    };
    ui.poll();
    require(ui.stream.sent.size() == 1, "new token roster cannot follow stale status from another game connection");
    ui.stream.on_write = {}; ui.poll();
    require(ui.stream.sent.size() == 4 && ui.stream.sent[1].type() == Type::game_status &&
        ui.stream.sent[2].type() == Type::game_auto_heal_roster, "next poll sends new token status before new roster");
    confirm(ui.stream.sent.back(), 102, 0, {2});
}
void malformed_and_roles() {
    require(Packet(Type::game_auto_heal_roster).valid() && Packet(Type::game_auto_heal_exclusions_set).valid() &&
        Packet(Type::game_movement_trace_set).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid(),
        "supported wire types are valid and unknown types are rejected");
    std::vector<Packet> invalid_rosters, invalid_commands;
    const auto valid_roster = roster_packet(roster());
    const auto valid_command = command(100, 10, {1, 2});
    for (size_t length = 0; length <= valid_roster.length() + 4; ++length) {
        if (length == valid_roster.length()) continue;
        Packet packet(Type::game_auto_heal_roster);
        require(packet.add_bytes(valid_roster.bytes.data() + 8, length), "create malformed roster length");
        invalid_rosters.push_back(packet);
    }
    for (size_t length = 0; length <= valid_command.length() + 4; ++length) {
        if (length == valid_command.length()) continue;
        Packet packet(Type::game_auto_heal_exclusions_set);
        require(packet.add_bytes(valid_command.bytes.data() + 8, length), "create malformed exclusions length");
        invalid_commands.push_back(packet);
    }
    // Offsets are payload-relative: match, player count, UID/hero pairs, exclusion count and IDs.
    for (const auto field : std::vector<std::pair<size_t, uint32_t>>{
        {4, 0}, {8, 15}, {8, UINT32_MAX}, {12, 0}, {16, 65536}, {16, UINT32_MAX},
        {20, 10}, {44, 15}, {44, UINT32_MAX}, {48, 0}, {48, 3}}) {
        auto packet = valid_roster; write32(packet.bytes.data() + 8 + field.first, field.second);
        invalid_rosters.push_back(packet);
    }
    for (const auto field : std::vector<std::pair<size_t, uint32_t>>{
        {4, 0}, {12, 0}, {16, 15}, {16, UINT32_MAX}, {20, 65536}, {20, UINT32_MAX}, {24, 1}}) {
        auto packet = valid_command; write32(packet.bytes.data() + 8 + field.first, field.second);
        invalid_commands.push_back(packet);
    }
    auto duplicate_excluded = valid_roster;
    write32(duplicate_excluded.bytes.data() + 8 + 44, 2);
    require(duplicate_excluded.add_number(2), "fixture appends duplicate roster exclusion");
    invalid_rosters.push_back(duplicate_excluded);
    for (const bool primary : {false, true}) {
        for (const auto& packet : primary ? invalid_commands : invalid_rosters) {
            Sessions sessions;
            Worker ui(sessions, enter(sessions));
            Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
            game.receive({valid_roster}); ui.receive({command(100, 1, {1})});
            const auto before = sessions.auto_heal_roster_snapshot(ui.member.binding);
            auto& source = primary ? ui : game;
            source.receive({packet, primary ? command(100, 11, {2}) : roster_packet(roster(200))});
            protocol_error(source);
            const auto after = sessions.auto_heal_roster_snapshot(ui.member.binding);
            require(after.roster_revision == before.roster_revision && after.exclusions_revision == before.exclusions_revision &&
                after.exclusions.match_id == 100 && after.exclusions.request_id == 1 && after.exclusions.hero_ids == std::vector<uint32_t>{1},
                "malformed request is atomic and terminates dispatch before next frame");
        }
    }
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({valid_roster}); protocol_error(ui);
    game.receive({valid_command}); protocol_error(game);
    require(!sessions.auto_heal_roster_snapshot(ui.member.binding).ready, "wrong roles cannot initialize roster");
    require(!sessions.update_game_auto_heal_roster({}, roster()) &&
        sessions.set_game_auto_heal_exclusions({}, {100, 1, {1}}) == Sessions::AutoHealExclusionsResult::invalid,
        "empty binding cannot read or write dynamic state");
    game.stream.sent.clear(); game.receive({valid_roster});
    ui.stream.sent.clear(); ui.receive({command(100, 1, {1}), command(100, 1, {2})});
    protocol_error(ui);
    require(sessions.auto_heal_roster_snapshot(game.member.binding).exclusions.hero_ids == std::vector<uint32_t>{1},
        "same request number with different set cannot change accepted state");
    require(sessions.invalidate_account("owner") && !sessions.auto_heal_roster_snapshot(game.member.binding).ready,
        "account invalidation discards access and dynamic state");
}
void limits_and_empty_roster() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    GameAutoDisarmRoster maximum{UINT64_MAX, {}, {}};
    for (uint32_t i = 1; i <= game_auto_disarm_player_limit; ++i) {
        maximum.players.push_back({i, i}); maximum.excluded_hero_ids.push_back(i);
    }
    game.receive({roster_packet(maximum)}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), UINT64_MAX, 0, maximum.excluded_hero_ids);
    ui.receive({command(UINT64_MAX, UINT64_MAX, maximum.excluded_hero_ids)}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), UINT64_MAX, UINT64_MAX, maximum.excluded_hero_ids);
    maximum.players.clear(); maximum.excluded_hero_ids.clear();
    game.receive({roster_packet(maximum)}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), UINT64_MAX, UINT64_MAX, {});
    const auto snapshot = sessions.auto_heal_roster_snapshot(ui.member.binding);
    require(snapshot.ready && snapshot.roster.players.empty(), "valid empty roster clears all dynamic controls");
}
Packet config(uint32_t enabled = 0, uint32_t health = 80) {
    Packet packet(Type::game_auto_heal_set);
    require(packet.add_number(enabled) && packet.add_number(health), "encode heal config");
    return packet;
}
void configuration_and_reconnect() {
    static_assert(uint32_t(Type::game_auto_heal_set) == 153 && uint32_t(Type::game_auto_heal_roster) == 154 &&
        uint32_t(Type::game_auto_heal_exclusions_set) == 155);
    require(game_auto_heal_health_min == 10 && game_auto_heal_health_max == 100 && game_auto_heal_health_default == 80,
        "healing threshold constants are 10..100 with default 80");
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto initial = sessions.auto_heal_command_snapshot(game.member.binding);
    require(!initial.initialized && initial.enabled == 0 && initial.health_percent == 80, "healing initially disabled at 80 percent");
    game.poll(); require(game.stream.sent.empty(), "no unsolicited initialization before LA configuration");
    for (uint32_t enabled : {0u, 1u}) for (uint32_t health : {80u, 10u, 37u, 100u}) {
        const auto before = game.stream.sent.size();
        ui.receive({config(enabled, health)}); game.poll();
        require(game.stream.sent.size() == before + 1 && game.stream.sent.back().bytes == config(enabled, health).bytes,
            "real handler forwards complete on/off card and health threshold");
        const auto snapshot = sessions.auto_heal_command_snapshot(game.member.binding);
        require(snapshot.enabled == enabled && snapshot.health_percent == health && snapshot.initialized,
            "session retains exact healing configuration");
        ui.receive({config(enabled, health)}); game.poll();
        require(game.stream.sent.size() == before + 1 && sessions.auto_heal_command_snapshot(game.member.binding).revision == snapshot.revision,
            "unchanged healing config is deduplicated");
    }
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "config stays within paired account and IP");
    ui.receive({config(0, 37)});
    const auto revision = game.sent_auto_heal_command_revision;
    game.stream.writable = false; game.poll();
    require(game.sent_auto_heal_command_revision == revision, "failed config send remains pending");
    game.stream.writable = true; game.poll();
    require(game.stream.sent.back().bytes == config(0, 37).bytes, "retry sends latest disabled threshold");
    sessions.leave(game.member.binding);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 1 && replacement.stream.sent.back().bytes == config(0, 37).bytes,
        "game reconnect restores complete config exactly once");
    game.stream.sent.clear(); game.poll(); require(game.stream.sent.empty(), "stale game token receives no config");
    replacement.stream.sent.clear(); replacement.receive({config(1, 100)}); protocol_error(replacement);
    require(sessions.auto_heal_command_snapshot(replacement.member.binding).health_percent == 37,
        "game endpoint cannot change healing configuration");
    sessions.leave(ui.member.binding); ui.receive({config(1, 100)}); protocol_error(ui);
}
void invalid_configs_are_atomic() {
    std::vector<Packet> invalid{config(2, 80), config(UINT32_MAX, 80), config(1, 0), config(1, 9),
        config(1, 101), config(1, UINT32_MAX)};
    const auto valid = config();
    for (size_t length = 0; length <= 12; ++length) {
        if (length == 8) continue;
        Packet packet(Type::game_auto_heal_set);
        require(packet.add_bytes(valid.bytes.data() + 8, length), "encode malformed heal payload");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        Worker ui(sessions, enter(sessions));
        Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
        ui.receive({config(1, 37)});
        const auto before = sessions.auto_heal_command_snapshot(game.member.binding).revision;
        ui.receive({packet, config()}); protocol_error(ui); game.poll();
        require(sessions.auto_heal_command_snapshot(game.member.binding).revision == before &&
            game.stream.sent.size() == 1 && game.stream.sent[0].bytes == config(1, 37).bytes,
            "invalid config preserves complete card and stops following frames");
    }
}
void allied_roster_and_feature_isolation() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    require(sessions.update_game(game.member.binding, 10, 4, "Self"), "identify own UID before allied roster");
    game.receive({roster_packet(roster())});
    const auto allies = sessions.auto_heal_roster_snapshot(ui.member.binding);
    require(allies.ready && allies.roster.players[0].uid == 10, "own player stays eligible in allied roster");
    require(sessions.update_game_auto_disarm_roster(game.member.binding, roster()) &&
        sessions.update_game_near_item_roster(game.member.binding, 6, roster()), "initialize independent enemy feature rosters");
    require(sessions.set_game_auto_disarm_exclusions(ui.member.binding, {100, 1, {65535}}) == Sessions::AutoDisarmExclusionsResult::applied &&
        sessions.set_game_near_item_exclusions(ui.member.binding, 6, {100, 1, {2}}) == Sessions::NearItemExclusionsResult::applied,
        "set independent enemy exclusions");
    ui.receive({command(100, 1, {1})});
    require(sessions.auto_heal_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{1} &&
        sessions.auto_disarm_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{65535} &&
        sessions.near_item_roster_snapshot(ui.member.binding, 6).exclusions.hero_ids == std::vector<uint32_t>{2},
        "healing can exclude self without altering disarm or any near-item exclusion");
    Packet disarm_packet, near_packet;
    require(make_game_auto_disarm_roster(roster(), disarm_packet) && make_game_near_item_roster(6, roster(), near_packet), "encode foreign feature types");
    GameAutoDisarmRoster decoded;
    require(!read_game_auto_heal_roster(disarm_packet, decoded) && !read_game_auto_heal_roster(near_packet, decoded) &&
        !read_game_auto_disarm_roster(roster_packet(roster()), decoded), "allied roster parser remains isolated by message type");
    auto next_match = roster(101); next_match.excluded_hero_ids.clear();
    game.receive({roster_packet(next_match)});
    require(sessions.auto_heal_roster_snapshot(ui.member.binding).exclusions.hero_ids.empty() &&
        sessions.auto_heal_roster_snapshot(ui.member.binding).exclusions.request_id == 0 &&
        sessions.near_item_roster_snapshot(ui.member.binding, 6).exclusions.hero_ids == std::vector<uint32_t>{2},
        "new allied match clears healing exclusions and request id without resetting other features");
}
}
int main() {
    try {
        configuration_and_reconnect();
        invalid_configs_are_atomic();
        allied_roster_and_feature_isolation();
        initialization_and_routing();
        pruning_matches_and_reconnect();
        snapshot_and_write_races();
        malformed_and_roles();
        limits_and_empty_roster();
        std::cout << "PASS: " << checks << " auto heal server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
