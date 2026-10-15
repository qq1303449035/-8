// Standalone C++17 test; no database or network. The timeout phase takes 61 s.
// cl /nologo /std:c++17 /EHsc /utf-8 tests\session_regression_tests.cpp
#include "../network/protocol.h"
#include "../sessions.h"
#include <algorithm>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using chh::server::Sessions;
using chh::server::ClientRole;
using Clock = std::chrono::steady_clock;
using Game = Sessions::Game;
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, ClientRole role) {
    auto binding = sessions.enter(account, ip, role);
    require(bool(binding), "fixture binding created");
    return binding;
}
void game(Sessions& sessions, const Sessions::Binding& binding, uint32_t state,
    const std::string& name, bool team, uint32_t uid = 100) {
    require(sessions.update_game(binding, uid, state, name, 7, 1, team ? 1 : 0), "fixture game state updated");
}
template<class T> T value(const std::map<std::string, T>& items, const std::string& account) {
    const auto found = items.find(account);
    return found == items.end() ? T{} : found->second;
}
void selected(Sessions& sessions, const std::string& account, bool connected,
    uint32_t state, const std::string& name, bool team, uint32_t uid) {
    const auto status = sessions.game_snapshot(account);
    const auto presence = sessions.presence();
    require(status.connected == connected && status.state == state && status.player_name == name &&
        status.team == team && status.uid == uid, "account snapshot selects one complete game instance");
    require(value(presence.game_connected, account) == connected && value(presence.game_states, account) == state &&
        value(presence.game_player_names, account) == name && value(presence.game_teams, account) == team,
        "presence and account snapshot select consistent fields");
}
void team_tests(uint32_t old_ip, uint32_t new_ip) {
    Sessions sessions;
    auto sender = enter(sessions, "sender", 500, ClientRole::primary);
    const auto sender_game = enter(sessions, "sender", 500, ClientRole::secondary);
    game(sessions, sender_game, 1, "Captain", true, 501);
    auto old_primary = enter(sessions, "peer", old_ip, ClientRole::primary);
    const auto old_game = enter(sessions, "peer", old_ip, ClientRole::secondary);
    game(sessions, old_game, 1, "OldPlayer", true, 101);
    require(sessions.request_game_team(sender, "peer", true), "initial invite accepted");
    require(sessions.game_team_snapshot(old_game).player_name == "Captain", "invite carries sender player name");
    const auto before_switch = sessions.presence();
    require(sessions.leave(old_primary), "old primary leaves");
    require(!sessions.game_team_snapshot(old_game).initialized, "primary leave clears previously queued invite");
    game(sessions, old_game, 4, "OldPlayer", false, 101);
    auto new_primary = enter(sessions, "peer", new_ip, ClientRole::primary);
    const auto after_switch = sessions.presence();
    require(after_switch.revision != before_switch.revision, "primary switch invalidates presence cache");
    require(after_switch.primary_accounts.count("peer") == 1, "presence includes replacement primary account");
    selected(sessions, "peer", false, 0, "", false, 0);
    require(!sessions.request_game_team(sender, "peer", true), "active primary without paired game does not fall back to old IP");
    require(!sessions.game_team_snapshot(old_game).initialized, "migration cannot replay command on old game");
    auto new_game = enter(sessions, "peer", new_ip, ClientRole::secondary);
    game(sessions, new_game, 1, "NewPlayer", true, 202);
    selected(sessions, "peer", true, 1, "NewPlayer", true, 202);
    require(sessions.request_game_team(sender, "peer", true), "invite follows active primary pairing");
    const auto invite = sessions.game_team_snapshot(new_game);
    require(invite.initialized && invite.player_name == "Captain", "invite delivered to selected new game");
    require(!sessions.game_team_snapshot(old_game).initialized, "unselected old game receives no invite");
    require(sessions.request_game_team(sender, "peer", false), "join accepted for peer lobby team");
    const auto join = sessions.game_team_snapshot(sender_game);
    require(join.initialized && join.player_name == "NewPlayer", "join delivered to own game with selected peer name");
    require(sessions.game_team_snapshot(new_game).revision == invite.revision, "join does not enqueue another peer command");

    const auto stale_game = new_game;
    sessions.leave(stale_game);
    new_game = enter(sessions, "peer", new_ip, ClientRole::secondary);
    game(sessions, new_game, 1, "Reconnected", true, 203);
    require(!sessions.game_team_snapshot(new_game).initialized, "reconnected game does not replay old team command");
    require(!sessions.game_team_snapshot(stale_game).initialized, "stale game binding cannot read replacement command");
    sessions.leave(stale_game);
    require(sessions.current(new_game), "late old game disconnect preserves replacement");
    require(sessions.request_game_team(sender, "peer", true), "new request reaches replacement game");

    require(!sessions.request_game_team(sender, "sender", true), "self invite rejected");
    require(!sessions.request_game_team(sender_game, "peer", true), "secondary cannot act as primary sender");
    for (const auto& bad_name : {std::string{}, std::string(chh::wire::game_team_name_limit + 1, 'x'), std::string("a\0b", 3)}) {
        game(sessions, sender_game, 1, bad_name, true, 501);
        require(!sessions.request_game_team(sender, "peer", true), "invite rejects unusable sender name");
        game(sessions, sender_game, 1, "Captain", true, 501);
        game(sessions, new_game, 1, bad_name, true, 203);
        require(!sessions.request_game_team(sender, "peer", false), "join rejects unusable peer name");
    }
    game(sessions, new_game, 1, "Reconnected", true, 203);
    game(sessions, sender_game, 1, "Captain", false, 501);
    require(!sessions.request_game_team(sender, "peer", true), "invite requires sender team");
    game(sessions, sender_game, 3, "Captain", true, 501);
    require(!sessions.request_game_team(sender, "peer", true), "invite requires sender lobby");
    game(sessions, sender_game, 1, "Captain", true, 501);
    game(sessions, new_game, 3, "Reconnected", true, 203);
    require(!sessions.request_game_team(sender, "peer", false), "join requires peer lobby");
    game(sessions, new_game, 1, "Reconnected", false, 203);
    require(!sessions.request_game_team(sender, "peer", false), "join requires peer team");
    game(sessions, new_game, 1, "Reconnected", true, 203);
    const auto stale_sender = sender;
    require(sessions.leave(sender), "sender primary leaves");
    require(!sessions.game_team_snapshot(sender_game).initialized, "primary leave clears queued join command");
    sender = enter(sessions, "sender", 500, ClientRole::primary);
    require(!sessions.request_game_team(stale_sender, "peer", true), "stale primary cannot issue invitation");
    require(sessions.request_game_team(sender, "peer", true), "replacement primary can issue invitation");

    require(sessions.leave(new_primary), "selected peer primary leaves");
    require(sessions.presence().primary_accounts.count("peer") == 0, "presence removes offline primary account");
    require(!sessions.game_team_snapshot(new_game).initialized, "peer primary exit clears pending command");
    selected(sessions, "peer", true, 4, "OldPlayer", false, 101);
    game(sessions, old_game, 2, "OlderToken", false, 101);
    game(sessions, new_game, 2, "NewestToken", true, 203);
    selected(sessions, "peer", true, 2, "NewestToken", true, 203);
    require(sessions.request_game_team(sender, "peer", true), "offline-primary peer remains reachable through selected game");
    require(sessions.game_team_snapshot(new_game).player_name == "Captain" &&
        !sessions.game_team_snapshot(old_game).initialized, "equal states select newest game token independent of IP order");
    new_primary = enter(sessions, "peer", old_ip, ClientRole::primary);
    require(!sessions.game_team_snapshot(new_game).initialized, "primary enter clears invitation queued during primary absence");
    require(sessions.presence().primary_accounts.count("peer") == 1, "presence restores returning primary account");
    selected(sessions, "peer", true, 2, "OlderToken", false, 101);
    std::cout << "PASS team routing with old/new IP order " << old_ip << '/' << new_ip << '\n';
}

