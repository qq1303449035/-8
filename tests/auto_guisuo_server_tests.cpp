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
uint32_t test_side = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "auto_guisuo_roster_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_game_revision = UINT64_MAX;
    uint64_t sent_auto_guisuo_command_revision = UINT64_MAX;
    std::array<uint64_t, game_auto_guisuo_side_count> sent_auto_guisuo_roster_revisions{UINT64_MAX, UINT64_MAX};
    std::array<uint64_t, game_auto_guisuo_side_count> sent_auto_guisuo_exclusions_revisions{UINT64_MAX, UINT64_MAX};
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "auto_guisuo_roster_handler.inc"
        }
    }
    void poll() {
        do {
            if (member.binding.role == Role::primary) {
#include "auto_guisuo_roster_primary.inc"
            } else {
#include "auto_guisuo_roster_game.inc"
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
    require(make_game_auto_guisuo_roster(test_side, value, packet), "fixture encodes roster");
    return packet;
}
Packet command(uint64_t match, uint64_t request, std::vector<uint32_t> heroes) {
    Packet packet;
    require(make_game_auto_guisuo_exclusions(test_side, {match, request, std::move(heroes)}, packet), "fixture encodes exclusions");
    return packet;
}
bool read_test_roster(const Packet& packet, GameAutoDisarmRoster& value) {
    uint32_t side = UINT32_MAX;
    return read_game_auto_guisuo_roster(packet, side, value) && side == test_side;
}
bool read_test_exclusions(const Packet& packet, GameAutoDisarmExclusions& value) {
    uint32_t side = UINT32_MAX;
    return read_game_auto_guisuo_exclusions(packet, side, value) && side == test_side;
}
GameAutoDisarmExclusions exclusions(const Packet& packet) {
    GameAutoDisarmExclusions value;
    require(read_test_exclusions(packet, value), "valid exclusions response");
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
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_auto_guisuo_roster,
        "primary receives roster followed by exclusions");
    GameAutoDisarmRoster received;
    require(read_test_roster(ui.stream.sent[0], received) && received.players.size() == 4 &&
        received.excluded_hero_ids == std::vector<uint32_t>{2}, "all UIDs retained including duplicate hero");
    confirm(ui.stream.sent[1], 100, 0, {2});
    require(game.stream.sent.size() == 1, "game receives its initial exclusions once");
    confirm(game.stream.sent.back(), 100, 0, {2});
    require(other_ip.stream.sent.empty() && other_game.stream.sent.empty() && other_account.stream.sent.size() == 1,
        "account and IP pairs isolate dynamic roster");
    ui.receive({command(100, 1, {65535, 1, 2})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    confirm(game.stream.sent.back(), 100, 1, {1, 2, 65535});
    const auto before = sessions.auto_guisuo_roster_snapshot(game.member.binding, test_side);
    game.receive({roster_packet(roster())}); game.poll(); ui.poll();
    require(sessions.auto_guisuo_roster_snapshot(game.member.binding, test_side).exclusions_revision == before.exclusions_revision &&
        game.stream.sent.size() == 2, "same-match game report cannot overwrite cached exclusions");
    ui.receive({command(100, 1, {2, 65535, 1})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    require(game.stream.sent.size() == 2, "identical request is acknowledged without game duplicate");
    ui.receive({command(100, 2, {1})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 2, {1});
    ui.stream.sent.clear();
    ui.receive({command(100, 1, {2})}); ui.poll();
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_auto_guisuo_roster,
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
    const auto before = sessions.auto_guisuo_roster_snapshot(secondary, test_side);
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    auto after = sessions.auto_guisuo_roster_snapshot(secondary, test_side);
    require(after.roster.players.size() == 3 && after.exclusions.hero_ids == std::vector<uint32_t>({1, 2}) &&
        after.exclusions_revision == before.exclusions_revision, "deleting one duplicate-hero UID keeps hero exclusion");
    next.players.erase(next.players.begin() + 1); next.excluded_hero_ids.clear();
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    after = sessions.auto_guisuo_roster_snapshot(secondary, test_side);
    require(after.exclusions_revision > before.exclusions_revision, "last hero UID deletion advances exclusion revision");
    confirm(game.stream.sent.back(), 100, 7, {1});
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 7, {1, 2})}); ui.poll();
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 8, {2, 65535})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});

    sessions.leave(secondary);
    require(!sessions.auto_guisuo_roster_snapshot(primary, test_side).ready, "disconnect hides retained old roster");
    secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, secondary);
    ui.stream.sent.clear(); ui.receive({command(100, 9, {1})}); ui.poll(); reconnected.poll();
    require(ui.stream.sent.size() == 1 && ui.stream.sent[0].type() == Type::game_status && reconnected.stream.sent.empty(),
        "no stale dynamic initialization before reconnect roster");
    reconnected.receive({roster_packet(roster())}); ui.poll(); reconnected.poll();
    confirm(reconnected.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});
    require(!sessions.auto_guisuo_roster_snapshot(game.member.binding, test_side).ready &&
        !sessions.update_game_auto_guisuo_roster(game.member.binding, test_side, roster(900)), "old secondary cannot read or change replacement");
    sessions.leave(game.member.binding);
    require(sessions.current(secondary), "late old disconnect cannot reset replacement");
    sessions.leave(primary); primary = enter(sessions);
    require(sessions.set_game_auto_guisuo_exclusions(ui.member.binding, test_side, {100, 99, {1}}) ==
        Sessions::AutoGuisuoExclusionsResult::invalid, "replaced primary loses write access");
    Worker new_ui(sessions, primary); new_ui.poll();
    require(new_ui.stream.sent.size() == 3 && new_ui.stream.sent[0].type() == Type::game_status &&
        new_ui.stream.sent[1].type() == Type::game_auto_guisuo_roster, "primary reconnect initializes status then roster then confirmation");
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
    confirm(command(101, sessions.auto_guisuo_roster_snapshot(primary, test_side).exclusions.request_id,
        sessions.auto_guisuo_roster_snapshot(primary, test_side).exclusions.hero_ids), 101, 0, {2});
}
void snapshot_and_write_races() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.receive({roster_packet(roster())});
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() == Type::game_auto_guisuo_roster)
            require(sessions.set_game_auto_guisuo_exclusions(ui.member.binding, test_side, {100, 1, {1}}) ==
                Sessions::AutoGuisuoExclusionsResult::applied, "simulate concurrent state update between response frames");
    };
    ui.poll();
    confirm(ui.stream.sent.back(), 100, 0, {2});
    ui.stream.on_write = {}; ui.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1});
    game.stream.writable = false; game.poll();
    require(game.sent_auto_guisuo_exclusions_revisions[test_side] == UINT64_MAX, "failed game send remains pending");
    game.stream.writable = true; game.poll(); confirm(game.stream.sent.back(), 100, 1, {1});
    ui.sent_auto_guisuo_roster_revisions[test_side] = UINT64_MAX; ui.stream.sent.clear();
    ui.stream.writable = false; ui.poll();
    require(ui.sent_auto_guisuo_roster_revisions[test_side] == UINT64_MAX, "failed roster send remains pending");
    ui.stream.writable = true; ui.poll();
    require(ui.stream.sent.size() == 2, "roster retry still sends both coherent frames");
    confirm(ui.stream.sent.back(), 100, 1, {1});

    ui.sent_game_revision = UINT64_MAX; ui.stream.sent.clear();
    Sessions::Binding replacement;
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() != Type::game_status) return;
        sessions.leave(game.member.binding);
        replacement = enter(sessions, "owner", 10, Role::secondary);
        require(sessions.update_game_auto_guisuo_roster(replacement, test_side, roster(102)), "replacement initialized during status write");
    };
    ui.poll();
    require(ui.stream.sent.size() == 1, "new token roster cannot follow stale status from another game connection");
    ui.stream.on_write = {}; ui.poll();
    require(ui.stream.sent.size() == 4 && ui.stream.sent[1].type() == Type::game_status &&
        ui.stream.sent[2].type() == Type::game_auto_guisuo_roster, "next poll sends new token status before new roster");
    confirm(ui.stream.sent.back(), 102, 0, {2});
}
void malformed_and_roles() {
    require(Packet(Type::game_auto_guisuo_roster).valid() && Packet(Type::game_auto_guisuo_exclusions_set).valid() &&
        Packet(Type::game_movement_trace_set).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid(),
        "supported wire types are valid and unknown types are rejected");
    std::vector<Packet> invalid_rosters, invalid_commands;
    const auto valid_roster = roster_packet(roster());
    const auto valid_command = command(100, 10, {1, 2});
    for (size_t length = 0; length <= valid_roster.length() + 4; ++length) {
        if (length == valid_roster.length()) continue;
        Packet packet(Type::game_auto_guisuo_roster);
        require(packet.add_bytes(valid_roster.bytes.data() + 8, length), "create malformed roster length");
        invalid_rosters.push_back(packet);
    }
    for (size_t length = 0; length <= valid_command.length() + 4; ++length) {
        if (length == valid_command.length()) continue;
        Packet packet(Type::game_auto_guisuo_exclusions_set);
        require(packet.add_bytes(valid_command.bytes.data() + 8, length), "create malformed exclusions length");
        invalid_commands.push_back(packet);
    }
    // Fields after the side prefix: match, player count, UID/hero pairs, exclusion count and IDs.
    for (const auto field : std::vector<std::pair<size_t, uint32_t>>{
        {4, 0}, {8, 15}, {8, UINT32_MAX}, {12, 0}, {16, 65536}, {16, UINT32_MAX},
        {20, 10}, {44, 15}, {44, UINT32_MAX}, {48, 0}, {48, 3}}) {
        auto packet = valid_roster; write32(packet.bytes.data() + 12 + field.first, field.second);
        invalid_rosters.push_back(packet);
    }
    for (const auto field : std::vector<std::pair<size_t, uint32_t>>{
        {4, 0}, {12, 0}, {16, 15}, {16, UINT32_MAX}, {20, 65536}, {20, UINT32_MAX}, {24, 1}}) {
        auto packet = valid_command; write32(packet.bytes.data() + 12 + field.first, field.second);
        invalid_commands.push_back(packet);
    }
    auto duplicate_excluded = valid_roster;
    write32(duplicate_excluded.bytes.data() + 12 + 44, 2);
    require(duplicate_excluded.add_number(2), "fixture appends duplicate roster exclusion");
    invalid_rosters.push_back(duplicate_excluded);
    for (const bool primary : {false, true}) {
        for (const auto& packet : primary ? invalid_commands : invalid_rosters) {
            Sessions sessions;
            Worker ui(sessions, enter(sessions));
            Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
            game.receive({valid_roster}); ui.receive({command(100, 1, {1})});
            const auto before = sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side);
            auto& source = primary ? ui : game;
            source.receive({packet, primary ? command(100, 11, {2}) : roster_packet(roster(200))});
            protocol_error(source);
            const auto after = sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side);
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
    require(!sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side).ready, "wrong roles cannot initialize roster");
    require(!sessions.update_game_auto_guisuo_roster({}, test_side, roster()) &&
        sessions.set_game_auto_guisuo_exclusions({}, test_side, {100, 1, {1}}) == Sessions::AutoGuisuoExclusionsResult::invalid,
        "empty binding cannot read or write dynamic state");
    game.stream.sent.clear(); game.receive({valid_roster});
    ui.stream.sent.clear(); ui.receive({command(100, 1, {1}), command(100, 1, {2})});
    protocol_error(ui);
    require(sessions.auto_guisuo_roster_snapshot(game.member.binding, test_side).exclusions.hero_ids == std::vector<uint32_t>{1},
        "same request number with different set cannot change accepted state");
    require(sessions.invalidate_account("owner") && !sessions.auto_guisuo_roster_snapshot(game.member.binding, test_side).ready,
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
    const auto snapshot = sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side);
    require(snapshot.ready && snapshot.roster.players.empty(), "valid empty roster clears all dynamic controls");
}
Packet config(uint32_t enabled = 0, uint32_t min_count = 0) {
    Packet packet(Type::game_auto_guisuo_set);
    require(packet.add_number(enabled) && packet.add_number(min_count), "encode guisuo config");
    return packet;
}
void configuration_and_reconnect() {
    static_assert(uint32_t(Type::game_auto_guisuo_set) == 159 && uint32_t(Type::game_auto_guisuo_roster) == 160 &&
        uint32_t(Type::game_auto_guisuo_exclusions_set) == 161);
    require(game_auto_guisuo_min_count_default == 0 && game_auto_guisuo_min_count_max == 6,
        "nearby-player count defaults to zero with maximum six");
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto initial = sessions.auto_guisuo_command_snapshot(game.member.binding);
    require(!initial.initialized && !initial.enabled && !initial.min_count, "initial defaults are disabled with zero count");
    game.poll(); require(game.stream.sent.empty(), "no unsolicited command before LA initialization");
    for (uint32_t enabled : {0u, 1u}) for (uint32_t min_count = 0; min_count <= 6; ++min_count) {
        const auto before = game.stream.sent.size();
        ui.receive({config(enabled, min_count)}); game.poll();
        require(game.stream.sent.size() == before + 1 && game.stream.sent.back().bytes == config(enabled, min_count).bytes,
            "actual handler forwards all valid flags and count settings");
        const auto snapshot = sessions.auto_guisuo_command_snapshot(game.member.binding);
        require(snapshot.enabled == enabled && snapshot.min_count == min_count && snapshot.initialized,
            "session atomically preserves both fields");
        ui.receive({config(enabled, min_count)}); game.poll();
        require(game.stream.sent.size() == before + 1 &&
            sessions.auto_guisuo_command_snapshot(game.member.binding).revision == snapshot.revision,
            "duplicate settings do not advance revisions or resend");
    }
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "commands are account and IP isolated");
    ui.receive({config(0, 3)});
    const auto revision = game.sent_auto_guisuo_command_revision;
    game.stream.writable = false; game.poll();
    require(game.sent_auto_guisuo_command_revision == revision, "failed send remains pending");
    game.stream.writable = true; game.poll();
    require(game.stream.sent.back().bytes == config(0, 3).bytes, "retry forwards disabled count setting");
    sessions.leave(game.member.binding);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 1 && replacement.stream.sent.back().bytes == config(0, 3).bytes,
        "game reconnect restores complete configuration exactly once");
    game.stream.sent.clear(); game.poll(); require(game.stream.sent.empty(), "stale game token cannot receive commands");
    replacement.stream.sent.clear(); replacement.receive({config(1, 6)}); protocol_error(replacement);
    require(sessions.auto_guisuo_command_snapshot(replacement.member.binding).min_count == 3,
        "game endpoint cannot mutate controls");
    sessions.leave(ui.member.binding); ui.receive({config(1, 6)}); protocol_error(ui);
}
void invalid_configs_are_atomic() {
    std::vector<Packet> invalid{config(2, 0), config(UINT32_MAX, 0), config(1, 7), config(1, UINT32_MAX)};
    const auto valid = config();
    for (size_t length = 0; length <= 12; ++length) {
        if (length == 8) continue;
        Packet packet(Type::game_auto_guisuo_set);
        require(packet.add_bytes(valid.bytes.data() + 8, length), "encode malformed count payload");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        Worker ui(sessions, enter(sessions));
        Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
        ui.receive({config(1, 3)});
        const auto before = sessions.auto_guisuo_command_snapshot(game.member.binding).revision;
        ui.receive({packet, config()}); protocol_error(ui); game.poll();
        require(sessions.auto_guisuo_command_snapshot(game.member.binding).revision == before &&
            game.stream.sent.size() == 1 && game.stream.sent[0].bytes == config(1, 3).bytes,
            "invalid config preserves complete card and stops dispatch before later frame");
    }
}
void roster_and_feature_isolation() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    require(sessions.update_game(game.member.binding, 10, 4, "Self"), "identify own UID before allied roster");
    game.receive({roster_packet(roster())});
    const auto allies = sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side);
    require(allies.ready && allies.roster.players[0].uid == 10, "server retains the exact roster supplied by the game");
    require(sessions.update_game_auto_heal_roster(game.member.binding, roster()) &&
        sessions.update_game_auto_disarm_roster(game.member.binding, roster()) &&
        sessions.update_game_near_item_roster(game.member.binding, 6, roster()), "initialize other features independently");
    require(sessions.set_game_auto_heal_exclusions(ui.member.binding, {100, 1, {2, 65535}}) == Sessions::AutoHealExclusionsResult::applied &&
        sessions.set_game_auto_disarm_exclusions(ui.member.binding, {100, 1, {65535}}) == Sessions::AutoDisarmExclusionsResult::applied &&
        sessions.set_game_near_item_exclusions(ui.member.binding, 6, {100, 1, {2}}) == Sessions::NearItemExclusionsResult::applied,
        "initialize distinct exclusions in healing, disarm and near-item features");
    ui.receive({command(100, 1, {1})});
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side).exclusions.hero_ids == std::vector<uint32_t>{1} &&
        sessions.auto_heal_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>({2, 65535}) &&
        sessions.auto_disarm_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{65535} &&
        sessions.near_item_roster_snapshot(ui.member.binding, 6).exclusions.hero_ids == std::vector<uint32_t>{2},
        "guisuo exclusions do not alter other allied or enemy exclusions");
    Packet heal_packet;
    require(make_game_auto_heal_roster(roster(), heal_packet), "encode different allied feature packet");
    GameAutoDisarmRoster decoded;
    require(!read_test_roster(heal_packet, decoded) &&
        !read_game_auto_heal_roster(roster_packet(roster()), decoded), "allied features enforce distinct wire types");
    auto next_match = roster(101); next_match.excluded_hero_ids.clear();
    game.receive({roster_packet(next_match)});
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side).exclusions.hero_ids.empty() &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, test_side).exclusions.request_id == 0 &&
        sessions.auto_heal_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>({2, 65535}),
        "new match clears guisuo exclusions without resetting healing");
}
void two_sides_are_independent() {
    static_assert(game_auto_guisuo_side_count == 2 && game_auto_guisuo_ally == 0 && game_auto_guisuo_enemy == 1);
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({config(1, 4)});
    for (test_side = 0; test_side < 2; ++test_side) {
        game.receive({roster_packet(roster())});
        ui.receive({command(100, test_side ? 2 : 90, {test_side ? 2u : 1u})});
    }
    ui.poll(); game.poll();
    require(ui.stream.sent.size() == 5 && game.stream.sent.size() == 3, "both side rosters and confirmations precede one common config");
    for (test_side = 0; test_side < 2; ++test_side) {
        confirm(game.stream.sent[test_side], 100, test_side ? 2 : 90, {test_side ? 2u : 1u});
        GameAutoDisarmRoster decoded;
        require(read_test_roster(ui.stream.sent[1 + 2 * test_side], decoded), "primary frames retain each side prefix");
    }
    auto ally = sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0);
    auto enemy = sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1);
    test_side = 1;
    ui.receive({command(100, 3, {65535})}); game.poll();
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0).exclusions_revision == ally.exclusions_revision &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).exclusions.request_id == 3,
        "low enemy request advances independently from higher allied request");
    test_side = 0;
    ui.receive({command(100, 89, {2})});
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0).exclusions.hero_ids == std::vector<uint32_t>{1} &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).exclusions.hero_ids == std::vector<uint32_t>{65535},
        "stale allied request cannot alter either exclusion channel");
    sessions.leave(game.member.binding);
    Worker reconnect(sessions, enter(sessions, "owner", 10, Role::secondary));
    require(!sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0).ready &&
        !sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).ready, "reconnect hides both retained rosters");
    reconnect.receive({roster_packet(roster())}); reconnect.poll();
    confirm(reconnect.stream.sent[0], 100, 90, {1});
    require(!sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).ready, "allied report cannot unlock enemy roster");
    test_side = 1; reconnect.receive({roster_packet(roster())}); reconnect.poll();
    confirm(reconnect.stream.sent.back(), 100, 3, {65535});
    require(sessions.auto_guisuo_command_snapshot(reconnect.member.binding).min_count == 4,
        "single configuration persists across reconnect");
    test_side = 0; reconnect.receive({roster_packet({101, {}, {}})});
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0).exclusions.request_id == 0 &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, 0).roster.players.empty() &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).exclusions.request_id == 3,
        "new match clears only the reported side until other side identifies its match");
    test_side = 1; reconnect.receive({roster_packet({101, {}, {}})});
    require(sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).exclusions.hero_ids.empty() &&
        sessions.auto_guisuo_roster_snapshot(ui.member.binding, 1).exclusions.request_id == 0, "new match clears enemy exclusions");
    for (const uint32_t invalid : {2u, UINT32_MAX}) {
        require(!valid_game_auto_guisuo_side(invalid) &&
            !sessions.auto_guisuo_roster_snapshot(ui.member.binding, invalid).ready &&
            !sessions.update_game_auto_guisuo_roster(reconnect.member.binding, invalid, roster()) &&
            sessions.set_game_auto_guisuo_exclusions(ui.member.binding, invalid, {100, 1, {1}}) == Sessions::AutoGuisuoExclusionsResult::invalid,
            "invalid side fails session validation before indexing");
        Packet encoded(Type::ping);
        require(!make_game_auto_guisuo_roster(invalid, roster(), encoded) &&
            !make_game_auto_guisuo_exclusions(invalid, {100, 1, {1}}, encoded), "invalid side cannot serialize");
        auto packet = roster_packet(roster()); write32(packet.bytes.data() + 8, invalid);
        reconnect.stream.sent.clear(); reconnect.receive({packet}); protocol_error(reconnect);
        packet = command(100, 1, {1}); write32(packet.bytes.data() + 8, invalid);
        ui.stream.sent.clear(); ui.receive({packet}); protocol_error(ui);
    }
    // On failure, a two-side batch must leave the outer send cycle immediately.
    for (test_side = 0; test_side < 2; ++test_side) reconnect.receive({roster_packet(roster(102))});
    Worker failed_ui(sessions, ui.member.binding), failed_game(sessions, reconnect.member.binding);
    failed_ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() == Type::game_status) failed_ui.stream.writable = false;
    };
    failed_ui.poll();
    require(failed_ui.sent_auto_guisuo_roster_revisions[0] == UINT64_MAX &&
        failed_ui.sent_auto_guisuo_roster_revisions[1] == UINT64_MAX, "failed first roster leaves both revisions pending");
    failed_game.stream.on_write = [&](const Packet&) { failed_game.stream.writable = false; };
    failed_game.poll();
    require(failed_game.sent_auto_guisuo_exclusions_revisions[0] != UINT64_MAX &&
        failed_game.sent_auto_guisuo_exclusions_revisions[1] == UINT64_MAX &&
        failed_game.sent_auto_guisuo_command_revision == UINT64_MAX, "failed second exclusion stops before config");
    failed_game.stream.on_write = {}; failed_game.stream.writable = true; failed_game.poll();
    require(failed_game.stream.sent.size() == 3 && failed_game.stream.sent.back().type() == Type::game_auto_guisuo_set,
        "retry sends pending side once then common config");
}
}
int main() {
    try {
        for (test_side = 0; test_side < game_auto_guisuo_side_count; ++test_side) {
        configuration_and_reconnect();
        invalid_configs_are_atomic();
        roster_and_feature_isolation();
        initialization_and_routing();
        pruning_matches_and_reconnect();
        snapshot_and_write_races();
        malformed_and_roles();
        limits_and_empty_roster();
        }
        two_sides_are_independent();
        std::cout << "PASS: " << checks << " auto guisuo server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
