// Uses the real server receive and send branches; TLS is replaced by an offline sink.
#include "../network/protocol.h"
#include "../sessions.h"
#include <functional>
#include <iostream>
#include <stdexcept>
#include <limits>

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
#include "near_item1_roster_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_game_revision = UINT64_MAX;
    std::array<uint64_t, game_near_item_count> sent_near_item_command_revisions;
    std::array<uint64_t, game_near_item_count> sent_near_item_roster_revisions;
    std::array<uint64_t, game_near_item_count> sent_near_item_exclusions_revisions;
    uint64_t& sent_near_item1_command_revision = sent_near_item_command_revisions[0];
    uint64_t& sent_near_item1_roster_revision = sent_near_item_roster_revisions[0];
    uint64_t& sent_near_item1_exclusions_revision = sent_near_item_exclusions_revisions[0];
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        sent_near_item_command_revisions.fill(UINT64_MAX);
        sent_near_item_roster_revisions.fill(UINT64_MAX);
        sent_near_item_exclusions_revisions.fill(UINT64_MAX);
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "near_item1_roster_handler.inc"
        }
    }
    void poll() {
        do {
            if (member.binding.role == Role::primary) {
#include "near_item1_roster_primary.inc"
            } else {
#include "near_item1_roster_game.inc"
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
    require(make_game_near_item1_roster(value, packet), "fixture encodes roster");
    return packet;
}
Packet command(uint64_t match, uint64_t request, std::vector<uint32_t> heroes) {
    Packet packet;
    require(make_game_near_item1_exclusions({match, request, std::move(heroes)}, packet), "fixture encodes exclusions");
    return packet;
}
GameAutoDisarmExclusions exclusions(const Packet& packet) {
    GameAutoDisarmExclusions value;
    require(read_game_near_item1_exclusions(packet, value), "valid exclusions response");
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
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_near_item1_roster,
        "primary receives roster followed by exclusions");
    GameAutoDisarmRoster received;
    require(read_game_near_item1_roster(ui.stream.sent[0], received) && received.players.size() == 4 &&
        received.excluded_hero_ids == std::vector<uint32_t>{2}, "all UIDs retained including duplicate hero");
    confirm(ui.stream.sent[1], 100, 0, {2});
    require(game.stream.sent.size() == 1, "game receives its initial exclusions once");
    confirm(game.stream.sent.back(), 100, 0, {2});
    require(other_ip.stream.sent.empty() && other_game.stream.sent.empty() && other_account.stream.sent.size() == 1,
        "account and IP pairs isolate dynamic roster");
    ui.receive({command(100, 1, {65535, 1, 2})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    confirm(game.stream.sent.back(), 100, 1, {1, 2, 65535});
    const auto before = sessions.near_item1_roster_snapshot(game.member.binding);
    game.receive({roster_packet(roster())}); game.poll(); ui.poll();
    require(sessions.near_item1_roster_snapshot(game.member.binding).exclusions_revision == before.exclusions_revision &&
        game.stream.sent.size() == 2, "same-match game report cannot overwrite cached exclusions");
    ui.receive({command(100, 1, {2, 65535, 1})}); ui.poll(); game.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1, 2, 65535});
    require(game.stream.sent.size() == 2, "identical request is acknowledged without game duplicate");
    ui.receive({command(100, 2, {1})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 2, {1});
    ui.stream.sent.clear();
    ui.receive({command(100, 1, {2})}); ui.poll();
    require(ui.stream.sent.size() == 2 && ui.stream.sent[0].type() == Type::game_near_item1_roster,
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
    const auto before = sessions.near_item1_roster_snapshot(secondary);
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    auto after = sessions.near_item1_roster_snapshot(secondary);
    require(after.roster.players.size() == 3 && after.exclusions.hero_ids == std::vector<uint32_t>({1, 2}) &&
        after.exclusions_revision == before.exclusions_revision, "deleting one duplicate-hero UID keeps hero exclusion");
    next.players.erase(next.players.begin() + 1); next.excluded_hero_ids.clear();
    game.receive({roster_packet(next)}); ui.poll(); game.poll();
    after = sessions.near_item1_roster_snapshot(secondary);
    require(after.exclusions_revision > before.exclusions_revision, "last hero UID deletion advances exclusion revision");
    confirm(game.stream.sent.back(), 100, 7, {1});
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 7, {1, 2})}); ui.poll();
    confirm(ui.stream.sent.back(), 100, 7, {1});
    ui.receive({command(100, 8, {2, 65535})}); ui.poll(); game.poll();
    confirm(game.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});

    sessions.leave(secondary);
    require(!sessions.near_item1_roster_snapshot(primary).ready, "disconnect hides retained old roster");
    secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, secondary);
    ui.stream.sent.clear(); ui.receive({command(100, 9, {1})}); ui.poll(); reconnected.poll();
    require(ui.stream.sent.size() == 1 && ui.stream.sent[0].type() == Type::game_status && reconnected.stream.sent.empty(),
        "no stale dynamic initialization before reconnect roster");
    reconnected.receive({roster_packet(roster())}); ui.poll(); reconnected.poll();
    confirm(reconnected.stream.sent.back(), 100, 8, {65535});
    confirm(ui.stream.sent.back(), 100, 8, {65535});
    require(!sessions.near_item1_roster_snapshot(game.member.binding).ready &&
        !sessions.update_game_near_item1_roster(game.member.binding, roster(900)), "old secondary cannot read or change replacement");
    sessions.leave(game.member.binding);
    require(sessions.current(secondary), "late old disconnect cannot reset replacement");
    sessions.leave(primary); primary = enter(sessions);
    require(sessions.set_game_near_item1_exclusions(ui.member.binding, {100, 99, {1}}) ==
        Sessions::NearItem1ExclusionsResult::invalid, "replaced primary loses write access");
    Worker new_ui(sessions, primary); new_ui.poll();
    require(new_ui.stream.sent.size() == 3 && new_ui.stream.sent[0].type() == Type::game_status &&
        new_ui.stream.sent[1].type() == Type::game_near_item1_roster, "primary reconnect initializes status then roster then confirmation");
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
    confirm(command(101, sessions.near_item1_roster_snapshot(primary).exclusions.request_id,
        sessions.near_item1_roster_snapshot(primary).exclusions.hero_ids), 101, 0, {2});
}
void snapshot_and_write_races() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.receive({roster_packet(roster())});
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() == Type::game_near_item1_roster)
            require(sessions.set_game_near_item1_exclusions(ui.member.binding, {100, 1, {1}}) ==
                Sessions::NearItem1ExclusionsResult::applied, "simulate concurrent state update between response frames");
    };
    ui.poll();
    confirm(ui.stream.sent.back(), 100, 0, {2});
    ui.stream.on_write = {}; ui.poll();
    confirm(ui.stream.sent.back(), 100, 1, {1});
    game.stream.writable = false; game.poll();
    require(game.sent_near_item1_exclusions_revision == UINT64_MAX, "failed game send remains pending");
    game.stream.writable = true; game.poll(); confirm(game.stream.sent.back(), 100, 1, {1});
    ui.sent_near_item1_roster_revision = UINT64_MAX; ui.stream.sent.clear();
    ui.stream.writable = false; ui.poll();
    require(ui.sent_near_item1_roster_revision == UINT64_MAX, "failed roster send remains pending");
    ui.stream.writable = true; ui.poll();
    require(ui.stream.sent.size() == 2, "roster retry still sends both coherent frames");
    confirm(ui.stream.sent.back(), 100, 1, {1});

    ui.sent_game_revision = UINT64_MAX; ui.stream.sent.clear();
    Sessions::Binding replacement;
    ui.stream.on_write = [&](const Packet& packet) {
        if (packet.type() != Type::game_status) return;
        sessions.leave(game.member.binding);
        replacement = enter(sessions, "owner", 10, Role::secondary);
        require(sessions.update_game_near_item1_roster(replacement, roster(102)), "replacement initialized during status write");
    };
    ui.poll();
    require(ui.stream.sent.size() == 1, "new token roster cannot follow stale status from another game connection");
    ui.stream.on_write = {}; ui.poll();
    require(ui.stream.sent.size() == 4 && ui.stream.sent[1].type() == Type::game_status &&
        ui.stream.sent[2].type() == Type::game_near_item1_roster, "next poll sends new token status before new roster");
    confirm(ui.stream.sent.back(), 102, 0, {2});
}
void malformed_and_roles() {
    require(Packet(Type::game_near_item1_roster).valid() && Packet(Type::game_near_item1_exclusions_set).valid() &&
        Packet(Type::game_movement_trace_set).valid() && Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid(),
        "supported wire types are valid and unknown types are rejected");
    std::vector<Packet> invalid_rosters, invalid_commands;
    const auto valid_roster = roster_packet(roster());
    const auto valid_command = command(100, 10, {1, 2});
    for (size_t length = 0; length <= valid_roster.length() + 4; ++length) {
        if (length == valid_roster.length()) continue;
        Packet packet(Type::game_near_item1_roster);
        require(packet.add_bytes(valid_roster.bytes.data() + 8, length), "create malformed roster length");
        invalid_rosters.push_back(packet);
    }
    for (size_t length = 0; length <= valid_command.length() + 4; ++length) {
        if (length == valid_command.length()) continue;
        Packet packet(Type::game_near_item1_exclusions_set);
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
            const auto before = sessions.near_item1_roster_snapshot(ui.member.binding);
            auto& source = primary ? ui : game;
            source.receive({packet, primary ? command(100, 11, {2}) : roster_packet(roster(200))});
            protocol_error(source);
            const auto after = sessions.near_item1_roster_snapshot(ui.member.binding);
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
    require(!sessions.near_item1_roster_snapshot(ui.member.binding).ready, "wrong roles cannot initialize roster");
    require(!sessions.update_game_near_item1_roster({}, roster()) &&
        sessions.set_game_near_item1_exclusions({}, {100, 1, {1}}) == Sessions::NearItem1ExclusionsResult::invalid,
        "empty binding cannot read or write dynamic state");
    game.stream.sent.clear(); game.receive({valid_roster});
    ui.stream.sent.clear(); ui.receive({command(100, 1, {1}), command(100, 1, {2})});
    protocol_error(ui);
    require(sessions.near_item1_roster_snapshot(game.member.binding).exclusions.hero_ids == std::vector<uint32_t>{1},
        "same request number with different set cannot change accepted state");
    require(sessions.invalidate_account("owner") && !sessions.near_item1_roster_snapshot(game.member.binding).ready,
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
    const auto snapshot = sessions.near_item1_roster_snapshot(ui.member.binding);
    require(snapshot.ready && snapshot.roster.players.empty(), "valid empty roster clears all dynamic controls");
}
Packet config(uint32_t enabled = 0, uint32_t direct = 1, uint32_t on_attack = 0,
    uint32_t on_attacked = 0, float range = 3.0f, uint32_t health = 100) {
    Packet packet(Type::game_near_item1_set);
    for (uint32_t field : {enabled, direct, on_attack, on_attacked, game_near_item1_range_to_bits(range), health})
        require(packet.add_number(field), "encode config field");
    return packet;
}
void config_routing_and_boundaries() {
    static_assert(uint32_t(Type::game_near_item1_set) == 147 && uint32_t(Type::game_near_item1_roster) == 148 &&
        uint32_t(Type::game_near_item1_exclusions_set) == 149);
    require(game_near_item1_range_min == 3 && game_near_item1_range_max == 6 &&
        game_near_item1_range_default == 6 && game_near_item1_range_step == 0.1f && game_near_item1_health_default == 100,
        "near item defaults and limits match the card");
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    auto current = sessions.near_item1_command_snapshot(game.member.binding);
    require(!current.initialized && !current.enabled && current.direct == 1 && !current.on_attack && !current.on_attacked &&
        current.range_bits == game_near_item1_range_to_bits(6) && current.health_percent == 100, "initial config defaults");
    game.poll(); require(game.stream.sent.empty(), "no command before LA initialization");
    ui.receive({config()}); game.poll();
    require(game.stream.sent.size() == 1 && game.stream.sent.back().bytes == config().bytes,
        "disabled config initializes and forwards all six fields");
    for (uint32_t bits = 0; bits < 16; ++bits) {
        const auto packet = config(bits & 1, (bits >> 1) & 1, (bits >> 2) & 1, (bits >> 3) & 1,
            bits & 1 ? 6.0f : 3.0f, bits & 2 ? 0u : 100u);
        const auto count = game.stream.sent.size();
        ui.receive({packet}); game.poll();
        require(game.stream.sent.size() == count + 1 && game.stream.sent.back().bytes == packet.bytes,
            "every flag combination preserves field order and slider boundaries");
        const auto revision = sessions.near_item1_command_snapshot(game.member.binding).revision;
        ui.receive({packet}); game.poll();
        require(game.stream.sent.size() == count + 1 && sessions.near_item1_command_snapshot(game.member.binding).revision == revision,
            "duplicate config does not advance revision or resend");
    }
    const auto packet = config(1, 0, 1, 0, 4.3f, 37);
    ui.receive({packet});
    const auto sent_revision = game.sent_near_item1_command_revision;
    game.stream.writable = false; game.poll();
    require(game.sent_near_item1_command_revision == sent_revision, "failed config send remains pending");
    game.stream.writable = true; game.poll();
    require(game.stream.sent.back().bytes == packet.bytes, "fractional meters and health preserve exact bytes");
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "config isolates account and IP pairs");
    sessions.leave(game.member.binding);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 1 && replacement.stream.sent.back().bytes == packet.bytes,
        "game reconnect restores latest complete configuration once");
    require(!sessions.near_item1_command_snapshot(game.member.binding).initialized, "old game token loses config access");
    game.stream.sent.clear(); game.poll(); require(game.stream.sent.empty(), "replaced game receives nothing");
    const auto saved = sessions.near_item1_command_snapshot(replacement.member.binding);
    require(saved.health_percent == 37 && saved.on_attack == 1 && saved.on_attacked == 0 && !saved.direct,
        "configuration survives game connection reset");
    sessions.leave(ui.member.binding);
    ui.receive({config()}); protocol_error(ui);
    Worker new_ui(sessions, enter(sessions));
    new_ui.receive({config(0, 1, 0, 1, 5.1f, 1)}); replacement.poll();
    require(replacement.stream.sent.back().bytes == config(0, 1, 0, 1, 5.1f, 1).bytes,
        "replacement primary can change retained card");
    replacement.stream.sent.clear(); replacement.receive({config()}); protocol_error(replacement);
    require(sessions.near_item1_command_snapshot(replacement.member.binding).health_percent == 1,
        "game endpoint cannot mutate configuration");
}
void invalid_configs_are_atomic() {
    std::vector<Packet> invalid;
    for (size_t field = 0; field < 6; ++field) {
        if (field == 4) continue;
        auto packet = config();
        write32(packet.bytes.data() + 8 + field * 4, field == 5 ? 101u : 2u); invalid.push_back(packet);
        write32(packet.bytes.data() + 8 + field * 4, UINT32_MAX); invalid.push_back(packet);
    }
    const float inf = std::numeric_limits<float>::infinity();
    for (float value : {std::nextafter(3.0f, -inf), std::nextafter(6.0f, inf), inf, -inf,
        std::numeric_limits<float>::quiet_NaN(), -1.0f, 0.0f}) invalid.push_back(config(1, 0, 1, 1, value, 25));
    const auto valid = config();
    for (size_t length = 0; length <= 28; ++length) {
        if (length == 24) continue;
        Packet packet(Type::game_near_item1_set);
        require(packet.add_bytes(valid.bytes.data() + 8, length), "fixture malformed config length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        Worker ui(sessions, enter(sessions));
        Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
        const auto retained = config(1, 1, 0, 1, 4.5f, 60);
        ui.receive({retained});
        const auto before = sessions.near_item1_command_snapshot(game.member.binding).revision;
        ui.receive({packet, config()}); protocol_error(ui); game.poll();
        require(sessions.near_item1_command_snapshot(game.member.binding).revision == before &&
            game.stream.sent.size() == 1 && game.stream.sent.back().bytes == retained.bytes,
            "invalid config is atomic and stops dispatch before subsequent frame");
    }
}
void disarm_isolation_and_wire_types() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    require(sessions.update_game_auto_disarm_roster(game.member.binding, roster()), "initialize independent disarm roster");
    game.receive({roster_packet(roster())});
    ui.receive({command(100, 1, {1})});
    require(sessions.set_game_auto_disarm_exclusions(ui.member.binding, {100, 1, {65535}}) ==
        Sessions::AutoDisarmExclusionsResult::applied, "set independent disarm exclusions with same request id");
    require(sessions.near_item1_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{1} &&
        sessions.auto_disarm_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{65535},
        "near item and disarm exclusions do not affect one another");
    Packet disarm_roster, disarm_exclusions;
    require(make_game_auto_disarm_roster(roster(), disarm_roster) &&
        make_game_auto_disarm_exclusions({100, 2, {1}}, disarm_exclusions), "encode disarm fixtures");
    GameAutoDisarmRoster decoded_roster; GameAutoDisarmExclusions decoded_exclusions;
    require(!read_game_near_item1_roster(disarm_roster, decoded_roster) &&
        !read_game_near_item1_exclusions(disarm_exclusions, decoded_exclusions) &&
        !read_game_auto_disarm_roster(roster_packet(roster()), decoded_roster) &&
        !read_game_auto_disarm_exclusions(command(100, 2, {1}), decoded_exclusions), "wrapper readers enforce distinct message types");
    auto next = roster(101); next.excluded_hero_ids.clear();
    game.receive({roster_packet(next)});
    require(sessions.near_item1_roster_snapshot(ui.member.binding).exclusions.hero_ids.empty() &&
        sessions.auto_disarm_roster_snapshot(ui.member.binding).exclusions.hero_ids == std::vector<uint32_t>{65535},
        "new match clears only near item exclusions");
}
Packet slot_config(uint32_t slot, uint32_t health) {
    const auto payload = config(slot & 1, (slot >> 1) & 1, (slot >> 2) & 1, slot & 1, 3.0f + slot * 0.5f, health);
    if (slot == 1) return payload;
    Packet packet(Type::game_near_item_set);
    require(packet.add_number(slot) && packet.add_bytes(payload.bytes.data() + 8, payload.length()), "encode slot config");
    return packet;
}
Packet slot_roster(uint32_t slot, uint64_t match = 100) {
    auto value = roster(match); value.excluded_hero_ids.clear();
    if (slot == 1) return roster_packet(value);
    Packet packet;
    require(make_game_near_item_roster(slot, value, packet), "encode slot roster");
    return packet;
}
Packet slot_exclusions(uint32_t slot, uint64_t match, uint64_t request, const std::vector<uint32_t>& heroes) {
    if (slot == 1) return command(match, request, heroes);
    Packet packet;
    require(make_game_near_item_exclusions(slot, {match, request, heroes}, packet), "encode slot exclusions");
    return packet;
}
void six_slots_route_and_reconnect() {
    static_assert(game_near_item_count == 6 && uint32_t(Type::game_near_item_set) == 150 &&
        uint32_t(Type::game_near_item_roster) == 151 && uint32_t(Type::game_near_item_exclusions_set) == 152);
    Sessions sessions;
    Worker ui(sessions, enter(sessions));
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    const std::array<std::vector<uint32_t>, 6> excluded{{{1}, {2}, {65535}, {1, 2}, {1, 65535}, {2, 65535}}};
    for (uint32_t slot = 1; slot <= game_near_item_count; ++slot) {
        const auto initial = sessions.near_item_command_snapshot(game.member.binding, slot);
        require(!initial.initialized && initial.direct == 1 && initial.range_bits == game_near_item1_range_to_bits(6) &&
            initial.health_percent == 100, "each slot starts at independent 6m and 100% defaults");
        ui.receive({slot_config(slot, slot * 10)}); game.poll();
        require(game.stream.sent.size() == slot && game.stream.sent.back().bytes == slot_config(slot, slot * 10).bytes,
            "all six configs forward in correct slot without changing legacy slot-one encoding");
        game.receive({slot_roster(slot)});
        ui.receive({slot_exclusions(slot, 100, 1, excluded[slot - 1])});
        // Discard this slot's pending exclusions so subsequent config counts remain independent.
        game.poll(); require(game.stream.sent.back().bytes == slot_exclusions(slot, 100, 1, excluded[slot - 1]).bytes,
            "each slot receives only its own confirmed exclusion set");
        game.stream.sent.pop_back();
    }
    ui.poll();
    require(ui.stream.sent.size() == 13 && ui.stream.sent.front().type() == Type::game_status,
        "LA receives status then six roster and confirmation pairs");
    for (uint32_t slot = 1; slot <= game_near_item_count; ++slot) {
        const auto current = sessions.near_item_command_snapshot(game.member.binding, slot);
        require(current.health_percent == slot * 10 && current.range_bits == game_near_item1_range_to_bits(3.0f + slot * 0.5f),
            "six configuration values do not overwrite each other");
        const auto state = sessions.near_item_roster_snapshot(ui.member.binding, slot);
        require(state.ready && state.exclusions.request_id == 1 && state.exclusions.hero_ids == excluded[slot - 1],
            "same request number remains independent in each slot");
        uint32_t decoded_slot = 1; GameAutoDisarmRoster decoded;
        const auto& packet = ui.stream.sent[2 * slot - 1];
        require((slot == 1 ? read_game_near_item1_roster(packet, decoded) : read_game_near_item_roster(packet, decoded_slot, decoded)) &&
            decoded_slot == slot && decoded.excluded_hero_ids == excluded[slot - 1], "LA roster routing retains slot and exclusions");
        require(ui.stream.sent[2 * slot].bytes == slot_exclusions(slot, 100, 1, excluded[slot - 1]).bytes,
            "LA confirmation preserves slot and request id");
    }
    ui.stream.sent.clear(); game.stream.sent.clear();
    const auto unaffected = sessions.near_item_command_snapshot(game.member.binding, 5).revision;
    ui.receive({slot_config(4, 1), slot_exclusions(2, 100, 2, {})}); ui.poll(); game.poll();
    require(ui.stream.sent.size() == 1 && ui.stream.sent[0].bytes == slot_exclusions(2, 100, 2, {}).bytes &&
        game.stream.sent.size() == 2 && game.stream.sent[0].bytes == slot_exclusions(2, 100, 2, {}).bytes &&
        game.stream.sent[1].bytes == slot_config(4, 1).bytes &&
        sessions.near_item_command_snapshot(game.member.binding, 5).revision == unaffected,
        "changing slot-four settings and slot-two exclusions emits only those two changes");
    sessions.leave(game.member.binding);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 6, "reconnect replays all six configs once before fresh rosters");
    for (uint32_t slot = 1; slot <= game_near_item_count; ++slot) {
        require(replacement.stream.sent[slot - 1].bytes == slot_config(slot, slot == 4 ? 1 : slot * 10).bytes &&
            !sessions.near_item_roster_snapshot(ui.member.binding, slot).ready, "reconnect preserves config but hides stale exclusions");
        replacement.receive({slot_roster(slot)});
    }
    replacement.stream.sent.clear(); replacement.poll();
    require(replacement.stream.sent.size() == 6, "fresh same-match rosters restore six retained exclusion sets");
    for (uint32_t slot = 1; slot <= game_near_item_count; ++slot)
        require(replacement.stream.sent[slot - 1].bytes == slot_exclusions(slot, 100, slot == 2 ? 2 : 1,
            slot == 2 ? std::vector<uint32_t>{} : excluded[slot - 1]).bytes, "reconnect never mixes slot request ids or exclusions");
    replacement.receive({slot_roster(3, 101)});
    require(sessions.near_item_roster_snapshot(ui.member.binding, 3).exclusions.hero_ids.empty() &&
        sessions.near_item_roster_snapshot(ui.member.binding, 3).exclusions.request_id == 0 &&
        sessions.near_item_roster_snapshot(ui.member.binding, 6).exclusions.hero_ids == excluded[5],
        "a fresh match clears only the slot whose roster has arrived");
    const auto revision = sessions.near_item_roster_snapshot(ui.member.binding, 3).roster_revision;
    game.stream.sent.clear(); game.receive({slot_roster(3, 102)}); protocol_error(game);
    require(sessions.near_item_roster_snapshot(ui.member.binding, 3).roster_revision == revision,
        "old game token cannot alter a reconnected slot");
}
void invalid_slots_and_multi_payloads() {
    for (uint32_t slot : {0u, 7u, UINT32_MAX}) {
        require(!valid_game_near_item_slot(slot), "reject slot outside 1..6");
        Packet packet;
        require(!make_game_near_item_roster(slot, roster(), packet) &&
            !make_game_near_item_exclusions(slot, {100, 1, {}}, packet), "builders reject invalid slot");
        Sessions sessions;
        Worker ui(sessions, enter(sessions));
        Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
        require(!sessions.set_game_near_item(ui.member.binding, slot, 1, 1, 1, 1, game_near_item1_range_to_bits(6), 100) &&
            !sessions.update_game_near_item_roster(game.member.binding, slot, roster()) &&
            sessions.set_game_near_item_exclusions(ui.member.binding, slot, {100, 1, {}}) == Sessions::NearItemExclusionsResult::invalid &&
            !sessions.near_item_command_snapshot(game.member.binding, slot).initialized &&
            !sessions.near_item_roster_snapshot(ui.member.binding, slot).ready, "session rejects invalid slot before array indexing");
        auto bad_config = slot_config(2, 20); write32(bad_config.bytes.data() + 8, slot);
        auto bad_roster = slot_roster(2); write32(bad_roster.bytes.data() + 8, slot);
        auto bad_exclusions = slot_exclusions(2, 100, 1, {1}); write32(bad_exclusions.bytes.data() + 8, slot);
        uint32_t decoded_slot = 55; GameAutoDisarmRoster decoded_roster; GameAutoDisarmExclusions decoded_exclusions;
        require(!read_game_near_item_roster(bad_roster, decoded_slot, decoded_roster) && decoded_slot == 55 &&
            !read_game_near_item_exclusions(bad_exclusions, decoded_slot, decoded_exclusions) && decoded_slot == 55,
            "invalid slot parser leaves output slot untouched");
        ui.receive({bad_config, slot_config(2, 20)}); protocol_error(ui); ui.stream.sent.clear();
        ui.receive({bad_exclusions}); protocol_error(ui);
        game.receive({bad_roster}); protocol_error(game);
        require(!sessions.near_item_command_snapshot(game.member.binding, 2).initialized &&
            !sessions.near_item_roster_snapshot(ui.member.binding, 2).ready, "invalid frames cannot initialize another slot");
    }
    for (const Packet& valid : {slot_config(6, 100), slot_roster(6), slot_exclusions(6, 100, 1, {1})}) {
        for (size_t length : {size_t(0), size_t(3), size_t(valid.length() - 1), size_t(valid.length() + 1)}) {
            Sessions sessions;
            Worker ui(sessions, enter(sessions));
            Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
            Packet invalid(valid.type()); require(invalid.add_bytes(valid.bytes.data() + 8, length), "encode malformed multi-slot payload");
            auto& sender = valid.type() == Type::game_near_item_roster ? game : ui;
            sender.receive({invalid}); protocol_error(sender);
        }
    }
    Sessions sessions;
    Worker ui(sessions, enter(sessions)); Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({slot_roster(6)}); protocol_error(ui);
    game.receive({slot_config(6, 100)}); protocol_error(game); game.stream.sent.clear();
    game.receive({slot_exclusions(6, 100, 1, {})}); protocol_error(game);
}
}
int main() {
    try {
        six_slots_route_and_reconnect();
        invalid_slots_and_multi_payloads();
        config_routing_and_boundaries();
        invalid_configs_are_atomic();
        disarm_isolation_and_wire_types();
        initialization_and_routing();
        pruning_matches_and_reconnect();
        snapshot_and_write_races();
        malformed_and_roles();
        limits_and_empty_roster();
        std::cout << "PASS: " << checks << " near-item server checks (offline, six slots and legacy slot one)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