void skill_hint_tests() {
    Sessions sessions;
    auto primary = enter(sessions, "viewer", 10, ClientRole::primary);
    const auto other_ip = enter(sessions, "viewer", 20, ClientRole::secondary);
    const auto other_account = enter(sessions, "other", 10, ClientRole::secondary);
    // Initial all-off values are still a command and must initialize a game
    // that starts after the primary has uploaded its controls.
    require(sessions.set_game_skill_hint(primary, 0, 0, 0), "queue initial all-off card before game connects");
    auto own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    auto initial = sessions.skill_hint_command_snapshot(own_game);
    require(initial.initialized && initial.revision && !initial.enabled && !initial.ally && !initial.enemy && !initial.hero_skills,
        "new game receives all-off initialization");
    require(!sessions.skill_hint_command_snapshot(other_ip).initialized &&
        !sessions.skill_hint_command_snapshot(other_account).initialized,
        "initialization is isolated by account and observed IP");
    require(!sessions.skill_hint_command_snapshot(primary).initialized,
        "primary cannot read game-bound command snapshot");

    for (uint32_t bits = 0; bits != 16; ++bits) {
        const uint32_t enabled = (bits >> 3) & 1, ally = (bits >> 2) & 1, enemy = (bits >> 1) & 1, hero_skills = bits & 1;
        chh::wire::Packet packet(chh::wire::Type::game_skill_hint_set);
        require(packet.add_number(enabled) && packet.add_number(ally) && packet.add_number(enemy) && packet.add_number(hero_skills) &&
            packet.valid() && packet.length() == 16, "encode every card combination as a valid 16-byte frame");
        chh::wire::Reader body{packet};
        uint32_t decoded_enabled = 0, decoded_ally = 0, decoded_enemy = 0, decoded_hero_skills = 0;
        require(body.number(decoded_enabled) && body.number(decoded_ally) && body.number(decoded_enemy) &&
            body.number(decoded_hero_skills) && body.done() && decoded_enabled == enabled && decoded_ally == ally &&
            decoded_enemy == enemy && decoded_hero_skills == hero_skills,
            "wire order preserves main, ally, enemy and hero skills flags");
        require(sessions.set_game_skill_hint(primary, decoded_enabled, decoded_ally, decoded_enemy, decoded_hero_skills),
            "accept all sixteen switch combinations");
        const auto command = sessions.skill_hint_command_snapshot(own_game);
        require(command.initialized && command.enabled == enabled && command.ally == ally && command.enemy == enemy && command.hero_skills == hero_skills,
            "paired game receives a consistent complete card");
        require(!bits || command.revision > initial.revision, "changed controls advance command revision");
        require(sessions.set_game_skill_hint(primary, enabled, ally, enemy, hero_skills) &&
            sessions.skill_hint_command_snapshot(own_game).revision == command.revision,
            "repeated initialization does not duplicate unchanged commands");
        require(!sessions.skill_hint_command_snapshot(other_ip).initialized &&
            !sessions.skill_hint_command_snapshot(other_account).initialized,
            "card updates never leak across account or IP");
        initial = command;
    }
    for (const uint32_t invalid : {2u, UINT32_MAX}) {
        require(!sessions.set_game_skill_hint(primary, invalid, 0, 0) &&
            !sessions.set_game_skill_hint(primary, 0, invalid, 0) &&
            !sessions.set_game_skill_hint(primary, 0, 0, invalid) &&
            !sessions.set_game_skill_hint(primary, 0, 0, 0, invalid), "reject non-boolean switch values");
    }
    require(!sessions.set_game_skill_hint(own_game, 0, 0, 0) &&
        !sessions.set_game_skill_hint({}, 0, 0, 0), "reject game-role and empty command senders");
    require(sessions.skill_hint_command_snapshot(own_game).revision == initial.revision,
        "rejected commands leave the current settings intact");
    for (uint32_t count = 0; count != 6; ++count) {
        if (count == 3 || count == 4) continue;
        chh::wire::Packet malformed(chh::wire::Type::game_skill_hint_set);
        for (uint32_t n = 0; n != count; ++n) require(malformed.add_number(0), "build malformed card payload");
        chh::wire::Reader body{malformed}; uint32_t a = 0, b = 0, c = 0, d = 0;
        require(!(body.number(a) && body.number(b) && body.number(c) && (body.done() || body.number(d)) && body.done()),
            "frame reader rejects missing or trailing card values");
    }

    const auto old_game = own_game;
    sessions.leave(old_game);
    require(sessions.set_game_skill_hint(primary, 0, 1, 0, 1), "accept latest card while game is disconnected");
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    const auto reconnected = sessions.skill_hint_command_snapshot(own_game);
    require(reconnected.initialized && !reconnected.enabled && reconnected.ally && !reconnected.enemy && reconnected.hero_skills,
        "reconnected game initializes from the latest complete card");
    require(!sessions.skill_hint_command_snapshot(old_game).initialized,
        "stale game token cannot read replacement initialization");
    sessions.leave(old_game);
    require(sessions.current(own_game), "late game disconnect preserves replacement");
    const auto old_primary = primary;
    require(sessions.leave(old_primary), "primary can leave while paired game remains");
    primary = enter(sessions, "viewer", 10, ClientRole::primary);
    require(!sessions.set_game_skill_hint(old_primary, 1, 1, 1), "stale primary cannot update replacement controls");
    require(sessions.set_game_skill_hint(primary, 1, 0, 1), "replacement primary can initialize current game");
    const auto replaced = sessions.skill_hint_command_snapshot(own_game);
    require(replaced.enabled && !replaced.ally && replaced.enemy && !replaced.hero_skills && replaced.revision > reconnected.revision,
        "replacement primary settings supersede cached settings");
    sessions.leave(primary);
    sessions.leave(own_game);
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    require(!sessions.skill_hint_command_snapshot(own_game).initialized,
        "settings disappear when both endpoints of the pair leave");
    std::cout << "PASS skill hint flags, frame shape, pairing and reconnect lifecycle\n";
}

void status_display_tests() {
    static_assert(uint32_t(chh::wire::Type::game_status_display_set) == 123);
    Sessions sessions;
    auto primary = enter(sessions, "viewer", 10, ClientRole::primary);
    const auto other_ip = enter(sessions, "viewer", 20, ClientRole::secondary);
    const auto other_account = enter(sessions, "other", 10, ClientRole::secondary);
    // Initial disabled defaults are still a command and must initialize a game
    // that starts after the primary has uploaded its controls.
    require(sessions.set_game_status_display(primary, 0, 1, 1), "queue initial disabled card with both team flags enabled before game connects");
    auto own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    auto initial = sessions.status_display_command_snapshot(own_game);
    require(initial.initialized && initial.revision && !initial.enabled && initial.ally && initial.enemy,
        "new game receives disabled initialization with both team flags enabled");
    require(!sessions.status_display_command_snapshot(other_ip).initialized &&
        !sessions.status_display_command_snapshot(other_account).initialized,
        "initialization is isolated by account and observed IP");
    require(!sessions.status_display_command_snapshot(primary).initialized,
        "primary cannot read game-bound command snapshot");

    for (uint32_t bits = 0; bits != 8; ++bits) {
        const uint32_t enabled = (bits >> 2) & 1, ally = (bits >> 1) & 1, enemy = bits & 1;
        chh::wire::Packet packet(chh::wire::Type::game_status_display_set);
        require(packet.add_number(enabled) && packet.add_number(ally) && packet.add_number(enemy) &&
            packet.valid() && packet.length() == 12, "encode every card combination as a valid 12-byte frame");
        chh::wire::Reader body{packet};
        uint32_t decoded_enabled = 0, decoded_ally = 0, decoded_enemy = 0;
        require(body.number(decoded_enabled) && body.number(decoded_ally) && body.number(decoded_enemy) &&
            body.done() && decoded_enabled == enabled && decoded_ally == ally && decoded_enemy == enemy,
            "wire order preserves main, ally and enemy flags");
        require(sessions.set_game_status_display(primary, decoded_enabled, decoded_ally, decoded_enemy),
            "accept all eight switch combinations");
        const auto command = sessions.status_display_command_snapshot(own_game);
        require(command.initialized && command.enabled == enabled && command.ally == ally && command.enemy == enemy,
            "paired game receives a consistent complete card");
        require(command.revision > initial.revision, "changed controls advance command revision");
        require(sessions.set_game_status_display(primary, enabled, ally, enemy) &&
            sessions.status_display_command_snapshot(own_game).revision == command.revision,
            "repeated initialization does not duplicate unchanged commands");
        require(!sessions.status_display_command_snapshot(other_ip).initialized &&
            !sessions.status_display_command_snapshot(other_account).initialized,
            "card updates never leak across account or IP");
        initial = command;
    }
    for (const uint32_t invalid : {2u, UINT32_MAX}) {
        require(!sessions.set_game_status_display(primary, invalid, 0, 0) &&
            !sessions.set_game_status_display(primary, 0, invalid, 0) &&
            !sessions.set_game_status_display(primary, 0, 0, invalid), "reject non-boolean switch values");
    }
    require(!sessions.set_game_status_display(own_game, 0, 0, 0) &&
        !sessions.set_game_status_display({}, 0, 0, 0), "reject game-role and empty command senders");
    require(sessions.status_display_command_snapshot(own_game).revision == initial.revision,
        "rejected commands leave the current settings intact");
    for (uint32_t count = 0; count != 5; ++count) {
        if (count == 3) continue;
        chh::wire::Packet malformed(chh::wire::Type::game_status_display_set);
        for (uint32_t n = 0; n != count; ++n) require(malformed.add_number(0), "build malformed card payload");
        chh::wire::Reader body{malformed}; uint32_t a = 0, b = 0, c = 0;
        require(!(body.number(a) && body.number(b) && body.number(c) && body.done()),
            "frame reader rejects missing or trailing card values");
    }

    const auto old_game = own_game;
    sessions.leave(old_game);
    require(sessions.set_game_status_display(primary, 0, 1, 0), "accept latest card while game is disconnected");
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    const auto reconnected = sessions.status_display_command_snapshot(own_game);
    require(reconnected.initialized && !reconnected.enabled && reconnected.ally && !reconnected.enemy,
        "reconnected game initializes from the latest complete card");
    require(!sessions.status_display_command_snapshot(old_game).initialized,
        "stale game token cannot read replacement initialization");
    sessions.leave(old_game);
    require(sessions.current(own_game), "late game disconnect preserves replacement");
    const auto old_primary = primary;
    require(sessions.leave(old_primary), "primary can leave while paired game remains");
    primary = enter(sessions, "viewer", 10, ClientRole::primary);
    require(!sessions.set_game_status_display(old_primary, 1, 1, 1), "stale primary cannot update replacement controls");
    require(sessions.set_game_status_display(primary, 1, 0, 1), "replacement primary can initialize current game");
    const auto replaced = sessions.status_display_command_snapshot(own_game);
    require(replaced.enabled && !replaced.ally && replaced.enemy && replaced.revision > reconnected.revision,
        "replacement primary settings supersede cached settings");
    sessions.leave(primary);
    sessions.leave(own_game);
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    require(!sessions.status_display_command_snapshot(own_game).initialized,
        "settings disappear when both endpoints of the pair leave");
    std::cout << "PASS status display flags, frame shape, pairing and reconnect lifecycle\n";
}

void auto_smite_tests() {
    static_assert(uint32_t(chh::wire::Type::game_auto_smite_set) == 122);
    Sessions sessions;
    auto primary = enter(sessions, "viewer", 10, ClientRole::primary);
    const auto other_ip = enter(sessions, "viewer", 20, ClientRole::secondary);
    const auto other_account = enter(sessions, "other", 10, ClientRole::secondary);
    // Initial disabled defaults are still a command and must initialize a game
    // that starts after the primary has uploaded its controls.
    require(sessions.set_game_auto_smite(primary, 0, 1, 1, 0), "queue initial disabled card with monster catcher off before game connects");
    auto own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    auto initial = sessions.auto_smite_command_snapshot(own_game);
    require(initial.initialized && initial.revision && !initial.enabled && initial.dragon && initial.buffs && !initial.monster_catcher,
        "new game receives disabled initialization with monster catcher off");
    require(!sessions.auto_smite_command_snapshot(other_ip).initialized &&
        !sessions.auto_smite_command_snapshot(other_account).initialized,
        "initialization is isolated by account and observed IP");
    require(!sessions.auto_smite_command_snapshot(primary).initialized,
        "primary cannot read game-bound command snapshot");

    require(sessions.set_game_auto_smite(primary, 0, 1, 1, 1), "changing only monster catcher accepted");
    const auto catcher_only = sessions.auto_smite_command_snapshot(own_game);
    require(catcher_only.revision > initial.revision && !catcher_only.enabled && catcher_only.dragon &&
        catcher_only.buffs && catcher_only.monster_catcher, "monster-catcher-only change advances complete card revision");
    initial = catcher_only;
    for (uint32_t bits = 0; bits != 16; ++bits) {
        const uint32_t enabled = (bits >> 3) & 1, dragon = (bits >> 2) & 1, buffs = (bits >> 1) & 1;
        const uint32_t monster_catcher = bits & 1;
        chh::wire::Packet packet(chh::wire::Type::game_auto_smite_set);
        require(packet.add_number(enabled) && packet.add_number(dragon) && packet.add_number(buffs) &&
            packet.add_number(monster_catcher) && packet.valid() && packet.length() == 16,
            "encode every card combination as a valid 16-byte frame");
        chh::wire::Reader body{packet};
        uint32_t decoded_enabled = 0, decoded_dragon = 0, decoded_buffs = 0, decoded_monster_catcher = 0;
        require(body.number(decoded_enabled) && body.number(decoded_dragon) && body.number(decoded_buffs) &&
            body.number(decoded_monster_catcher) && body.done() && decoded_enabled == enabled &&
            decoded_dragon == dragon && decoded_buffs == buffs && decoded_monster_catcher == monster_catcher,
            "wire order preserves main, dragon, buffs and monster catcher flags");
        require(sessions.set_game_auto_smite(primary, decoded_enabled, decoded_dragon, decoded_buffs, decoded_monster_catcher),
            "accept all sixteen switch combinations");
        const auto command = sessions.auto_smite_command_snapshot(own_game);
        require(command.initialized && command.enabled == enabled && command.dragon == dragon && command.buffs == buffs &&
            command.monster_catcher == monster_catcher,
            "paired game receives a consistent complete card");
        require(command.revision > initial.revision, "changed auto-smite controls advance command revision");
        require(sessions.set_game_auto_smite(primary, enabled, dragon, buffs, monster_catcher) &&
            sessions.auto_smite_command_snapshot(own_game).revision == command.revision,
            "repeated initialization does not duplicate unchanged commands");
        require(!sessions.auto_smite_command_snapshot(other_ip).initialized &&
            !sessions.auto_smite_command_snapshot(other_account).initialized,
            "card updates never leak across account or IP");
        initial = command;
    }
    for (const uint32_t invalid : {2u, UINT32_MAX}) {
        require(!sessions.set_game_auto_smite(primary, invalid, 0, 0, 0) &&
            !sessions.set_game_auto_smite(primary, 0, invalid, 0, 0) &&
            !sessions.set_game_auto_smite(primary, 0, 0, invalid, 0) &&
            !sessions.set_game_auto_smite(primary, 0, 0, 0, invalid), "reject non-boolean switch values in all four fields");
    }
    require(!sessions.set_game_auto_smite(own_game, 0, 0, 0, 0) &&
        !sessions.set_game_auto_smite({}, 0, 0, 0, 0), "reject game-role and empty command senders");
    require(sessions.auto_smite_command_snapshot(own_game).revision == initial.revision,
        "rejected commands leave the current settings intact");
    for (uint32_t count = 0; count != 6; ++count) {
        if (count == 4) continue;
        chh::wire::Packet malformed(chh::wire::Type::game_auto_smite_set);
        for (uint32_t n = 0; n != count; ++n) require(malformed.add_number(0), "build malformed card payload");
        chh::wire::Reader body{malformed}; uint32_t a = 0, b = 0, c = 0, d = 0;
        require(!(body.number(a) && body.number(b) && body.number(c) && body.number(d) && body.done()),
            "frame reader rejects old twelve-byte, incomplete and trailing card values");
    }

    const auto old_game = own_game;
    sessions.leave(old_game);
    require(sessions.set_game_auto_smite(primary, 0, 1, 0, 1), "accept latest card while game is disconnected");
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    const auto reconnected = sessions.auto_smite_command_snapshot(own_game);
    require(reconnected.initialized && !reconnected.enabled && reconnected.dragon && !reconnected.buffs && reconnected.monster_catcher,
        "reconnected game initializes from latest card including monster catcher");
    require(!sessions.auto_smite_command_snapshot(old_game).initialized,
        "stale game token cannot read replacement initialization");
    sessions.leave(old_game);
    require(sessions.current(own_game), "late game disconnect preserves replacement");
    const auto old_primary = primary;
    require(sessions.leave(old_primary), "primary can leave while paired game remains");
    primary = enter(sessions, "viewer", 10, ClientRole::primary);
    require(!sessions.set_game_auto_smite(old_primary, 1, 1, 1, 1), "stale primary cannot update replacement controls");
    require(sessions.set_game_auto_smite(primary, 1, 0, 1, 0), "replacement primary can initialize current game");
    const auto replaced = sessions.auto_smite_command_snapshot(own_game);
    require(replaced.enabled && !replaced.dragon && replaced.buffs && !replaced.monster_catcher && replaced.revision > reconnected.revision,
        "replacement primary settings supersede cached settings");
    sessions.leave(primary);
    sessions.leave(own_game);
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    require(!sessions.auto_smite_command_snapshot(own_game).initialized,
        "settings disappear when both endpoints of the pair leave");
    std::cout << "PASS auto smite flags, frame shape, pairing and reconnect lifecycle\n";
}

void extra_action_tests() {
    using Action = chh::wire::GameExtraAction;
    const uint32_t talent = uint32_t(Action::talent), fate = uint32_t(Action::fate);
    const uint32_t switch_account = uint32_t(Action::switch_account);
    require(uint32_t(chh::wire::Type::game_extra_action) == 121 &&
        talent == 1 && fate == 2 && switch_account == 3, "button actions retain their agreed wire mapping");
    Sessions sessions;
    auto primary = enter(sessions, "viewer", 10, ClientRole::primary);
    auto own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    const auto other_ip = enter(sessions, "viewer", 20, ClientRole::secondary);
    const auto other_account = enter(sessions, "other", 10, ClientRole::secondary);
    require(sessions.game_snapshot(primary).game_connection_id == own_game.token &&
        sessions.game_snapshot("viewer").game_connection_id == own_game.token,
        "status snapshots expose the currently paired game connection token");
    const std::vector<uint32_t> clicks{talent, fate, fate, switch_account, talent};
    for (const auto action : clicks) {
        chh::wire::Packet packet(chh::wire::Type::game_extra_action);
        require(packet.add_number(action) && packet.add_u64(own_game.token) && packet.valid() && packet.length() == 12,
            "primary encodes action and target connection as a twelve-byte frame");
        chh::wire::Reader body{packet}; uint32_t decoded = 0;
        uint64_t target_game_connection_id = 0;
        require(body.number(decoded) && body.u64(target_game_connection_id) && body.done() &&
            decoded == action && target_game_connection_id == own_game.token &&
            chh::wire::valid_game_extra_action(decoded), "button action frame round trips");
        require(sessions.request_game_extra_action(primary, decoded, target_game_connection_id),
            "current primary can enqueue each button click");
        chh::wire::Packet forwarded(chh::wire::Type::game_extra_action);
        require(forwarded.add_number(decoded) && forwarded.length() == 4 && forwarded.number() == action,
            "server forwards only action to the targeted game");
    }
    require(sessions.take_game_extra_actions(primary).empty() &&
        sessions.take_game_extra_actions({}).empty(), "only current secondary may consume queued actions");
    require(sessions.take_game_extra_actions(other_ip).empty() &&
        sessions.take_game_extra_actions(other_account).empty(), "actions remain isolated by account and observed IP");
    require(sessions.take_game_extra_actions(own_game) == clicks,
        "actions retain FIFO order and duplicate fate toggles");
    require(sessions.take_game_extra_actions(own_game).empty(), "button actions are delivered only once");
    for (const auto invalid : {0u, 4u, UINT32_MAX}) {
        require(!chh::wire::valid_game_extra_action(invalid) &&
            !sessions.request_game_extra_action(primary, invalid, own_game.token), "unknown extra action rejected");
    }
    require(!sessions.request_game_extra_action(own_game, talent, own_game.token) &&
        !sessions.request_game_extra_action({}, talent, own_game.token), "secondary and empty sender cannot issue actions");
    require(!sessions.request_game_extra_action(primary, talent, 0) &&
        !sessions.request_game_extra_action(primary, talent, other_ip.token) &&
        !sessions.request_game_extra_action(primary, talent, other_account.token),
        "zero and unrelated connection tokens cannot target this pair");
    for (const auto count : {0u, 1u, 2u, 4u}) {
        chh::wire::Packet packet(chh::wire::Type::game_extra_action);
        for (uint32_t n = 0; n < count; ++n) require(packet.add_number(talent), "build malformed action frame");
        chh::wire::Reader body{packet}; uint32_t decoded = 0; uint64_t target = 0;
        require(!(body.number(decoded) && body.u64(target) && body.done()), "missing and trailing action values rejected");
    }
    require(sessions.take_game_extra_actions(own_game).empty(), "invalid requests do not queue an action");

    for (unsigned n = 0; n < 32; ++n)
        require(sessions.request_game_extra_action(primary, fate, own_game.token), "accept action up to per-pair queue limit");
    require(!sessions.request_game_extra_action(primary, talent, own_game.token) && sessions.current_primary(primary),
        "full queue ignores new action while primary stays connected");
    require(sessions.take_game_extra_actions(own_game) == std::vector<uint32_t>(32, fate),
        "full queue retains existing clicks in order");
    require(sessions.request_game_extra_action(primary, switch_account, own_game.token), "drained queue accepts fresh action");

    require(sessions.set_game_skill_hint(primary, 1, 1, 0) && sessions.set_game_jungle_timer(primary, 1),
        "existing controls initialized before lifecycle checks");
    const auto old_game = own_game;
    sessions.leave(old_game);
    require(sessions.game_snapshot(primary).game_connection_id == 0,
        "disconnected game has no target connection token");
    require(!sessions.request_game_extra_action(primary, fate, own_game.token) && sessions.current_primary(primary),
        "game exit race drops click without disconnecting primary");
    own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
    require(sessions.game_snapshot(primary).game_connection_id == own_game.token && own_game.token != old_game.token,
        "rapid game reconnect publishes a distinct target token");
    require(!sessions.request_game_extra_action(primary, fate, old_game.token) && sessions.current_primary(primary),
        "delayed LA click cannot target replacement game even if LA missed disconnected status");
    require(sessions.take_game_extra_actions(own_game).empty(), "game reconnect does not replay pending clicks");
    require(sessions.request_game_extra_action(primary, talent, own_game.token), "new game accepts fresh click");
    require(sessions.take_game_extra_actions(old_game).empty(), "stale game cannot consume replacement action");
    sessions.leave(old_game);
    require(sessions.take_game_extra_actions(own_game) == std::vector<uint32_t>{talent},
        "stale game disconnect preserves replacement action");
    const auto hints = sessions.skill_hint_command_snapshot(own_game);
    const auto timer = sessions.jungle_timer_command_snapshot(own_game);
    require(hints.initialized && hints.enabled && hints.ally && !hints.enemy && timer.initialized && timer.enabled,
        "game reconnect retains skill hint and jungle timer settings");

    require(sessions.request_game_extra_action(primary, fate, own_game.token), "queue click before primary disconnect");
    const auto old_primary = primary;
    require(sessions.leave(old_primary), "primary disconnect leaves game active");
    require(sessions.take_game_extra_actions(own_game).empty(), "primary disconnect clears pending clicks");
    primary = enter(sessions, "viewer", 10, ClientRole::primary);
    require(sessions.take_game_extra_actions(own_game).empty(), "primary reconnect does not replay old click");
    require(!sessions.request_game_extra_action(old_primary, fate, own_game.token), "stale primary cannot issue fresh clicks");
    require(sessions.request_game_extra_action(primary, switch_account, own_game.token), "replacement primary can issue click");
    sessions.leave(old_primary);
    require(sessions.take_game_extra_actions(own_game) == std::vector<uint32_t>{switch_account},
        "stale primary disconnect preserves replacement click");
    require(sessions.request_game_extra_action(primary, talent, own_game.token), "queue click before primary IP change");
    require(sessions.leave(primary), "old primary IP leaves");
    primary = enter(sessions, "viewer", 30, ClientRole::primary);
    require(!sessions.request_game_extra_action(primary, talent, own_game.token), "new primary without own game never falls back to old IP");
    require(sessions.take_game_extra_actions(own_game).empty() && sessions.take_game_extra_actions(other_ip).empty(),
        "IP change cannot replay a click to a previous game");
    const auto new_game = enter(sessions, "viewer", 30, ClientRole::secondary);
    require(sessions.take_game_extra_actions(new_game).empty(), "newly paired game has no old button actions");
    require(sessions.request_game_extra_action(primary, fate, new_game.token) &&
        sessions.take_game_extra_actions(new_game) == std::vector<uint32_t>{fate}, "new click follows current same-IP pair");
    std::cout << "PASS extra action mapping, FIFO, limits, pairing and disconnect lifecycle\n";
}

struct StatsFixture {
    Sessions sessions;
    Sessions::Binding primary, own_game, target;
    uint32_t request_id = 0;
    Clock::time_point requested;
    explicit StatsFixture(bool deliver = true) {
        primary = enter(sessions, "viewer", 10, ClientRole::primary);
        own_game = enter(sessions, "viewer", 10, ClientRole::secondary);
        game(sessions, own_game, 3, "Viewer", false, 100);
        enter(sessions, "relay", 20, ClientRole::primary);
        target = enter(sessions, "relay", 20, ClientRole::secondary);
        game(sessions, target, 1, "Relay", false, 200);
        Game::Player player;
        player.uid = 300; player.identification_code = 1234; player.name = "Observed";
        requested = Clock::now();
        require(sessions.update_game_players(own_game, {player}), "player refresh queues stats request");
        require(sessions.take_game_stats_results(primary).empty(), "request starts pending");
        if (deliver) {
            const auto requests = sessions.take_game_stats_requests(target);
            require(requests.size() == 1 && requests[0].targets.size() == 1 && requests[0].targets[0].uid == 300 &&
                requests[0].targets[0].identification_code == 1234, "request routed to connected lobby game");
            request_id = requests[0].request_id;
            require(request_id != 0 && sessions.take_game_stats_requests(target).empty(), "stats request delivered once");
        }
    }
    Game::StatsEntry success() const {
        Game::StatsEntry entry;
        entry.request_id = request_id; entry.uid = 300;
        entry.result = static_cast<uint32_t>(chh::wire::GameStatsResult::ok); entry.level = 17;
        return entry;
    }
    void heartbeat() { require(sessions.update_game_memory(target, 123456), "connected relay heartbeat remains accepted"); }
};
void one_result(const std::vector<Game::StatsBatch>& batches, uint32_t request_id, bool success, const char* label) {
    require(batches.size() == 1 && batches[0].request_id != 0 &&
        (!request_id || batches[0].request_id == request_id) && batches[0].entries.size() == 1 &&
        batches[0].entries[0].uid == 300 && batches[0].entries[0].request_id == batches[0].request_id &&
        batches[0].entries[0].result == static_cast<uint32_t>(success ? chh::wire::GameStatsResult::ok : chh::wire::GameStatsResult::unavailable), label);
}
void completed_batch_replacement_test() {
    StatsFixture fixture;
    require(fixture.sessions.complete_game_stats(fixture.target, fixture.request_id, {fixture.success()}), "complete before primary replacement");
    const auto old_primary = fixture.primary;
    require(fixture.sessions.leave(old_primary), "completed batch requester leaves");
    fixture.primary = enter(fixture.sessions, "viewer", 10, ClientRole::primary);
    require(fixture.sessions.take_game_stats_results(fixture.primary).empty(), "replacement primary does not inherit completed old batch");
    require(fixture.sessions.take_game_stats_results(old_primary).empty(), "stale primary cannot consume old completed batch");
    std::cout << "PASS completed result cleared across primary replacement\n";
}
void timeout_tests() {
    StatsFixture polled, overdue_reply, unsent(false), replaced, early_success, disconnected;
    const auto replaced_primary = replaced.primary;
    require(replaced.sessions.leave(replaced_primary), "pending requester leaves");
    replaced.primary = enter(replaced.sessions, "viewer", 10, ClientRole::primary);
    require(early_success.sessions.complete_game_stats(early_success.target, early_success.request_id, {early_success.success()}), "timely success accepted");
    one_result(early_success.sessions.take_game_stats_results(early_success.primary), early_success.request_id, true, "success before timeout returned once");
    disconnected.sessions.leave(disconnected.target);
    one_result(disconnected.sessions.take_game_stats_results(disconnected.primary), disconnected.request_id, false, "target disconnect immediately returns unavailable");
    const auto disconnected_target = disconnected.target;
    disconnected.target = enter(disconnected.sessions, "relay", 20, ClientRole::secondary);
    game(disconnected.sessions, disconnected.target, 1, "RelayAgain", false, 200);
    require(disconnected.sessions.take_game_stats_requests(disconnected.target).empty(), "target reconnect cannot replay lost stats request");

    // Independent fixtures share one real wall-clock wait. Only the ordinary
    // worker is polled; other routes first see expiry through their own APIs.
    const auto deadline = Clock::now() + std::chrono::seconds(61);
    std::vector<Game::StatsBatch> polled_batches;
    long long first_expiry_ms = -1;
    std::cout << "Waiting 61 seconds for real stats deadlines; polling and heartbeats continue.\n" << std::flush;
    while (Clock::now() < deadline) {
        polled.heartbeat(); overdue_reply.heartbeat(); unsent.heartbeat(); replaced.heartbeat();
        auto batches = polled.sessions.take_game_stats_results(polled.primary);
        if (!batches.empty()) require(Clock::now() - polled.requested >= std::chrono::seconds(60), "ordinary poll does not expire request early");
        if (!batches.empty() && first_expiry_ms < 0)
            first_expiry_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - polled.requested).count();
        for (auto& batch : batches) polled_batches.push_back(std::move(batch));
        std::this_thread::sleep_until((std::min)(deadline, Clock::now() + std::chrono::milliseconds(200)));
    }
    auto remaining = polled.sessions.take_game_stats_results(polled.primary);
    if (!remaining.empty() && first_expiry_ms < 0)
        first_expiry_ms = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - polled.requested).count();
    for (auto& batch : remaining) polled_batches.push_back(std::move(batch));
    one_result(polled_batches, polled.request_id, false, "ordinary polls expire route despite healthy heartbeat and no player refresh");
    require(polled.sessions.complete_game_stats(polled.target, polled.request_id, {polled.success()}), "late polled reply safely ignored");
    require(polled.sessions.take_game_stats_results(polled.primary).empty(), "expired polled reply creates no duplicate result");

    require(overdue_reply.sessions.complete_game_stats(overdue_reply.target, overdue_reply.request_id, {overdue_reply.success()}), "overdue reply handled before any result poll");
    one_result(overdue_reply.sessions.take_game_stats_results(overdue_reply.primary), overdue_reply.request_id, false, "completion itself enforces deadline and rejects late success");
    require(overdue_reply.sessions.complete_game_stats(overdue_reply.target, overdue_reply.request_id, {overdue_reply.success()}), "second overdue reply ignored");
    require(overdue_reply.sessions.take_game_stats_results(overdue_reply.primary).empty(), "overdue reply cannot duplicate unavailable batch");

    require(unsent.sessions.take_game_stats_requests(unsent.target).empty(), "expired unsent request never reaches game worker");
    one_result(unsent.sessions.take_game_stats_results(unsent.primary), 0, false, "request queue polling expires unsent route");
    require(unsent.sessions.take_game_stats_requests(unsent.target).empty() && unsent.sessions.take_game_stats_results(unsent.primary).empty(), "expired unsent queue and result are drained once");
    require(replaced.sessions.complete_game_stats(replaced.target, replaced.request_id, {replaced.success()}), "replaced requester's late reply ignored");
    require(replaced.sessions.take_game_stats_results(replaced.primary).empty() && replaced.sessions.take_game_stats_results(replaced_primary).empty(), "pending timeout does not leak into replacement primary");
    require(early_success.sessions.take_game_stats_results(early_success.primary).empty(), "completed success does not later time out");
    require(disconnected.sessions.complete_game_stats(disconnected_target, disconnected.request_id, {disconnected.success()}), "disconnected target's stale reply ignored");
    require(disconnected.sessions.take_game_stats_results(disconnected.primary).empty(), "disconnect failure is not duplicated by timeout");
    std::cout << "PASS real-time stats expiry, stale results and reconnect lifecycle; first expiry: " << first_expiry_ms << " ms\n";
}
} // namespace

int main(int argc, char** argv) {
    try {
        if (argc == 2 && std::string(argv[1]) == "--extra-actions-only") extra_action_tests();
        else if (argc == 2 && std::string(argv[1]) == "--auto-smite-only") auto_smite_tests();
        else if (argc == 2 && std::string(argv[1]) == "--status-display-only") status_display_tests();
        else if (argc == 2 && std::string(argv[1]) == "--skill-hint-only") skill_hint_tests();
        else {
            team_tests(100, 200);
            team_tests(200, 100);
            skill_hint_tests();
            status_display_tests();
            auto_smite_tests();
            extra_action_tests();
            completed_batch_replacement_test();
            timeout_tests();
        }
        std::cout << "RESULT " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
