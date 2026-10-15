#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <chrono>
#include <deque>
#include <iterator>
#include <map>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace chh::server {
enum class ClientRole { primary, secondary };
// The observed peer IP is supplied by accept(), never by the client payload.
class Sessions {
public:
    using Key = std::pair<std::string, uint32_t>;
    struct Binding {
        Key key;
        uint64_t token = 0;
        ClientRole role = ClientRole::primary;
        explicit operator bool() const { return token != 0; }
    };
    struct Snapshot {
        uint64_t revision = 0;
        uint32_t accounts = 0, primary = 0, secondary = 0;
    };
    struct Connection {
        ClientRole role = ClientRole::primary;
        std::string name;
        // Diagnostic value only; Sessions never owns or closes a socket.
        uintptr_t socket = 0;
        uint32_t ip = 0; // Observed peer IPv4 address, in network byte order.
        uint64_t connection_token = 0;
    };
    struct ConnectionsSnapshot {
        Snapshot count;
        std::vector<Connection> clients;
    };
    enum class ExplorerQueueResult { queued, not_found, not_ready, busy };
    struct ExplorerQueued {
        ExplorerQueueResult result = ExplorerQueueResult::not_found;
        uint64_t request_id = 0;
    };
    // Status fields the primary loop and team actions read on every pass.
    // Kept separate so polling does not copy player lists and stats queues.
    struct GameStatus {
        uint64_t revision = 0;
        uint64_t game_connection_id = 0;
        uint32_t uid = 0, state = 0;
        uint32_t map_id = 0, mode = 0;
        bool team = false;
        bool connected = false;
        // Available virtual memory reported by the secondary client.  This is
        // kept with the game status so the matching primary receives a single
        // consistent snapshot on every change.
        uint64_t available_virtual_memory = 0;
        // Player name parsed from the game's 0x5BDE login packet.  It follows
        // the current game account/region and is never used as an identity.
        std::string player_name;
    };
    struct Game : GameStatus {
        struct Player {
            uint32_t uid = 0;
            uint32_t team = 0;
            uint32_t hero_id = 0;
            uint32_t damage_dealt = 0;
            uint32_t damage_taken = 0;
            uint32_t level = 0;
            uint32_t vip = 0;
            uint32_t competitive_power = 0;
            // The game-server player identifier (Packet+35 in 0x0563).
            // It is used only as an opaque query token by the stats relay.
            uint32_t identification_code = 0;
            std::string name;
        };
        struct StatsTarget {
            uint32_t uid = 0;
            uint32_t identification_code = 0;
        };
        struct StatsRequest {
            uint32_t request_id = 0;
            std::vector<StatsTarget> targets;
        };
        struct StatsMatch {
            uint32_t hero_id = 0;
            uint32_t result = 0;
        };
        struct StatsMode {
            uint32_t win_rate_x100 = 0;
            uint32_t match_count = 0;
            std::array<StatsMatch, chh::wire::game_stats_recent_limit> recent{};
        };
        struct StatsEntry {
            uint32_t request_id = 0;
            uint32_t uid = 0;
            uint32_t result = static_cast<uint32_t>(chh::wire::GameStatsResult::unavailable);
            uint32_t level = 0, vip = 0, competitive_power = 0;
            std::array<StatsMode, chh::wire::game_stats_mode_count> modes{};
        };
        struct StatsBatch {
            uint32_t request_id = 0;
            std::vector<StatsEntry> entries;
        };
        // SightI is initialized by the game endpoint after LoadInit().  The
        // two independent revisions let each connection poll only the
        // direction intended for it: game_sight_revision is sent to the
        // primary, sight_command_revision is sent back to the game endpoint.
        uint64_t sight_revision = 0, sight_command_revision = 0;
        uint64_t skill_hint_command_revision = 0;
        uint32_t skill_hint_enabled = 0, skill_hint_ally = 0, skill_hint_enemy = 0, skill_hint_hero_skills = 0;
        uint64_t status_display_command_revision = 0;
        uint32_t status_display_enabled = 0, status_display_ally = 1, status_display_enemy = 1;
        uint64_t auto_cleanse_command_revision = 0;
        uint64_t auto_disarm_command_revision = 0;
        uint32_t auto_disarm_enabled = 0, auto_disarm_on_skill = 1, auto_disarm_direct = 1;
        uint32_t auto_disarm_range_bits = chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default);
        chh::wire::GameAutoDisarmRoster auto_disarm_roster;
        uint64_t auto_disarm_roster_revision = 0, auto_disarm_exclusions_revision = 0;
        uint64_t auto_disarm_request_id = 0;
        bool auto_disarm_roster_fresh = false;
        uint32_t auto_cleanse_enabled = 0, auto_cleanse_ignite = 1, auto_cleanse_disarm = 1;
        uint32_t auto_cleanse_root = 1, auto_cleanse_control = 1, auto_cleanse_silence = 1, auto_cleanse_stun = 1;
        uint32_t auto_cleanse_taunt = 1;
        uint32_t auto_cleanse_delay_ms = 300, auto_cleanse_min_count = 0;
        uint32_t auto_cleanse_control_time_ms = 0;
        uint64_t auto_guisuo_command_revision = 0;
        uint32_t auto_guisuo_enabled = 0, auto_guisuo_min_count = chh::wire::game_auto_guisuo_min_count_default;
        struct AutoGuisuoTarget {
            chh::wire::GameAutoDisarmRoster roster;
            uint64_t roster_revision = 0, exclusions_revision = 0, request_id = 0;
            bool roster_fresh = false;
        };
        std::array<AutoGuisuoTarget, chh::wire::game_auto_guisuo_side_count> auto_guisuo_targets;
        uint64_t auto_dihuo_command_revision = 0;
        uint32_t auto_dihuo_enabled = 0, auto_dihuo_range_bits = chh::wire::game_auto_dihuo_range_to_bits(chh::wire::game_auto_dihuo_range_default);
        chh::wire::GameAutoDisarmRoster auto_dihuo_roster;
        uint64_t auto_dihuo_roster_revision = 0, auto_dihuo_exclusions_revision = 0, auto_dihuo_request_id = 0;
        bool auto_dihuo_roster_fresh = false;
        uint64_t auto_douji_command_revision = 0;
        uint32_t auto_douji_flags = chh::wire::game_auto_douji_flags_default, auto_douji_range_bits = chh::wire::game_auto_douji_range_to_bits(chh::wire::game_auto_douji_range_default);
        chh::wire::GameAutoDisarmRoster auto_douji_roster;
        uint64_t auto_douji_roster_revision = 0, auto_douji_exclusions_revision = 0, auto_douji_request_id = 0;
        bool auto_douji_roster_fresh = false;
        uint64_t auto_dark_shield_command_revision = 0;
        uint32_t auto_dark_shield_enabled = 0, auto_dark_shield_min_count = chh::wire::game_auto_dark_shield_min_count_default;
        uint32_t auto_dark_shield_use_dark_shield = 1, auto_dark_shield_use_dining_table = 0;
        chh::wire::GameAutoDisarmRoster auto_dark_shield_roster;
        uint64_t auto_dark_shield_roster_revision = 0, auto_dark_shield_exclusions_revision = 0, auto_dark_shield_request_id = 0;
        bool auto_dark_shield_roster_fresh = false;
        uint64_t auto_heal_command_revision = 0;
        uint32_t auto_heal_enabled = 0, auto_heal_health_percent = chh::wire::game_auto_heal_health_default;
        chh::wire::GameAutoDisarmRoster auto_heal_roster;
        uint64_t auto_heal_roster_revision = 0, auto_heal_exclusions_revision = 0, auto_heal_request_id = 0;
        bool auto_heal_roster_fresh = false;
        struct NearItem {
            uint64_t command_revision = 0;
            uint32_t enabled = 0, direct = 1, on_attack = 0, on_attacked = 0;
            uint32_t range_bits = chh::wire::game_near_item1_range_to_bits(chh::wire::game_near_item1_range_default);
            uint32_t health_percent = chh::wire::game_near_item1_health_default;
            chh::wire::GameAutoDisarmRoster roster;
            uint64_t roster_revision = 0, exclusions_revision = 0, request_id = 0;
            bool roster_fresh = false;
        };
        std::array<NearItem, chh::wire::game_near_item_count> near_items;
        uint64_t training_mode_command_revision = 0;
        uint32_t training_mode_enabled = 0, training_mode_unlimited_cooldown = 0, training_mode_attack_dummy = 0;
        uint64_t auto_smite_command_revision = 0;
        uint32_t auto_smite_enabled = 0, auto_smite_dragon = 1, auto_smite_buffs = 1;
        uint32_t auto_smite_monster_catcher = 0;
        uint64_t jungle_timer_command_revision = 0;
        uint32_t jungle_timer_enabled = 0;
        uint64_t eye_timer_command_revision = 0;
        uint32_t eye_timer_enabled = 0;
        uint64_t trap_timer_command_revision = 0;
        uint32_t trap_timer_enabled = 0;
        uint64_t disappearance_hint_command_revision = 0;
        uint32_t disappearance_hint_enabled = 0, disappearance_hint_duration_seconds = 30, disappearance_hint_show_heroes = 0;
        uint64_t movement_trace_command_revision = 0;
        uint32_t movement_trace_enabled = 0;
        uint64_t tower_range_command_revision = 0;
        uint32_t tower_range_enabled = 0;
        uint64_t attack_range_command_revision = 0;
        uint32_t attack_range_enabled = 0;
        uint64_t auto_afk_command_revision = 0;
        uint32_t auto_afk_enabled = 0;
        uint64_t move_distance_command_revision = 0;
        uint32_t move_distance_bits = chh::wire::game_move_distance_to_bits(chh::wire::game_move_distance_default);
        uint64_t hero_keys_command_revision = 0;
        uint32_t hero_combo_key = chh::wire::game_hero_combo_key_default;
        uint32_t hero_toggle_key = chh::wire::game_hero_toggle_key_default;
        uint32_t hero_lane_key = chh::wire::game_hero_lane_key_default;
        uint64_t hero_delay_command_revision = 0;
        uint32_t hero_delay_ms = chh::wire::game_hero_delay_default_ms;
        uint64_t skin_block_command_revision = 0;
        uint32_t skin_block_enabled = 0;
        uint64_t projectile_display_command_revision = 0;
        uint32_t projectile_display_enabled = 0, projectile_display_enemy = 1, projectile_display_ally = 0;
        uint64_t auto_dodge_command_revision = 0;
        uint32_t auto_dodge_enabled = 0, auto_dodge_rectangles = 1, auto_dodge_circles = 1;
        uint64_t low_health_hint_command_revision = 0;
        uint32_t low_health_hint_enabled = 0, low_health_hint_threshold_percent = 30;
        uint64_t instant_pick_command_revision = 0;
        uint32_t instant_pick_enabled = 0;
        uint64_t auto_accept_command_revision = 0;
        uint32_t auto_accept_enabled = 0;
        uint64_t selection_shout_command_revision = 0;
        uint32_t selection_shout_enabled = 0;
        std::string selection_shout_text;
        uint64_t auto_ignite_command_revision = 0;
        uint32_t auto_ignite_enabled = 0;
        uint64_t auto_fairy_command_revision = 0, auto_devour_command_revision = 0;
        uint32_t auto_fairy_enabled = 0;
        uint32_t auto_devour_enabled = 0, auto_devour_minions = 1;
        uint64_t auto_glove_command_revision = 0;
        uint32_t auto_glove_enabled = 0;
        uint64_t auto_lingzhou_command_revision = 0;
        uint32_t auto_lingzhou_enabled = 0;
        uint64_t hero_select_command_revision = 0;
        uint32_t hero_select_id = 0;
        // One-shot close command from the primary.  This revision is reset
        // when the game endpoint leaves so a later game process is never
        // terminated by an old request.
        uint64_t close_command_revision = 0;
        // Button actions preserve click order, including repeated toggle clicks.
        // They belong only to the current primary/game pair and are never replayed.
        std::deque<uint32_t> extra_actions;
        struct SkinAction { uint32_t action = 0, skin_id = 0; };
        std::deque<SkinAction> skin_actions;
        uint64_t team_command_revision = 0;
        std::string team_command_name;
        uint64_t players_request_revision = 0;
        uint64_t players_revision = 0;
        std::vector<Player> players;
        uint64_t output_request_revision = 0;
        uint64_t output_revision = 0;
        std::vector<Player> output_players;
        std::deque<StatsRequest> stats_requests;
        std::deque<StatsBatch> stats_results;
        uint32_t sight = 0, sight_command = 0;
        bool sight_initialized = false;
    };
    struct Presence {
        uint64_t revision = 0, game_revision = 0;
        std::set<std::string> primary_accounts;
        std::map<std::string, uint32_t> game_states;
        std::map<std::string, bool> game_connected;
        std::map<std::string, bool> game_teams;
        std::map<std::string, std::string> game_player_names;
    };
    struct Profile {
        uint64_t revision = 0;
        std::string name;
        bool preferences_ready = false, hide_account_id = true;
    };
    struct PrivateClear {
        uint64_t revision = 0, through = 0;
        std::string peer;
    };
    struct PrivateClears {
        uint64_t revision = 0;
        bool reset = false;
        std::vector<PrivateClear> changes;
    };
    struct PrivateRecall {
        uint64_t revision = 0, id = 0;
        uint32_t timestamp = 0;
        std::string peer;
    };
    struct PrivateRecalls {
        uint64_t revision = 0;
        bool reset = false;
        std::vector<PrivateRecall> changes;
    };
    Binding enter(const std::string& account_id, uint32_t peer_ip, ClientRole role,
        const std::string& name = {}, uintptr_t socket = 0) {
        std::lock_guard lock(mutex_);
        if (role != ClientRole::primary && role != ClientRole::secondary) return {};
        if (role == ClientRole::primary)
            for (const auto& [existing_key, existing] : pairs_)
                if (existing_key.first == account_id && existing.primary) return {};
        const Key key{account_id, peer_ip};
        auto& pair = pairs_[key];
        if (role == ClientRole::primary ? pair.primary != 0 : pair.secondary != 0) return {};
        Binding binding{key, ++next_token_, role};
        if (!name.empty()) { pair.profile.revision = ++profile_revision_; pair.profile.name = name; }
        if (binding.role == ClientRole::primary) {
            clear_team_commands_locked(account_id);
            pair.primary = binding.token;
            pair.primary_socket = socket;
            pair.explorer = {};
            ++primary_revision_;
        }
        else {
            pair.secondary = binding.token;
            pair.secondary_socket = socket;
            // The game endpoint is connected before the player logs into the
            // game. Keep state 0 for this phase; state 1 (lobby) is reported
            // after Lua confirms that the player has entered the hall.
            reset_game_locked(pair.game, true);
        }
        ++revision_;
        return binding;
    }
    bool leave(const Binding& binding) {
        if (!binding) return false;
        std::lock_guard lock(mutex_);
        const auto it = pairs_.find(binding.key);
        if (it == pairs_.end()) return false;
        auto& token = binding.role == ClientRole::primary ? it->second.primary : it->second.secondary;
        if (token != binding.token) return false; // A late disconnect must not erase a replacement.
        token = 0;
        (binding.role == ClientRole::primary ? it->second.primary_socket : it->second.secondary_socket) = 0;
        if (binding.role == ClientRole::primary) {
            it->second.explorer = {};
            ++primary_revision_;
            it->second.game.stats_results.clear();
            it->second.game.extra_actions.clear();
            it->second.game.skin_actions.clear();
            clear_team_commands_locked(binding.key.first);
        }
        if (binding.role == ClientRole::secondary) {
            reset_game_locked(it->second.game, false);
            // Queued stats requests died with this endpoint; answer their requesters now.
            expire_stats_routes_locked(&binding.key, std::chrono::steady_clock::now());
        }
        // A game connection keeps its role when the primary UI disconnects.
        if (!it->second.primary && !it->second.secondary) pairs_.erase(it);
        ++revision_;
        if (binding.role != ClientRole::primary) return false;
        for (const auto& [key, pair] : pairs_)
            if (key.first == binding.key.first && pair.primary) return false;
        return true;
    }
    bool update_game(const Binding& binding, uint32_t uid, uint32_t state,
        std::string player_name, uint32_t map_id = 0, uint32_t mode = 0,
        uint32_t team_flag = 0) {
        // state 0 means the game endpoint is still on its own login screen.
        // It remains connected, but has no in-game UID yet.  This lets the
        // primary distinguish "game connected" from "entered the hall" and
        // also clears a previously reported state after the player logs out.
        if (binding.role != ClientRole::secondary || state > 4 || map_id > 100000 || mode > chh::wire::game_mode_max ||
            team_flag > 1 || (state == 0 && (uid != 0 || map_id != 0 || mode != 0 || team_flag != 0))) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary);
        if (!pair) return false;
        auto& game = pair->game;
        const uint32_t normalized_uid = state == 0 ? 0 : uid;
        const uint32_t normalized_map_id = state == 0 ? 0 : map_id;
        const uint32_t normalized_mode = state == 0 ? 0 : mode;
        const bool normalized_team = state == 0 ? false : team_flag != 0;
        // Friends' lists only show state, team, player name and connection.
        const bool presence_changed = game.state != state || game.team != normalized_team ||
            game.player_name != player_name || !game.connected;
        if (presence_changed || game.uid != normalized_uid || game.map_id != normalized_map_id ||
            game.mode != normalized_mode) {
            // Only status fields change. Player data is a user-requested
            // snapshot and commands/stats are independent queues; all of them
            // survive presence/state updates.
            game.revision = ++game_revision_;
            game.uid = normalized_uid;
            game.state = state;
            game.map_id = normalized_map_id;
            game.mode = normalized_mode;
            game.team = normalized_team;
            game.connected = true;
            game.player_name = std::move(player_name);
            if (presence_changed) ++presence_game_revision_;
        }
        return true;
    }
    bool update_game_memory(const Binding& binding, uint64_t available_virtual_memory) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary);
        if (!pair) return false;
        auto& game = pair->game;
        if (!game.connected || game.available_virtual_memory != available_virtual_memory) {
            // Periodic memory reports reach only the paired primary; they must
            // not invalidate every friend list's presence snapshot.
            if (!game.connected) ++presence_game_revision_;
            game.available_virtual_memory = available_virtual_memory;
            game.connected = true;
            game.revision = ++game_revision_;
        }
        return true;
    }
    // The game endpoint owns the initial SightI value.  It may only update the
    // pair identified by its authenticated account and observed source IP;
    // primary clients and unpaired/stale bindings are rejected.
    bool update_game_sight(const Binding& binding, uint32_t sight) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.sight_initialized && game.sight == sight) return true;
        game.sight = sight;
        game.sight_initialized = true;
        game.sight_revision = ++sight_revision_;
        return true;
    }
    // The primary owns slider changes.  Keep the latest command in the pair
    // so a game endpoint that connects just after the drag can still receive
    // the requested value on its first polling pass.
    bool set_game_sight(const Binding& binding, uint32_t sight) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.sight_command == sight && game.sight_command_revision != 0) return true;
        game.sight_command = sight;
        game.sight_command_revision = ++sight_command_revision_;
        return true;
    }
    // The primary sends the whole card so main and team switches are applied
    // together. Retain its latest values for a game endpoint reconnect.
    bool set_game_skill_hint(const Binding& binding, uint32_t enabled, uint32_t ally, uint32_t enemy, uint32_t hero_skills = 0) {
        if (enabled > 1 || ally > 1 || enemy > 1 || hero_skills > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.skill_hint_command_revision != 0 && game.skill_hint_enabled == enabled &&
            game.skill_hint_ally == ally && game.skill_hint_enemy == enemy && game.skill_hint_hero_skills == hero_skills) return true;
        game.skill_hint_enabled = enabled;
        game.skill_hint_ally = ally;
        game.skill_hint_enemy = enemy;
        game.skill_hint_hero_skills = hero_skills;
        game.skill_hint_command_revision = ++skill_hint_command_revision_;
        return true;
    }
    // Retain the complete status-display card for the paired game's next connection.
    bool set_game_status_display(const Binding& binding, uint32_t enabled, uint32_t ally, uint32_t enemy) {
        if (enabled > 1 || ally > 1 || enemy > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.status_display_command_revision != 0 && game.status_display_enabled == enabled &&
            game.status_display_ally == ally && game.status_display_enemy == enemy) return true;
        game.status_display_enabled = enabled;
        game.status_display_ally = ally;
        game.status_display_enemy = enemy;
        game.status_display_command_revision = ++status_display_command_revision_;
        return true;
    }
    // The range keeps its IEEE 754 bits while the complete card is cached and relayed.
    bool set_game_auto_disarm(const Binding& binding, uint32_t enabled, uint32_t on_skill, uint32_t direct,
        uint32_t range_bits) {
        if (enabled > 1 || on_skill > 1 || direct > 1 ||
            !chh::wire::valid_game_auto_disarm_range(chh::wire::game_auto_disarm_range_from_bits(range_bits))) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_disarm_command_revision != 0 && game.auto_disarm_enabled == enabled &&
            game.auto_disarm_on_skill == on_skill && game.auto_disarm_direct == direct &&
            game.auto_disarm_range_bits == range_bits) return true;
        game.auto_disarm_enabled = enabled;
        game.auto_disarm_on_skill = on_skill;
        game.auto_disarm_direct = direct;
        game.auto_disarm_range_bits = range_bits;
        game.auto_disarm_command_revision = ++auto_disarm_command_revision_;
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_auto_disarm_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        const bool first = !game.auto_disarm_roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(game.auto_disarm_roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        game.auto_disarm_roster = std::move(roster);
        game.auto_disarm_roster_fresh = true;
        game.auto_disarm_roster_revision = ++auto_disarm_roster_revision_;
        if (new_match) game.auto_disarm_request_id = 0;
        if (first || new_match || exclusions_changed)
            game.auto_disarm_exclusions_revision = ++auto_disarm_exclusions_revision_;
        return true;
    }
    enum class AutoDisarmExclusionsResult { invalid, ignored, applied };
    AutoDisarmExclusionsResult set_game_auto_disarm_exclusions(const Binding& binding,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoDisarmExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoDisarmExclusionsResult::invalid;
        auto& game = pair->game;
        if (!pair->secondary || !game.connected || !game.auto_disarm_roster_fresh ||
            command.match_id != game.auto_disarm_roster.match_id || command.request_id < game.auto_disarm_request_id)
            return AutoDisarmExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(game.auto_disarm_roster, command.hero_ids);
        if (command.request_id == game.auto_disarm_request_id)
            return command.hero_ids == game.auto_disarm_roster.excluded_hero_ids ?
                AutoDisarmExclusionsResult::applied : AutoDisarmExclusionsResult::invalid;
        game.auto_disarm_roster.excluded_hero_ids = std::move(command.hero_ids);
        game.auto_disarm_request_id = command.request_id;
        game.auto_disarm_exclusions_revision = ++auto_disarm_exclusions_revision_;
        return AutoDisarmExclusionsResult::applied;
    }
    bool set_game_auto_guisuo(const Binding& binding, uint32_t enabled, uint32_t min_count) {
        if (!chh::wire::valid_game_auto_guisuo_config(enabled, min_count)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_guisuo_command_revision != 0 && game.auto_guisuo_enabled == enabled &&
            game.auto_guisuo_min_count == min_count) return true;
        game.auto_guisuo_enabled = enabled;
        game.auto_guisuo_min_count = min_count;
        game.auto_guisuo_command_revision = ++auto_guisuo_command_revision_;
        return true;
    }
    bool update_game_auto_guisuo_roster(const Binding& binding, uint32_t side, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_guisuo_side(side) || !chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& item = pair->game.auto_guisuo_targets[side];
        const bool first = !item.roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(item.roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        item.roster = std::move(roster);
        item.roster_fresh = true;
        item.roster_revision = ++auto_guisuo_roster_revisions_[side];
        if (new_match) item.request_id = 0;
        if (first || new_match || exclusions_changed)
            item.exclusions_revision = ++auto_guisuo_exclusions_revisions_[side];
        return true;
    }
    enum class AutoGuisuoExclusionsResult { invalid, ignored, applied };
    AutoGuisuoExclusionsResult set_game_auto_guisuo_exclusions(const Binding& binding, uint32_t side,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_guisuo_side(side) || !chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoGuisuoExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoGuisuoExclusionsResult::invalid;
        auto& item = pair->game.auto_guisuo_targets[side];
        if (!pair->secondary || !pair->game.connected || !item.roster_fresh ||
            command.match_id != item.roster.match_id || command.request_id < item.request_id)
            return AutoGuisuoExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(item.roster, command.hero_ids);
        if (command.request_id == item.request_id)
            return command.hero_ids == item.roster.excluded_hero_ids ?
                AutoGuisuoExclusionsResult::applied : AutoGuisuoExclusionsResult::invalid;
        item.roster.excluded_hero_ids = std::move(command.hero_ids);
        item.request_id = command.request_id;
        item.exclusions_revision = ++auto_guisuo_exclusions_revisions_[side];
        return AutoGuisuoExclusionsResult::applied;
    }
    bool set_game_auto_dihuo(const Binding& binding, uint32_t enabled, uint32_t range_bits) {
        if (!chh::wire::valid_game_auto_dihuo_config(enabled, range_bits)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_dihuo_command_revision != 0 && game.auto_dihuo_enabled == enabled &&
            game.auto_dihuo_range_bits == range_bits) return true;
        game.auto_dihuo_enabled = enabled;
        game.auto_dihuo_range_bits = range_bits;
        game.auto_dihuo_command_revision = ++auto_dihuo_command_revision_;
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_auto_dihuo_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        const bool first = !game.auto_dihuo_roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(game.auto_dihuo_roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        game.auto_dihuo_roster = std::move(roster);
        game.auto_dihuo_roster_fresh = true;
        game.auto_dihuo_roster_revision = ++auto_dihuo_roster_revision_;
        if (new_match) game.auto_dihuo_request_id = 0;
        if (first || new_match || exclusions_changed)
            game.auto_dihuo_exclusions_revision = ++auto_dihuo_exclusions_revision_;
        return true;
    }
    enum class AutoDihuoExclusionsResult { invalid, ignored, applied };
    AutoDihuoExclusionsResult set_game_auto_dihuo_exclusions(const Binding& binding,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoDihuoExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoDihuoExclusionsResult::invalid;
        auto& game = pair->game;
        if (!pair->secondary || !game.connected || !game.auto_dihuo_roster_fresh ||
            command.match_id != game.auto_dihuo_roster.match_id || command.request_id < game.auto_dihuo_request_id)
            return AutoDihuoExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(game.auto_dihuo_roster, command.hero_ids);
        if (command.request_id == game.auto_dihuo_request_id)
            return command.hero_ids == game.auto_dihuo_roster.excluded_hero_ids ?
                AutoDihuoExclusionsResult::applied : AutoDihuoExclusionsResult::invalid;
        game.auto_dihuo_roster.excluded_hero_ids = std::move(command.hero_ids);
        game.auto_dihuo_request_id = command.request_id;
        game.auto_dihuo_exclusions_revision = ++auto_dihuo_exclusions_revision_;
        return AutoDihuoExclusionsResult::applied;
    }
    bool set_game_auto_douji(const Binding& binding, uint32_t flags, uint32_t range_bits) {
        if (!chh::wire::valid_game_auto_douji_config(flags, range_bits)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_douji_command_revision != 0 && game.auto_douji_flags == flags &&
            game.auto_douji_range_bits == range_bits) return true;
        game.auto_douji_flags = flags;
        game.auto_douji_range_bits = range_bits;
        game.auto_douji_command_revision = ++auto_douji_command_revision_;
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_auto_douji_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        const bool first = !game.auto_douji_roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(game.auto_douji_roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        game.auto_douji_roster = std::move(roster);
        game.auto_douji_roster_fresh = true;
        game.auto_douji_roster_revision = ++auto_douji_roster_revision_;
        if (new_match) game.auto_douji_request_id = 0;
        if (first || new_match || exclusions_changed)
            game.auto_douji_exclusions_revision = ++auto_douji_exclusions_revision_;
        return true;
    }
    enum class AutoDoujiExclusionsResult { invalid, ignored, applied };
    AutoDoujiExclusionsResult set_game_auto_douji_exclusions(const Binding& binding,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoDoujiExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoDoujiExclusionsResult::invalid;
        auto& game = pair->game;
        if (!pair->secondary || !game.connected || !game.auto_douji_roster_fresh ||
            command.match_id != game.auto_douji_roster.match_id || command.request_id < game.auto_douji_request_id)
            return AutoDoujiExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(game.auto_douji_roster, command.hero_ids);
        if (command.request_id == game.auto_douji_request_id)
            return command.hero_ids == game.auto_douji_roster.excluded_hero_ids ?
                AutoDoujiExclusionsResult::applied : AutoDoujiExclusionsResult::invalid;
        game.auto_douji_roster.excluded_hero_ids = std::move(command.hero_ids);
        game.auto_douji_request_id = command.request_id;
        game.auto_douji_exclusions_revision = ++auto_douji_exclusions_revision_;
        return AutoDoujiExclusionsResult::applied;
    }
    bool set_game_auto_dark_shield(const Binding& binding, uint32_t enabled, uint32_t min_count,
        uint32_t use_dark_shield = 1, uint32_t use_dining_table = 0) {
        if (!chh::wire::valid_game_auto_dark_shield_config(enabled, min_count, use_dark_shield, use_dining_table)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_dark_shield_command_revision != 0 && game.auto_dark_shield_enabled == enabled &&
            game.auto_dark_shield_min_count == min_count && game.auto_dark_shield_use_dark_shield == use_dark_shield &&
            game.auto_dark_shield_use_dining_table == use_dining_table) return true;
        game.auto_dark_shield_enabled = enabled;
        game.auto_dark_shield_min_count = min_count;
        game.auto_dark_shield_use_dark_shield = use_dark_shield;
        game.auto_dark_shield_use_dining_table = use_dining_table;
        game.auto_dark_shield_command_revision = ++auto_dark_shield_command_revision_;
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_auto_dark_shield_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        const bool first = !game.auto_dark_shield_roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(game.auto_dark_shield_roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        game.auto_dark_shield_roster = std::move(roster);
        game.auto_dark_shield_roster_fresh = true;
        game.auto_dark_shield_roster_revision = ++auto_dark_shield_roster_revision_;
        if (new_match) game.auto_dark_shield_request_id = 0;
        if (first || new_match || exclusions_changed)
            game.auto_dark_shield_exclusions_revision = ++auto_dark_shield_exclusions_revision_;
        return true;
    }
    enum class AutoDarkShieldExclusionsResult { invalid, ignored, applied };
    AutoDarkShieldExclusionsResult set_game_auto_dark_shield_exclusions(const Binding& binding,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoDarkShieldExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoDarkShieldExclusionsResult::invalid;
        auto& game = pair->game;
        if (!pair->secondary || !game.connected || !game.auto_dark_shield_roster_fresh ||
            command.match_id != game.auto_dark_shield_roster.match_id || command.request_id < game.auto_dark_shield_request_id)
            return AutoDarkShieldExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(game.auto_dark_shield_roster, command.hero_ids);
        if (command.request_id == game.auto_dark_shield_request_id)
            return command.hero_ids == game.auto_dark_shield_roster.excluded_hero_ids ?
                AutoDarkShieldExclusionsResult::applied : AutoDarkShieldExclusionsResult::invalid;
        game.auto_dark_shield_roster.excluded_hero_ids = std::move(command.hero_ids);
        game.auto_dark_shield_request_id = command.request_id;
        game.auto_dark_shield_exclusions_revision = ++auto_dark_shield_exclusions_revision_;
        return AutoDarkShieldExclusionsResult::applied;
    }
    bool set_game_auto_heal(const Binding& binding, uint32_t enabled, uint32_t health_percent) {
        if (!chh::wire::valid_game_auto_heal_config(enabled, health_percent)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_heal_command_revision != 0 && game.auto_heal_enabled == enabled &&
            game.auto_heal_health_percent == health_percent) return true;
        game.auto_heal_enabled = enabled;
        game.auto_heal_health_percent = health_percent;
        game.auto_heal_command_revision = ++auto_heal_command_revision_;
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_auto_heal_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& game = pair->game;
        const bool first = !game.auto_heal_roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(game.auto_heal_roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        game.auto_heal_roster = std::move(roster);
        game.auto_heal_roster_fresh = true;
        game.auto_heal_roster_revision = ++auto_heal_roster_revision_;
        if (new_match) game.auto_heal_request_id = 0;
        if (first || new_match || exclusions_changed)
            game.auto_heal_exclusions_revision = ++auto_heal_exclusions_revision_;
        return true;
    }
    enum class AutoHealExclusionsResult { invalid, ignored, applied };
    AutoHealExclusionsResult set_game_auto_heal_exclusions(const Binding& binding,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return AutoHealExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return AutoHealExclusionsResult::invalid;
        auto& game = pair->game;
        if (!pair->secondary || !game.connected || !game.auto_heal_roster_fresh ||
            command.match_id != game.auto_heal_roster.match_id || command.request_id < game.auto_heal_request_id)
            return AutoHealExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(game.auto_heal_roster, command.hero_ids);
        if (command.request_id == game.auto_heal_request_id)
            return command.hero_ids == game.auto_heal_roster.excluded_hero_ids ?
                AutoHealExclusionsResult::applied : AutoHealExclusionsResult::invalid;
        game.auto_heal_roster.excluded_hero_ids = std::move(command.hero_ids);
        game.auto_heal_request_id = command.request_id;
        game.auto_heal_exclusions_revision = ++auto_heal_exclusions_revision_;
        return AutoHealExclusionsResult::applied;
    }
    bool set_game_near_item(const Binding& binding, uint32_t item_slot, uint32_t enabled, uint32_t direct, uint32_t on_attack,
        uint32_t on_attacked, uint32_t range_bits, uint32_t health_percent) {
        if (!chh::wire::valid_game_near_item_slot(item_slot) || !chh::wire::valid_game_near_item1_config(enabled, direct, on_attack, on_attacked, range_bits, health_percent))
            return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& item = pair->game.near_items[item_slot - 1];
        if (item.command_revision != 0 && item.enabled == enabled &&
            item.direct == direct && item.on_attack == on_attack &&
            item.on_attacked == on_attacked && item.range_bits == range_bits &&
            item.health_percent == health_percent) return true;
        item.enabled = enabled;
        item.direct = direct;
        item.on_attack = on_attack;
        item.on_attacked = on_attacked;
        item.range_bits = range_bits;
        item.health_percent = health_percent;
        item.command_revision = ++near_item_command_revisions_[item_slot - 1];
        return true;
    }
    // A new game connection must identify its match before any retained exclusions
    // become visible. Only the first roster of a new match supplies initial exclusions.
    bool update_game_near_item_roster(const Binding& binding, uint32_t item_slot, chh::wire::GameAutoDisarmRoster roster) {
        if (!chh::wire::valid_game_near_item_slot(item_slot) || !chh::wire::valid_game_auto_disarm_roster(roster)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        auto& item = pair->game.near_items[item_slot - 1];
        const bool first = !item.roster_fresh;
        const auto [new_match, players_changed, exclusions_changed] =
            prepare_roster_update(item.roster, roster);
        if (!first && !new_match && !players_changed && !exclusions_changed) return true;
        item.roster = std::move(roster);
        item.roster_fresh = true;
        item.roster_revision = ++near_item_roster_revisions_[item_slot - 1];
        if (new_match) item.request_id = 0;
        if (first || new_match || exclusions_changed)
            item.exclusions_revision = ++near_item_exclusions_revisions_[item_slot - 1];
        return true;
    }
    enum class NearItemExclusionsResult { invalid, ignored, applied };
    NearItemExclusionsResult set_game_near_item_exclusions(const Binding& binding, uint32_t item_slot,
        chh::wire::GameAutoDisarmExclusions command) {
        if (!chh::wire::valid_game_near_item_slot(item_slot) || !chh::wire::valid_game_auto_disarm_exclusions(command) || !command.request_id)
            return NearItemExclusionsResult::invalid;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return NearItemExclusionsResult::invalid;
        auto& item = pair->game.near_items[item_slot - 1];
        if (!pair->secondary || !pair->game.connected || !item.roster_fresh ||
            command.match_id != item.roster.match_id || command.request_id < item.request_id)
            return NearItemExclusionsResult::ignored;
        // A player can leave between the UI click and receipt. Acknowledge the
        // new request with its still-present heroes so the UI can finish waiting.
        prune_roster_heroes(item.roster, command.hero_ids);
        if (command.request_id == item.request_id)
            return command.hero_ids == item.roster.excluded_hero_ids ?
                NearItemExclusionsResult::applied : NearItemExclusionsResult::invalid;
        item.roster.excluded_hero_ids = std::move(command.hero_ids);
        item.request_id = command.request_id;
        item.exclusions_revision = ++near_item_exclusions_revisions_[item_slot - 1];
        return NearItemExclusionsResult::applied;
    }
    // Keep the original slot-one API and wire messages compatible.
    bool set_game_near_item1(const Binding& binding, uint32_t enabled, uint32_t direct, uint32_t on_attack,
        uint32_t on_attacked, uint32_t range_bits, uint32_t health_percent) {
        return set_game_near_item(binding, 1, enabled, direct, on_attack, on_attacked, range_bits, health_percent);
    }
    bool update_game_near_item1_roster(const Binding& binding, chh::wire::GameAutoDisarmRoster roster) {
        return update_game_near_item_roster(binding, 1, std::move(roster));
    }
    using NearItem1ExclusionsResult = NearItemExclusionsResult;
    NearItem1ExclusionsResult set_game_near_item1_exclusions(const Binding& binding, chh::wire::GameAutoDisarmExclusions command) {
        return set_game_near_item_exclusions(binding, 1, std::move(command));
    }
    // Retain all auto-cleanse switches together for the paired game's next connection.
    bool set_game_auto_cleanse(const Binding& binding, uint32_t enabled, uint32_t ignite, uint32_t disarm,
        uint32_t root, uint32_t control, uint32_t silence, uint32_t stun, uint32_t taunt,
        uint32_t delay_ms, uint32_t min_count, uint32_t control_time_ms) {
        if (enabled > 1 || ignite > 1 || disarm > 1 || root > 1 || control > 1 || silence > 1 || stun > 1 ||
            taunt > 1 || delay_ms > 300 || min_count > 6 || control_time_ms > 1000) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_cleanse_command_revision != 0 && game.auto_cleanse_enabled == enabled &&
            game.auto_cleanse_ignite == ignite && game.auto_cleanse_disarm == disarm && game.auto_cleanse_root == root &&
            game.auto_cleanse_control == control && game.auto_cleanse_silence == silence && game.auto_cleanse_stun == stun &&
            game.auto_cleanse_taunt == taunt && game.auto_cleanse_delay_ms == delay_ms && game.auto_cleanse_min_count == min_count &&
            game.auto_cleanse_control_time_ms == control_time_ms)
            return true;
        game.auto_cleanse_enabled = enabled;
        game.auto_cleanse_ignite = ignite;
        game.auto_cleanse_disarm = disarm;
        game.auto_cleanse_root = root;
        game.auto_cleanse_control = control;
        game.auto_cleanse_silence = silence;
        game.auto_cleanse_stun = stun;
        game.auto_cleanse_taunt = taunt;
        game.auto_cleanse_delay_ms = delay_ms;
        game.auto_cleanse_min_count = min_count;
        game.auto_cleanse_control_time_ms = control_time_ms;
        game.auto_cleanse_command_revision = ++auto_cleanse_command_revision_;
        return true;
    }
    // Retain the complete training-mode card for the paired game's next connection.
    bool set_game_training_mode(const Binding& binding, uint32_t enabled, uint32_t unlimited_cooldown, uint32_t attack_dummy) {
        if (enabled > 1 || unlimited_cooldown > 1 || attack_dummy > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.training_mode_command_revision != 0 && game.training_mode_enabled == enabled &&
            game.training_mode_unlimited_cooldown == unlimited_cooldown && game.training_mode_attack_dummy == attack_dummy) return true;
        game.training_mode_enabled = enabled;
        game.training_mode_unlimited_cooldown = unlimited_cooldown;
        game.training_mode_attack_dummy = attack_dummy;
        game.training_mode_command_revision = ++training_mode_command_revision_;
        return true;
    }
    // The complete auto-smite card is retained for the paired game's next connection.
    bool set_game_auto_smite(const Binding& binding, uint32_t enabled, uint32_t dragon, uint32_t buffs,
        uint32_t monster_catcher) {
        if (enabled > 1 || dragon > 1 || buffs > 1 || monster_catcher > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_smite_command_revision != 0 && game.auto_smite_enabled == enabled &&
            game.auto_smite_dragon == dragon && game.auto_smite_buffs == buffs &&
            game.auto_smite_monster_catcher == monster_catcher) return true;
        game.auto_smite_enabled = enabled;
        game.auto_smite_dragon = dragon;
        game.auto_smite_buffs = buffs;
        game.auto_smite_monster_catcher = monster_catcher;
        game.auto_smite_command_revision = ++auto_smite_command_revision_;
        return true;
    }
    bool set_game_auto_glove(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_glove_command_revision != 0 && game.auto_glove_enabled == enabled) return true;
        game.auto_glove_enabled = enabled;
        game.auto_glove_command_revision = ++auto_glove_command_revision_;
        return true;
    }
    bool set_game_auto_lingzhou(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_lingzhou_command_revision != 0 && game.auto_lingzhou_enabled == enabled) return true;
        game.auto_lingzhou_enabled = enabled;
        game.auto_lingzhou_command_revision = ++auto_lingzhou_command_revision_;
        return true;
    }
    bool set_game_auto_fairy(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_fairy_command_revision != 0 && game.auto_fairy_enabled == enabled) return true;
        game.auto_fairy_enabled = enabled;
        game.auto_fairy_command_revision = ++auto_fairy_command_revision_;
        return true;
    }
    bool set_game_auto_devour(const Binding& binding, uint32_t enabled, uint32_t minions) {
        if (enabled > 1 || minions > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_devour_command_revision != 0 && game.auto_devour_enabled == enabled &&
            game.auto_devour_minions == minions) return true;
        game.auto_devour_enabled = enabled;
        game.auto_devour_minions = minions;
        game.auto_devour_command_revision = ++auto_devour_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_jungle_timer(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.jungle_timer_command_revision != 0 && game.jungle_timer_enabled == enabled) return true;
        game.jungle_timer_enabled = enabled;
        game.jungle_timer_command_revision = ++jungle_timer_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_eye_timer(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.eye_timer_command_revision != 0 && game.eye_timer_enabled == enabled) return true;
        game.eye_timer_enabled = enabled;
        game.eye_timer_command_revision = ++eye_timer_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_trap_timer(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.trap_timer_command_revision != 0 && game.trap_timer_enabled == enabled) return true;
        game.trap_timer_enabled = enabled;
        game.trap_timer_command_revision = ++trap_timer_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggles and duration across a game endpoint reconnect.
    bool set_game_disappearance_hint(const Binding& binding, uint32_t enabled, uint32_t duration_seconds, uint32_t show_heroes = 0) {
        if (enabled > 1 || show_heroes > 1 || (duration_seconds != 30 && duration_seconds != 45 && duration_seconds != 60)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.disappearance_hint_command_revision != 0 && game.disappearance_hint_enabled == enabled &&
            game.disappearance_hint_duration_seconds == duration_seconds && game.disappearance_hint_show_heroes == show_heroes) return true;
        game.disappearance_hint_enabled = enabled;
        game.disappearance_hint_duration_seconds = duration_seconds;
        game.disappearance_hint_show_heroes = show_heroes;
        game.disappearance_hint_command_revision = ++disappearance_hint_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_movement_trace(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.movement_trace_command_revision != 0 && game.movement_trace_enabled == enabled) return true;
        game.movement_trace_enabled = enabled;
        game.movement_trace_command_revision = ++movement_trace_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_tower_range(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.tower_range_command_revision != 0 && game.tower_range_enabled == enabled) return true;
        game.tower_range_enabled = enabled;
        game.tower_range_command_revision = ++tower_range_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_attack_range(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.attack_range_command_revision != 0 && game.attack_range_enabled == enabled) return true;
        game.attack_range_enabled = enabled;
        game.attack_range_command_revision = ++attack_range_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_auto_afk(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_afk_command_revision != 0 && game.auto_afk_enabled == enabled) return true;
        game.auto_afk_enabled = enabled;
        game.auto_afk_command_revision = ++auto_afk_command_revision_;
        return true;
    }
    // Preserve the primary's latest walk-attack minimum move distance (float32 bits) across a game endpoint reconnect.
    bool set_game_move_distance(const Binding& binding, uint32_t distance_bits) {
        if (!chh::wire::valid_game_move_distance_bits(distance_bits)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.move_distance_command_revision != 0 && game.move_distance_bits == distance_bits) return true;
        game.move_distance_bits = distance_bits;
        game.move_distance_command_revision = ++move_distance_command_revision_;
        return true;
    }
    // Preserve the primary's latest hero-function keys (combo key, toggle key, lane key; Windows
    // virtual-key codes) across a game endpoint reconnect.
    bool set_game_hero_keys(const Binding& binding, uint32_t combo_key, uint32_t toggle_key, uint32_t lane_key) {
        if (!chh::wire::valid_game_hero_keys(combo_key, toggle_key, lane_key)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.hero_keys_command_revision != 0 && game.hero_combo_key == combo_key &&
            game.hero_toggle_key == toggle_key && game.hero_lane_key == lane_key) return true;
        game.hero_combo_key = combo_key;
        game.hero_toggle_key = toggle_key;
        game.hero_lane_key = lane_key;
        game.hero_keys_command_revision = ++hero_keys_command_revision_;
        return true;
    }
    // Preserve the primary's latest hero-function run delay across a game endpoint reconnect.
    bool set_game_hero_delay(const Binding& binding, uint32_t delay_ms) {
        if (!chh::wire::valid_game_hero_delay(delay_ms)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.hero_delay_command_revision != 0 && game.hero_delay_ms == delay_ms) return true;
        game.hero_delay_ms = delay_ms;
        game.hero_delay_command_revision = ++hero_delay_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_skin_block(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.skin_block_command_revision != 0 && game.skin_block_enabled == enabled) return true;
        game.skin_block_enabled = enabled;
        game.skin_block_command_revision = ++skin_block_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle and team filters across a game endpoint reconnect.
    bool set_game_projectile_display(const Binding& binding, uint32_t enabled, uint32_t enemy = 1, uint32_t ally = 0) {
        if (enabled > 1 || enemy > 1 || ally > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.projectile_display_command_revision != 0 && game.projectile_display_enabled == enabled &&
            game.projectile_display_enemy == enemy && game.projectile_display_ally == ally) return true;
        game.projectile_display_enabled = enabled;
        game.projectile_display_enemy = enemy;
        game.projectile_display_ally = ally;
        game.projectile_display_command_revision = ++projectile_display_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle and shape filters across a game endpoint reconnect.
    bool set_game_auto_dodge(const Binding& binding, uint32_t enabled, uint32_t rectangles, uint32_t circles) {
        if (enabled > 1 || rectangles > 1 || circles > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_dodge_command_revision != 0 && game.auto_dodge_enabled == enabled &&
            game.auto_dodge_rectangles == rectangles && game.auto_dodge_circles == circles) return true;
        game.auto_dodge_enabled = enabled;
        game.auto_dodge_rectangles = rectangles;
        game.auto_dodge_circles = circles;
        game.auto_dodge_command_revision = ++auto_dodge_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle and threshold across a game endpoint reconnect.
    bool set_game_low_health_hint(const Binding& binding, uint32_t enabled, uint32_t threshold_percent = 30) {
        if (enabled > 1 || (threshold_percent != 30 && threshold_percent != 40 && threshold_percent != 50)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.low_health_hint_command_revision != 0 && game.low_health_hint_enabled == enabled &&
            game.low_health_hint_threshold_percent == threshold_percent) return true;
        game.low_health_hint_enabled = enabled;
        game.low_health_hint_threshold_percent = threshold_percent;
        game.low_health_hint_command_revision = ++low_health_hint_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_selection_shout(const Binding& binding, uint32_t enabled, std::string text) {
        if (!chh::wire::valid_game_selection_shout_config(enabled, text)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.selection_shout_command_revision != 0 && game.selection_shout_enabled == enabled &&
            game.selection_shout_text == text) return true;
        game.selection_shout_enabled = enabled;
        game.selection_shout_text = std::move(text);
        game.selection_shout_command_revision = ++selection_shout_command_revision_;
        return true;
    }
    bool set_game_auto_accept(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_accept_command_revision != 0 && game.auto_accept_enabled == enabled) return true;
        game.auto_accept_enabled = enabled;
        game.auto_accept_command_revision = ++auto_accept_command_revision_;
        return true;
    }
    bool set_game_instant_pick(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.instant_pick_command_revision != 0 && game.instant_pick_enabled == enabled) return true;
        game.instant_pick_enabled = enabled;
        game.instant_pick_command_revision = ++instant_pick_command_revision_;
        return true;
    }
    // Preserve the primary's latest toggle across a game endpoint reconnect.
    bool set_game_auto_ignite(const Binding& binding, uint32_t enabled) {
        if (enabled > 1) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.auto_ignite_command_revision != 0 && game.auto_ignite_enabled == enabled) return true;
        game.auto_ignite_enabled = enabled;
        game.auto_ignite_command_revision = ++auto_ignite_command_revision_;
        return true;
    }
    // Retain the primary's latest selection across a game endpoint reconnect.
    bool set_game_hero_select(const Binding& binding, uint32_t hero_id) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.hero_select_command_revision != 0 && game.hero_select_id == hero_id) return true;
        game.hero_select_id = hero_id;
        game.hero_select_command_revision = ++hero_select_command_revision_;
        return true;
    }
    bool clear_unavailable_hero_select(const Binding& binding, const std::vector<uint32_t>& allowed) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        auto& game = pair->game;
        if (game.hero_select_id != 0 && !std::binary_search(allowed.begin(), allowed.end(), game.hero_select_id)) {
            game.hero_select_id = 0;
            game.hero_select_command_revision = ++hero_select_command_revision_;
        }
        return true;
    }
    // Request that the currently connected game endpoint terminate its game
    // process.  The request is accepted only from the matching primary and
    // only while a secondary is connected for the same account and IP.
    bool request_game_close(const Binding& binding) {
        return queue_game_command(binding, &Game::close_command_revision, game_close_revision_);
    }
    bool request_game_extra_action(const Binding& binding, uint32_t action, uint64_t target_game_connection_id) {
        if (!chh::wire::valid_game_extra_action(action) || !target_game_connection_id) return false;
        std::lock_guard lock(mutex_);
        const auto pair = paired_game_locked(binding);
        if (!pair || pair->secondary != target_game_connection_id || pair->game.extra_actions.size() >= 32) return false;
        pair->game.extra_actions.push_back(action);
        return true;
    }
    std::vector<uint32_t> take_game_extra_actions(const Binding& binding) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        return pair ? drain(pair->game.extra_actions) : std::vector<uint32_t>{};
    }
    bool request_game_skin_action(const Binding& binding, uint32_t action, uint32_t skin_id, uint64_t target_game_connection_id) {
        if (!chh::wire::valid_game_skin_action(action, skin_id) || !target_game_connection_id) return false;
        std::lock_guard lock(mutex_);
        const auto pair = paired_game_locked(binding);
        if (!pair || pair->secondary != target_game_connection_id || pair->game.skin_actions.size() >= 32) return false;
        pair->game.skin_actions.push_back({action, skin_id});
        return true;
    }
    std::vector<Game::SkinAction> take_game_skin_actions(const Binding& binding) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        return pair ? drain(pair->game.skin_actions) : std::vector<Game::SkinAction>{};
    }
    // The caller authorizes the friendship. Select both current endpoints,
    // validate the team room and enqueue under one lock so a primary moving
    // to another IP cannot change the destination between check and use.
    bool request_game_team(const Binding& binding, const std::string& peer, bool invite) {
        if (peer.empty() || peer == binding.key.first) return false;
        std::lock_guard lock(mutex_);
        const auto own = paired_game_locked(binding);
        const auto other = account_game_locked(peer);
        if (!own || !other || !other->secondary || !other->game.connected) return false;
        const auto& host = invite ? own->game : other->game;
        if (host.state != 1 || !host.team ||
            host.player_name.size() > chh::wire::game_team_name_limit ||
            !chh::wire::valid_profile_name(host.player_name)) return false;
        auto& destination = invite ? other->game : own->game;
        destination.team_command_name = host.player_name;
        destination.team_command_revision = ++team_command_revision_;
        return true;
    }
    bool request_game_players(const Binding& binding) {
        return queue_game_command(binding, &Game::players_request_revision, players_request_revision_);
    }
    bool request_game_output(const Binding& binding) {
        return queue_game_command(binding, &Game::output_request_revision, output_request_revision_);
    }
    // Take pending cross-game stats requests for this authenticated game
    // endpoint. Requests are consumed once handed to the worker; a lost game
    // connection therefore cannot replay an old query after reconnecting.
    std::vector<Game::StatsRequest> take_game_stats_requests(const Binding& binding) {
        std::lock_guard lock(mutex_);
        expire_stats_routes_locked(nullptr, std::chrono::steady_clock::now());
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        return pair ? drain(pair->game.stats_requests) : std::vector<Game::StatsRequest>{};
    }
    // Completed stats batches are consumed by the original primary worker.
    std::vector<Game::StatsBatch> take_game_stats_results(const Binding& binding) {
        std::lock_guard lock(mutex_);
        // Every primary worker polls this even when no new roster arrives.
        expire_stats_routes_locked(nullptr, std::chrono::steady_clock::now());
        const auto pair = bound_locked(binding, ClientRole::primary);
        return pair ? drain(pair->game.stats_results) : std::vector<Game::StatsBatch>{};
    }
    // Accept a response only from the secondary selected for this request and
    // route it to the primary token that initiated the refresh. Unknown/stale
    // request IDs are ignored so a reconnect cannot tear down a live game
    // connection with an old response.
    bool complete_game_stats(const Binding& binding, uint32_t request_id,
        std::vector<Game::StatsEntry> entries) {
        if (binding.role != ClientRole::secondary || !request_id ||
            entries.size() > chh::wire::game_stats_player_limit) return false;
        std::lock_guard lock(mutex_);
        expire_stats_routes_locked(nullptr, std::chrono::steady_clock::now());
        const auto route = stats_routes_.find(request_id);
        if (route == stats_routes_.end()) return true;
        if (route->second.target != binding.key) return false;
        const auto target = pairs_.find(binding.key);
        if (target == pairs_.end() || target->second.secondary != binding.token) return false;
        for (const auto& entry : entries) {
            if (entry.request_id != request_id || !entry.uid ||
                !route->second.uids.count(entry.uid) ||
                !chh::wire::valid_game_stats_result(entry.result)) return false;
        }
        std::set<uint32_t> seen;
        for (const auto& entry : entries)
            if (!seen.insert(entry.uid).second) return false;
        if (const auto requester = route_requester_locked(route->second)) {
            Game::StatsBatch batch;
            batch.request_id = request_id;
            batch.entries = std::move(entries);
            for (const auto uid : route->second.uids)
                if (!seen.count(uid)) batch.entries.push_back(unavailable_stats(request_id, uid));
            push_stats_result(requester->game, std::move(batch));
        }
        stats_routes_.erase(route);
        return true;
    }
    struct GameUidOwner {
        std::string user_name;
        std::string account_id;
        bool hide_account_id = false;
    };
    // Return the LA identity for an active game UID.  The primary may be
    // viewing a match containing several LA users, so checking only its own
    // UID would incorrectly leave the other LA users white.
    GameUidOwner game_uid_owner(uint32_t uid) const {
        if (!uid) return {};
        std::lock_guard lock(mutex_);
        for (const auto& [key, pair] : pairs_)
            if (pair.secondary && pair.game.connected && pair.game.uid == uid)
                return {pair.profile.name, key.first, pair.profile.hide_account_id};
        return {};
    }
    // A one-shot command queued by the primary, polled by the game endpoint.
    struct CommandSnapshot {
        uint64_t revision = 0;
        bool initialized = false;
    };
    CommandSnapshot game_players_request_snapshot(const Binding& binding) const {
        return command_snapshot(binding, &Game::players_request_revision);
    }
    CommandSnapshot game_output_request_snapshot(const Binding& binding) const {
        return command_snapshot(binding, &Game::output_request_revision);
    }
    bool update_game_players(const Binding& binding, std::vector<Game::Player> players) {
        if (!valid_players(players)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        pair->game.players = std::move(players);
        pair->game.players_revision = ++players_revision_;
        queue_game_stats_locked(binding.key, *pair);
        return true;
    }
    bool update_game_output(const Binding& binding, std::vector<Game::Player> players) {
        if (!valid_players(players)) return false;
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::secondary, true);
        if (!pair) return false;
        pair->game.output_players = std::move(players);
        pair->game.output_revision = ++output_revision_;
        return true;
    }
    struct PlayersSnapshot {
        uint64_t revision = 0;
        std::vector<Game::Player> players;
        bool initialized = false;
    };
    // The list is copied only when its revision differs from known_revision
    // (the caller's last sent revision), so unchanged polls stay allocation-free.
    PlayersSnapshot game_players_snapshot(const Binding& binding, uint64_t known_revision = UINT64_MAX) const {
        return players_snapshot(binding, &Game::players_revision, &Game::players, known_revision);
    }
    PlayersSnapshot game_output_snapshot(const Binding& binding, uint64_t known_revision = UINT64_MAX) const {
        return players_snapshot(binding, &Game::output_revision, &Game::output_players, known_revision);
    }
    struct SightSnapshot {
        uint64_t revision = 0;
        uint32_t value = 0;
        bool initialized = false;
    };
    SightSnapshot sight_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        SightSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::primary)) {
            result.revision = pair->game.sight_revision;
            result.value = pair->game.sight;
            result.initialized = pair->game.sight_initialized;
        }
        return result;
    }
    SightSnapshot sight_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        SightSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.sight_command_revision;
            result.value = pair->game.sight_command;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct SkillHintSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, ally = 0, enemy = 0, hero_skills = 0;
        bool initialized = false;
    };
    SkillHintSnapshot skill_hint_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        SkillHintSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.skill_hint_command_revision;
            result.enabled = game.skill_hint_enabled;
            result.ally = game.skill_hint_ally;
            result.enemy = game.skill_hint_enemy;
            result.hero_skills = game.skill_hint_hero_skills;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDisarmSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, on_skill = 1, direct = 1;
        uint32_t range_bits = chh::wire::game_auto_disarm_range_to_bits(chh::wire::game_auto_disarm_range_default);
        bool initialized = false;
    };
    AutoDisarmSnapshot auto_disarm_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDisarmSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.auto_disarm_command_revision;
            result.enabled = game.auto_disarm_enabled;
            result.on_skill = game.auto_disarm_on_skill;
            result.direct = game.auto_disarm_direct;
            result.range_bits = game.auto_disarm_range_bits;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDisarmRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoDisarmRosterSnapshot auto_disarm_roster_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDisarmRosterSnapshot result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_disarm_roster_fresh) return result;
        const auto& game = pair->game;
        result.game_connection_id = pair->secondary;
        result.roster_revision = game.auto_disarm_roster_revision;
        result.exclusions_revision = game.auto_disarm_exclusions_revision;
        result.roster = game.auto_disarm_roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = game.auto_disarm_request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct AutoGuisuoSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, min_count = chh::wire::game_auto_guisuo_min_count_default;
        bool initialized = false;
    };
    AutoGuisuoSnapshot auto_guisuo_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoGuisuoSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_guisuo_command_revision;
            result.enabled = pair->game.auto_guisuo_enabled;
            result.min_count = pair->game.auto_guisuo_min_count;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoGuisuoRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoGuisuoRosterSnapshot auto_guisuo_roster_snapshot(const Binding& binding, uint32_t side) const {
        std::lock_guard lock(mutex_);
        AutoGuisuoRosterSnapshot result;
        if (!chh::wire::valid_game_auto_guisuo_side(side)) return result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_guisuo_targets[side].roster_fresh) return result;
        const auto& item = pair->game.auto_guisuo_targets[side];
        result.game_connection_id = pair->secondary;
        result.roster_revision = item.roster_revision;
        result.exclusions_revision = item.exclusions_revision;
        result.roster = item.roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = item.request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct AutoDihuoSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, range_bits = chh::wire::game_auto_dihuo_range_to_bits(chh::wire::game_auto_dihuo_range_default);
        bool initialized = false;
    };
    AutoDihuoSnapshot auto_dihuo_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDihuoSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_dihuo_command_revision;
            result.enabled = pair->game.auto_dihuo_enabled;
            result.range_bits = pair->game.auto_dihuo_range_bits;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDihuoRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoDihuoRosterSnapshot auto_dihuo_roster_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDihuoRosterSnapshot result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_dihuo_roster_fresh) return result;
        const auto& game = pair->game;
        result.game_connection_id = pair->secondary;
        result.roster_revision = game.auto_dihuo_roster_revision;
        result.exclusions_revision = game.auto_dihuo_exclusions_revision;
        result.roster = game.auto_dihuo_roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = game.auto_dihuo_request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct AutoDoujiSnapshot {
        uint64_t revision = 0;
        uint32_t flags = chh::wire::game_auto_douji_flags_default, range_bits = chh::wire::game_auto_douji_range_to_bits(chh::wire::game_auto_douji_range_default);
        bool initialized = false;
    };
    AutoDoujiSnapshot auto_douji_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDoujiSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_douji_command_revision;
            result.flags = pair->game.auto_douji_flags;
            result.range_bits = pair->game.auto_douji_range_bits;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDoujiRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoDoujiRosterSnapshot auto_douji_roster_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDoujiRosterSnapshot result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_douji_roster_fresh) return result;
        const auto& game = pair->game;
        result.game_connection_id = pair->secondary;
        result.roster_revision = game.auto_douji_roster_revision;
        result.exclusions_revision = game.auto_douji_exclusions_revision;
        result.roster = game.auto_douji_roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = game.auto_douji_request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct AutoDarkShieldSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, min_count = chh::wire::game_auto_dark_shield_min_count_default;
        uint32_t use_dark_shield = 1, use_dining_table = 0;
        bool initialized = false;
    };
    AutoDarkShieldSnapshot auto_dark_shield_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDarkShieldSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_dark_shield_command_revision;
            result.enabled = pair->game.auto_dark_shield_enabled;
            result.min_count = pair->game.auto_dark_shield_min_count;
            result.use_dark_shield = pair->game.auto_dark_shield_use_dark_shield;
            result.use_dining_table = pair->game.auto_dark_shield_use_dining_table;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDarkShieldRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoDarkShieldRosterSnapshot auto_dark_shield_roster_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDarkShieldRosterSnapshot result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_dark_shield_roster_fresh) return result;
        const auto& game = pair->game;
        result.game_connection_id = pair->secondary;
        result.roster_revision = game.auto_dark_shield_roster_revision;
        result.exclusions_revision = game.auto_dark_shield_exclusions_revision;
        result.roster = game.auto_dark_shield_roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = game.auto_dark_shield_request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct AutoHealSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, health_percent = chh::wire::game_auto_heal_health_default;
        bool initialized = false;
    };
    AutoHealSnapshot auto_heal_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoHealSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_heal_command_revision;
            result.enabled = pair->game.auto_heal_enabled;
            result.health_percent = pair->game.auto_heal_health_percent;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoHealRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    AutoHealRosterSnapshot auto_heal_roster_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoHealRosterSnapshot result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.auto_heal_roster_fresh) return result;
        const auto& game = pair->game;
        result.game_connection_id = pair->secondary;
        result.roster_revision = game.auto_heal_roster_revision;
        result.exclusions_revision = game.auto_heal_exclusions_revision;
        result.roster = game.auto_heal_roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = game.auto_heal_request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    struct NearItemSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, direct = 1, on_attack = 0, on_attacked = 0;
        uint32_t range_bits = chh::wire::game_near_item1_range_to_bits(chh::wire::game_near_item1_range_default);
        uint32_t health_percent = chh::wire::game_near_item1_health_default;
        bool initialized = false;
    };
    NearItemSnapshot near_item_command_snapshot(const Binding& binding, uint32_t item_slot) const {
        std::lock_guard lock(mutex_);
        NearItemSnapshot result;
        if (!chh::wire::valid_game_near_item_slot(item_slot)) return result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& item = pair->game.near_items[item_slot - 1];
            result.revision = item.command_revision;
            result.enabled = item.enabled;
            result.on_attack = item.on_attack;
            result.on_attacked = item.on_attacked;
            result.health_percent = item.health_percent;
            result.direct = item.direct;
            result.range_bits = item.range_bits;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct NearItemRosterSnapshot {
        uint64_t roster_revision = 0, exclusions_revision = 0;
        uint64_t game_connection_id = 0;
        chh::wire::GameAutoDisarmRoster roster;
        chh::wire::GameAutoDisarmExclusions exclusions;
        bool ready = false;
    };
    NearItemRosterSnapshot near_item_roster_snapshot(const Binding& binding, uint32_t item_slot) const {
        std::lock_guard lock(mutex_);
        NearItemRosterSnapshot result;
        if (!chh::wire::valid_game_near_item_slot(item_slot)) return result;
        if (binding.role != ClientRole::primary && binding.role != ClientRole::secondary) return result;
        const auto pair = bound_locked(binding, binding.role, true);
        if (!pair || !pair->secondary || !pair->game.near_items[item_slot - 1].roster_fresh) return result;
        const auto& item = pair->game.near_items[item_slot - 1];
        result.game_connection_id = pair->secondary;
        result.roster_revision = item.roster_revision;
        result.exclusions_revision = item.exclusions_revision;
        result.roster = item.roster;
        result.exclusions.match_id = result.roster.match_id;
        result.exclusions.request_id = item.request_id;
        result.exclusions.hero_ids = result.roster.excluded_hero_ids;
        result.ready = true;
        return result;
    }
    using NearItem1Snapshot = NearItemSnapshot;
    using NearItem1RosterSnapshot = NearItemRosterSnapshot;
    NearItem1Snapshot near_item1_command_snapshot(const Binding& binding) const { return near_item_command_snapshot(binding, 1); }
    NearItem1RosterSnapshot near_item1_roster_snapshot(const Binding& binding) const { return near_item_roster_snapshot(binding, 1); }
    struct AutoCleanseSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, ignite = 1, disarm = 1, root = 1, control = 1, silence = 1, stun = 1;
        uint32_t taunt = 1;
        uint32_t delay_ms = 300, min_count = 0;
        uint32_t control_time_ms = 0;
        bool initialized = false;
    };
    AutoCleanseSnapshot auto_cleanse_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoCleanseSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.auto_cleanse_command_revision;
            result.enabled = game.auto_cleanse_enabled;
            result.ignite = game.auto_cleanse_ignite;
            result.disarm = game.auto_cleanse_disarm;
            result.root = game.auto_cleanse_root;
            result.control = game.auto_cleanse_control;
            result.silence = game.auto_cleanse_silence;
            result.stun = game.auto_cleanse_stun;
            result.taunt = game.auto_cleanse_taunt;
            result.delay_ms = game.auto_cleanse_delay_ms;
            result.min_count = game.auto_cleanse_min_count;
            result.control_time_ms = game.auto_cleanse_control_time_ms;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct StatusDisplaySnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, ally = 1, enemy = 1;
        bool initialized = false;
    };
    StatusDisplaySnapshot status_display_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        StatusDisplaySnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.status_display_command_revision;
            result.enabled = game.status_display_enabled;
            result.ally = game.status_display_ally;
            result.enemy = game.status_display_enemy;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct TrainingModeSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, unlimited_cooldown = 0, attack_dummy = 0;
        bool initialized = false;
    };
    TrainingModeSnapshot training_mode_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        TrainingModeSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.training_mode_command_revision;
            result.enabled = game.training_mode_enabled;
            result.unlimited_cooldown = game.training_mode_unlimited_cooldown;
            result.attack_dummy = game.training_mode_attack_dummy;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoSmiteSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, dragon = 1, buffs = 1;
        uint32_t monster_catcher = 0;
        bool initialized = false;
    };
    AutoSmiteSnapshot auto_smite_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoSmiteSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            const auto& game = pair->game;
            result.revision = game.auto_smite_command_revision;
            result.enabled = game.auto_smite_enabled;
            result.dragon = game.auto_smite_dragon;
            result.buffs = game.auto_smite_buffs;
            result.monster_catcher = game.auto_smite_monster_catcher;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct JungleTimerSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    JungleTimerSnapshot jungle_timer_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        JungleTimerSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.jungle_timer_command_revision;
            result.enabled = pair->game.jungle_timer_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct EyeTimerSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    EyeTimerSnapshot eye_timer_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        EyeTimerSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.eye_timer_command_revision;
            result.enabled = pair->game.eye_timer_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct TrapTimerSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    TrapTimerSnapshot trap_timer_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        TrapTimerSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.trap_timer_command_revision;
            result.enabled = pair->game.trap_timer_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct DisappearanceHintSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, duration_seconds = 30, show_heroes = 0;
        bool initialized = false;
    };
    DisappearanceHintSnapshot disappearance_hint_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        DisappearanceHintSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.disappearance_hint_command_revision;
            result.enabled = pair->game.disappearance_hint_enabled;
            result.duration_seconds = pair->game.disappearance_hint_duration_seconds;
            result.show_heroes = pair->game.disappearance_hint_show_heroes;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct MovementTraceSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    MovementTraceSnapshot movement_trace_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        MovementTraceSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.movement_trace_command_revision;
            result.enabled = pair->game.movement_trace_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct TowerRangeSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    TowerRangeSnapshot tower_range_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        TowerRangeSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.tower_range_command_revision;
            result.enabled = pair->game.tower_range_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AttackRangeSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AttackRangeSnapshot attack_range_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AttackRangeSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.attack_range_command_revision;
            result.enabled = pair->game.attack_range_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoAfkSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoAfkSnapshot auto_afk_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoAfkSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_afk_command_revision;
            result.enabled = pair->game.auto_afk_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct MoveDistanceSnapshot {
        uint64_t revision = 0;
        uint32_t distance_bits = chh::wire::game_move_distance_to_bits(chh::wire::game_move_distance_default);
        bool initialized = false;
    };
    MoveDistanceSnapshot move_distance_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        MoveDistanceSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.move_distance_command_revision;
            result.distance_bits = pair->game.move_distance_bits;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct HeroKeysSnapshot {
        uint64_t revision = 0;
        uint32_t combo_key = chh::wire::game_hero_combo_key_default;
        uint32_t toggle_key = chh::wire::game_hero_toggle_key_default;
        uint32_t lane_key = chh::wire::game_hero_lane_key_default;
        bool initialized = false;
    };
    HeroKeysSnapshot hero_keys_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        HeroKeysSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.hero_keys_command_revision;
            result.combo_key = pair->game.hero_combo_key;
            result.toggle_key = pair->game.hero_toggle_key;
            result.lane_key = pair->game.hero_lane_key;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct HeroDelaySnapshot {
        uint64_t revision = 0;
        uint32_t delay_ms = chh::wire::game_hero_delay_default_ms;
        bool initialized = false;
    };
    HeroDelaySnapshot hero_delay_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        HeroDelaySnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.hero_delay_command_revision;
            result.delay_ms = pair->game.hero_delay_ms;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct SkinBlockSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    SkinBlockSnapshot skin_block_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        SkinBlockSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.skin_block_command_revision;
            result.enabled = pair->game.skin_block_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct ProjectileDisplaySnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, enemy = 1, ally = 0;
        bool initialized = false;
    };
    ProjectileDisplaySnapshot projectile_display_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        ProjectileDisplaySnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.projectile_display_command_revision;
            result.enabled = pair->game.projectile_display_enabled;
            result.enemy = pair->game.projectile_display_enemy;
            result.ally = pair->game.projectile_display_ally;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDodgeSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, rectangles = 1, circles = 1;
        bool initialized = false;
    };
    AutoDodgeSnapshot auto_dodge_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDodgeSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_dodge_command_revision;
            result.enabled = pair->game.auto_dodge_enabled;
            result.rectangles = pair->game.auto_dodge_rectangles;
            result.circles = pair->game.auto_dodge_circles;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct LowHealthHintSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, threshold_percent = 30;
        bool initialized = false;
    };
    LowHealthHintSnapshot low_health_hint_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        LowHealthHintSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.low_health_hint_command_revision;
            result.enabled = pair->game.low_health_hint_enabled;
            result.threshold_percent = pair->game.low_health_hint_threshold_percent;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct SelectionShoutSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        std::string text;
        bool initialized = false;
    };
    SelectionShoutSnapshot selection_shout_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        SelectionShoutSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.selection_shout_command_revision;
            result.enabled = pair->game.selection_shout_enabled;
            result.text = pair->game.selection_shout_text;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoAcceptSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoAcceptSnapshot auto_accept_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoAcceptSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_accept_command_revision;
            result.enabled = pair->game.auto_accept_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct InstantPickSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    InstantPickSnapshot instant_pick_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        InstantPickSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.instant_pick_command_revision;
            result.enabled = pair->game.instant_pick_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoGloveSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoGloveSnapshot auto_glove_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoGloveSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_glove_command_revision;
            result.enabled = pair->game.auto_glove_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoLingzhouSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoLingzhouSnapshot auto_lingzhou_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoLingzhouSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_lingzhou_command_revision;
            result.enabled = pair->game.auto_lingzhou_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoFairySnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoFairySnapshot auto_fairy_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoFairySnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_fairy_command_revision;
            result.enabled = pair->game.auto_fairy_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoDevourSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0, minions = 1;
        bool initialized = false;
    };
    AutoDevourSnapshot auto_devour_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoDevourSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_devour_command_revision;
            result.enabled = pair->game.auto_devour_enabled;
            result.minions = pair->game.auto_devour_minions;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct AutoIgniteSnapshot {
        uint64_t revision = 0;
        uint32_t enabled = 0;
        bool initialized = false;
    };
    AutoIgniteSnapshot auto_ignite_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        AutoIgniteSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.auto_ignite_command_revision;
            result.enabled = pair->game.auto_ignite_enabled;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    struct HeroSelectSnapshot {
        uint64_t revision = 0;
        uint32_t hero_id = 0;
        bool initialized = false;
    };
    HeroSelectSnapshot hero_select_command_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        HeroSelectSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary)) {
            result.revision = pair->game.hero_select_command_revision;
            result.hero_id = pair->game.hero_select_id;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    CommandSnapshot game_close_snapshot(const Binding& binding) const {
        return command_snapshot(binding, &Game::close_command_revision);
    }
    struct TeamSnapshot {
        uint64_t revision = 0;
        std::string player_name;
        bool initialized = false;
    };
    TeamSnapshot game_team_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        TeamSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary, true)) {
            result.revision = pair->game.team_command_revision;
            result.player_name = pair->game.team_command_name;
            result.initialized = result.revision != 0 && !result.player_name.empty();
        }
        return result;
    }
    bool current_primary(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        return bound_locked(binding, ClientRole::primary) != nullptr;
    }
    // A game client is admitted only when the already-authenticated primary
    // for the same account is connected from the same observed peer IP.  The
    // IP comes from accept(), never from the login payload.
    bool primary_online(const std::string& account_id, uint32_t peer_ip) const {
        std::lock_guard lock(mutex_);
        const auto it = pairs_.find(Key{account_id, peer_ip});
        return it != pairs_.end() && it->second.primary != 0;
    }
    // The game endpoint does not authenticate independently, but the common
    // serving loop still uses an empty profile as a stale-session marker.
    // Copy the already-authenticated primary profile before entering the game
    // binding so a valid compact game login cannot be mistaken for a stale one.
    Profile primary_profile(const std::string& account_id, uint32_t peer_ip) const {
        std::lock_guard lock(mutex_);
        const auto it = pairs_.find(Key{account_id, peer_ip});
        if (it == pairs_.end() || !it->second.primary) return {};
        return it->second.profile;
    }
    bool current(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        return bound_locked(binding, binding.role) != nullptr;
    }
    bool enable_explorer(const Binding& binding) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return false;
        pair->explorer.ready = true;
        return true;
    }
    // The token was captured when the operator began entering the socket.
    // A recycled socket must never redirect that command to a new connection.
    ExplorerQueued queue_explorer(uintptr_t socket, uint64_t expected_token) {
        std::lock_guard lock(mutex_);
        if (!expected_token) return {};
        for (auto& [key, pair] : pairs_) {
            if (pair.primary != expected_token || pair.primary_socket != socket) continue;
            auto& task = pair.explorer;
            if (!task.ready) return {ExplorerQueueResult::not_ready, 0};
            if (task.request_id) return {ExplorerQueueResult::busy, 0};
            if (!++next_explorer_request_) ++next_explorer_request_;
            task.request_id = next_explorer_request_;
            task.dispatched = task.announced = false;
            task.expires = std::chrono::steady_clock::now() + std::chrono::minutes(6);
            return {ExplorerQueueResult::queued, task.request_id};
        }
        return {};
    }
    uint64_t take_explorer_request(const Binding& binding) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair || !pair->explorer.request_id || pair->explorer.dispatched) return 0;
        pair->explorer.dispatched = true;
        return pair->explorer.request_id;
    }
    bool explorer_result(const Binding& binding, uint64_t request_id, bool final) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair || !request_id || pair->explorer.request_id != request_id || !pair->explorer.dispatched) return false;
        auto& task = pair->explorer;
        if (final) { task = {}; task.ready = true; }
        else {
            if (task.announced) return false;
            task.announced = true;
        }
        return true;
    }
    uint64_t expire_explorer_request(const Binding& binding,
        std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now()) {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair || !pair->explorer.request_id || now < pair->explorer.expires) return 0;
        const auto request = pair->explorer.request_id;
        pair->explorer = {}; pair->explorer.ready = true;
        return request;
    }
    bool invalidate_account(const std::string& account_id) {
        std::lock_guard lock(mutex_);
        bool had_primary = false;
        std::vector<Key> removed_games;
        for (auto it = pairs_.begin(); it != pairs_.end();) {
            if (it->first.first != account_id) { ++it; continue; }
            if (it->second.primary) { ++primary_revision_; had_primary = true; }
            if (it->second.secondary) {
                ++game_revision_; ++presence_game_revision_;
                removed_games.push_back(it->first);
            }
            ++revision_;
            it = pairs_.erase(it);
        }
        const auto now = std::chrono::steady_clock::now();
        for (const auto& key : removed_games) expire_stats_routes_locked(&key, now);
        return had_primary;
    }
    void rename_account(const std::string& account_id, const std::string& name) {
        std::lock_guard lock(mutex_);
        const auto revision = ++profile_revision_;
        for (auto& [key, pair] : pairs_)
            if (key.first == account_id) { pair.profile.revision = revision; pair.profile.name = name; }
    }
    void account_preferences(const std::string& account_id, bool ready, bool hidden) {
        std::lock_guard lock(mutex_);
        const auto revision = ++profile_revision_;
        for (auto& [key, pair] : pairs_) {
            if (key.first != account_id) continue;
            pair.profile.revision = revision;
            pair.profile.preferences_ready = ready;
            pair.profile.hide_account_id = hidden;
        }
    }
    Profile profile_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, binding.role);
        return pair ? pair->profile : Profile{};
    }
    bool begin_account_update(const Binding& binding, std::chrono::steady_clock::time_point now) {
        std::lock_guard lock(mutex_);
        if (!bound_locked(binding, ClientRole::primary)) return false;
        for (const auto& [key, pair] : pairs_)
            if (key.first == binding.key.first && now < pair.next_account_update) return false;
        // Reserve the attempt before validation or database work; failures count.
        for (auto& [key, pair] : pairs_)
            if (key.first == binding.key.first) pair.next_account_update = now + std::chrono::seconds(1);
        return true;
    }
    GameStatus game_snapshot(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        GameStatus result = pair ? static_cast<const GameStatus&>(pair->game) : GameStatus{};
        result.game_connection_id = pair && result.connected ? pair->secondary : 0;
        return result;
    }
    GameStatus game_snapshot(const std::string& account_id) const {
        std::lock_guard lock(mutex_);
        const auto pair = account_game_locked(account_id);
        GameStatus result = pair && pair->secondary ? static_cast<const GameStatus&>(pair->game) : GameStatus{};
        result.game_connection_id = pair && result.connected ? pair->secondary : 0;
        return result;
    }
    void conversation_changed(const std::string& actor, const std::string& peer) {
        std::lock_guard lock(mutex_);
        const auto revision = ++conversation_revision_;
        for (auto& [key, pair] : pairs_)
            if (key.first == actor || key.first == peer) pair.conversation_revision = revision;
    }
    uint64_t conversation_revision(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        return pair ? pair->conversation_revision : 0;
    }
    void friend_notes_changed(const std::string& actor) {
        std::lock_guard lock(mutex_);
        const auto revision = ++notes_revision_;
        for (auto& [key, pair] : pairs_)
            if (key.first == actor) pair.notes_revision = revision;
    }
    uint64_t notes_revision(const Binding& binding) const {
        std::lock_guard lock(mutex_);
        const auto pair = bound_locked(binding, ClientRole::primary);
        return pair ? pair->notes_revision : 0;
    }
    void private_cleared(const std::string& actor, const std::string& peer, uint64_t through) {
        std::lock_guard lock(mutex_);
        const auto revision = ++private_clear_revision_;
        const auto conversation = ++conversation_revision_;
        for (auto& [key, pair] : pairs_) {
            if (key.first != actor) continue;
            pair.conversation_revision = conversation;
            pair.private_clear_revision = revision;
            push_event(pair.private_clears, pair.private_clears_dropped, PrivateClear{revision, through, peer});
        }
    }
    void private_recalled(const std::string& actor, const std::string& peer,
        uint64_t id, uint32_t timestamp) {
        std::lock_guard lock(mutex_);
        const auto revision = ++private_recall_revision_;
        for (auto& [key, pair] : pairs_) {
            if (key.first != actor && key.first != peer) continue;
            const auto& other = key.first == actor ? peer : actor;
            pair.private_recall_revision = revision;
            push_event(pair.private_recalls, pair.private_recalls_dropped, PrivateRecall{revision, id, timestamp, other});
        }
    }
    PrivateRecalls private_recalls(const Binding& binding, uint64_t after) const {
        return events_since<PrivateRecalls>(binding, after, &Pair::private_recall_revision,
            &Pair::private_recalls_dropped, &Pair::private_recalls);
    }
    PrivateClears private_clears(const Binding& binding, uint64_t after) const {
        return events_since<PrivateClears>(binding, after, &Pair::private_clear_revision,
            &Pair::private_clears_dropped, &Pair::private_clears);
    }
    Presence presence() const {
        std::lock_guard lock(mutex_);
        if (presence_cache_valid_ && presence_cache_.revision == primary_revision_ &&
            presence_cache_.game_revision == presence_game_revision_) return presence_cache_;
        Presence result; result.revision = primary_revision_; result.game_revision = presence_game_revision_;
        for (auto it = pairs_.begin(); it != pairs_.end();) {
            const auto& account_id = it->first.first;
            const auto pair = account_game_locked(account_id);
            if (pair && pair->primary) result.primary_accounts.insert(account_id);
            if (pair && pair->secondary) {
                result.game_states[account_id] = pair->game.state;
                result.game_connected[account_id] = pair->game.connected;
                result.game_teams[account_id] = pair->game.team;
                result.game_player_names[account_id] = pair->game.player_name;
            }
            it = pairs_.upper_bound(Key{account_id, UINT32_MAX});
        }
        presence_cache_ = result;
        presence_cache_valid_ = true;
        return result;
    }
    Snapshot snapshot() const {
        std::lock_guard lock(mutex_);
        if (snapshot_cache_valid_ && snapshot_cache_.revision == revision_) return snapshot_cache_;
        std::set<std::string> accounts;
        Snapshot result; result.revision = revision_;
        for (const auto& [key, pair] : pairs_) {
            accounts.insert(key.first);
            if (pair.primary) ++result.primary;
            if (pair.secondary) ++result.secondary;
        }
        result.accounts = static_cast<uint32_t>(accounts.size());
        snapshot_cache_ = result;
        snapshot_cache_valid_ = true;
        return result;
    }
    // Counts and names are copied together; console output happens after unlock.
    // Enumerate every connection, including games at other IPs of one account.
    ConnectionsSnapshot connections_snapshot() const {
        std::lock_guard lock(mutex_);
        ConnectionsSnapshot result;
        result.count.revision = revision_;
        std::set<std::string> accounts;
        for (const auto& [key, pair] : pairs_) {
            if (pair.primary) {
                ++result.count.primary;
                result.clients.push_back({ClientRole::primary, pair.profile.name, pair.primary_socket, key.second, pair.primary});
            }
            if (pair.secondary) {
                ++result.count.secondary;
                result.clients.push_back({ClientRole::secondary, pair.game.player_name, pair.secondary_socket, key.second, pair.secondary});
            }
            if (pair.primary || pair.secondary) accounts.insert(key.first);
        }
        result.count.accounts = static_cast<uint32_t>(accounts.size());
        return result;
    }
private:
    using SteadyTime = std::chrono::steady_clock::time_point;
    // An unanswered stats request is answered as unavailable after this long;
    // a late reply for it is ignored like any other unknown request id.
    static constexpr std::chrono::seconds stats_route_timeout{60};
    static constexpr size_t stats_route_limit = 256, stats_queue_limit = 32, stats_result_limit = 32;
    struct StatsRoute {
        Key requester;
        uint64_t requester_primary = 0;
        Key target;
        std::set<uint32_t> uids;
        SteadyTime expires{};
    };
    struct Pair {
        struct ExplorerTask {
            bool ready = false, dispatched = false, announced = false;
            uint64_t request_id = 0;
            SteadyTime expires{};
        } explorer;
        uint64_t primary = 0, secondary = 0, conversation_revision = 0, notes_revision = 0;
        uintptr_t primary_socket = 0, secondary_socket = 0;
        uint64_t private_clear_revision = 0, private_clears_dropped = 0;
        uint64_t private_recall_revision = 0, private_recalls_dropped = 0;
        std::deque<PrivateClear> private_clears;
        std::deque<PrivateRecall> private_recalls;
        Game game;
        Profile profile;
        SteadyTime next_account_update{};
    };
    // The pair this binding still owns in the given role, or nullptr when the
    // binding is empty, has another role or was replaced/invalidated.
    Pair* bound_locked(const Binding& binding, ClientRole role, bool game_connected = false) {
        return const_cast<Pair*>(std::as_const(*this).bound_locked(binding, role, game_connected));
    }
    const Pair* bound_locked(const Binding& binding, ClientRole role, bool game_connected = false) const {
        if (!binding || binding.role != role) return nullptr;
        const auto it = pairs_.find(binding.key);
        if (it == pairs_.end()) return nullptr;
        const auto& pair = it->second;
        if ((role == ClientRole::primary ? pair.primary : pair.secondary) != binding.token) return nullptr;
        return game_connected && !pair.game.connected ? nullptr : &pair;
    }
    // A primary's pair while its own game endpoint is connected.
    Pair* paired_game_locked(const Binding& binding) {
        const auto pair = bound_locked(binding, ClientRole::primary);
        return pair && pair->secondary && pair->game.connected ? pair : nullptr;
    }
    // An online primary selects its own IP, including when its game is absent.
    // Without a primary retain offline game presence: highest state wins,
    // breaking ties by the most recently connected game, never by IP order.
    const Pair* account_game_locked(const std::string& account_id) const {
        const Pair* selected = nullptr;
        for (auto it = pairs_.lower_bound(Key{account_id, 0});
            it != pairs_.end() && it->first.first == account_id; ++it) {
            const auto& pair = it->second;
            if (pair.primary) return &pair;
            if (pair.secondary && pair.game.connected && (!selected ||
                pair.game.state > selected->game.state ||
                (pair.game.state == selected->game.state && pair.secondary > selected->secondary)))
                selected = &pair;
        }
        return selected;
    }
    Pair* account_game_locked(const std::string& account_id) {
        return const_cast<Pair*>(std::as_const(*this).account_game_locked(account_id));
    }
    void clear_team_commands_locked(const std::string& account_id) {
        for (auto it = pairs_.lower_bound(Key{account_id, 0});
            it != pairs_.end() && it->first.first == account_id; ++it) {
            it->second.game.team_command_revision = 0;
            it->second.game.team_command_name.clear();
        }
    }
    bool queue_game_command(const Binding& binding, uint64_t Game::* command, uint64_t& counter) {
        std::lock_guard lock(mutex_);
        const auto pair = paired_game_locked(binding);
        if (!pair) return false;
        pair->game.*command = ++counter;
        return true;
    }
    CommandSnapshot command_snapshot(const Binding& binding, uint64_t Game::* command) const {
        std::lock_guard lock(mutex_);
        CommandSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::secondary, true)) {
            result.revision = pair->game.*command;
            result.initialized = result.revision != 0;
        }
        return result;
    }
    PlayersSnapshot players_snapshot(const Binding& binding, uint64_t Game::* revision,
        std::vector<Game::Player> Game::* players, uint64_t known_revision) const {
        std::lock_guard lock(mutex_);
        PlayersSnapshot result;
        if (const auto pair = bound_locked(binding, ClientRole::primary, true)) {
            result.revision = pair->game.*revision;
            result.initialized = result.revision != 0;
            if (result.revision != known_revision) result.players = pair->game.*players;
        }
        return result;
    }
    static void prune_roster_heroes(const chh::wire::GameAutoDisarmRoster& roster, std::vector<uint32_t>& hero_ids) {
        hero_ids.erase(std::remove_if(hero_ids.begin(), hero_ids.end(),
            [&](uint32_t hero) { return !chh::wire::game_auto_disarm_roster_contains(roster, hero); }), hero_ids.end());
        std::sort(hero_ids.begin(), hero_ids.end());
    }
    struct RosterChanges {
        bool new_match, players_changed, exclusions_changed;
    };
    static RosterChanges prepare_roster_update(const chh::wire::GameAutoDisarmRoster& current,
        chh::wire::GameAutoDisarmRoster& roster) {
        const bool new_match = current.match_id != roster.match_id;
        if (!new_match) {
            roster.excluded_hero_ids = current.excluded_hero_ids;
            prune_roster_heroes(roster, roster.excluded_hero_ids);
        } else {
            std::sort(roster.excluded_hero_ids.begin(), roster.excluded_hero_ids.end());
        }
        const bool exclusions_changed = roster.excluded_hero_ids != current.excluded_hero_ids;
        const bool players_changed = roster.players.size() != current.players.size() ||
            !std::equal(roster.players.begin(), roster.players.end(), current.players.begin(),
                [](const auto& a, const auto& b) { return a.uid == b.uid && a.hero_id == b.hero_id; });
        return {new_match, players_changed, exclusions_changed};
    }
    static bool valid_players(const std::vector<Game::Player>& players) {
        if (players.size() > 14) return false;
        for (const auto& player : players)
            if (!player.uid || player.team > 255 || player.name.empty() ||
                player.name.size() > chh::wire::profile_name_limit || !chh::wire::valid_profile_name(player.name))
                return false;
        return true;
    }
    template<class T> static std::vector<T> drain(std::deque<T>& queue) {
        std::vector<T> result(std::make_move_iterator(queue.begin()), std::make_move_iterator(queue.end()));
        queue.clear();
        return result;
    }
    // A game endpoint (re)connects or leaves: status and one-shot commands
    // reset. The latest primary control settings and requested player data
    // survive a temporary reconnect and are delivered after the next login.
    void reset_game_locked(Game& game, bool connected) {
        Game fresh;
        fresh.revision = ++game_revision_;
        fresh.connected = connected;
        fresh.sight_command_revision = game.sight_command_revision;
        fresh.sight_command = game.sight_command;
        fresh.skill_hint_command_revision = game.skill_hint_command_revision;
        fresh.skill_hint_enabled = game.skill_hint_enabled;
        fresh.skill_hint_ally = game.skill_hint_ally;
        fresh.skill_hint_enemy = game.skill_hint_enemy;
        fresh.skill_hint_hero_skills = game.skill_hint_hero_skills;
        fresh.status_display_command_revision = game.status_display_command_revision;
        fresh.status_display_enabled = game.status_display_enabled;
        fresh.status_display_ally = game.status_display_ally;
        fresh.status_display_enemy = game.status_display_enemy;
        fresh.auto_cleanse_command_revision = game.auto_cleanse_command_revision;
        fresh.auto_disarm_command_revision = game.auto_disarm_command_revision;
        fresh.auto_disarm_enabled = game.auto_disarm_enabled;
        fresh.auto_disarm_on_skill = game.auto_disarm_on_skill;
        fresh.auto_disarm_direct = game.auto_disarm_direct;
        fresh.auto_disarm_range_bits = game.auto_disarm_range_bits;
        fresh.auto_disarm_roster = std::move(game.auto_disarm_roster);
        fresh.auto_disarm_roster_revision = game.auto_disarm_roster_revision;
        fresh.auto_disarm_exclusions_revision = game.auto_disarm_exclusions_revision;
        fresh.auto_disarm_request_id = game.auto_disarm_request_id;
        // fresh.auto_disarm_roster_fresh remains false until this endpoint sends 137.
        fresh.auto_guisuo_command_revision = game.auto_guisuo_command_revision;
        fresh.auto_guisuo_enabled = game.auto_guisuo_enabled;
        fresh.auto_guisuo_min_count = game.auto_guisuo_min_count;
        fresh.auto_guisuo_targets = std::move(game.auto_guisuo_targets);
        // Each side must identify this connection's match before retained exclusions are exposed.
        for (auto& target : fresh.auto_guisuo_targets) target.roster_fresh = false;
        fresh.auto_dihuo_command_revision = game.auto_dihuo_command_revision;
        fresh.auto_dihuo_enabled = game.auto_dihuo_enabled;
        fresh.auto_dihuo_range_bits = game.auto_dihuo_range_bits;
        fresh.auto_dihuo_roster = std::move(game.auto_dihuo_roster);
        fresh.auto_dihuo_roster_revision = game.auto_dihuo_roster_revision;
        fresh.auto_dihuo_exclusions_revision = game.auto_dihuo_exclusions_revision;
        fresh.auto_dihuo_request_id = game.auto_dihuo_request_id;
        // auto_dihuo_roster_fresh stays false until this connection identifies its match.
        fresh.auto_douji_command_revision = game.auto_douji_command_revision;
        fresh.auto_douji_flags = game.auto_douji_flags;
        fresh.auto_douji_range_bits = game.auto_douji_range_bits;
        fresh.auto_douji_roster = std::move(game.auto_douji_roster);
        fresh.auto_douji_roster_revision = game.auto_douji_roster_revision;
        fresh.auto_douji_exclusions_revision = game.auto_douji_exclusions_revision;
        fresh.auto_douji_request_id = game.auto_douji_request_id;
        // auto_douji_roster_fresh stays false until this connection identifies its match.
        fresh.auto_dark_shield_command_revision = game.auto_dark_shield_command_revision;
        fresh.auto_dark_shield_enabled = game.auto_dark_shield_enabled;
        fresh.auto_dark_shield_min_count = game.auto_dark_shield_min_count;
        fresh.auto_dark_shield_use_dark_shield = game.auto_dark_shield_use_dark_shield;
        fresh.auto_dark_shield_use_dining_table = game.auto_dark_shield_use_dining_table;
        fresh.auto_dark_shield_roster = std::move(game.auto_dark_shield_roster);
        fresh.auto_dark_shield_roster_revision = game.auto_dark_shield_roster_revision;
        fresh.auto_dark_shield_exclusions_revision = game.auto_dark_shield_exclusions_revision;
        fresh.auto_dark_shield_request_id = game.auto_dark_shield_request_id;
        // auto_dark_shield_roster_fresh stays false until this connection identifies its match.
        fresh.auto_heal_command_revision = game.auto_heal_command_revision;
        fresh.auto_heal_enabled = game.auto_heal_enabled;
        fresh.auto_heal_health_percent = game.auto_heal_health_percent;
        fresh.auto_heal_roster = std::move(game.auto_heal_roster);
        fresh.auto_heal_roster_revision = game.auto_heal_roster_revision;
        fresh.auto_heal_exclusions_revision = game.auto_heal_exclusions_revision;
        fresh.auto_heal_request_id = game.auto_heal_request_id;
        // auto_heal_roster_fresh stays false until this connection identifies its match.
        fresh.near_items = std::move(game.near_items);
        // Each slot must identify this game connection's match before retained exclusions are exposed.
        for (auto& item : fresh.near_items) item.roster_fresh = false;
        fresh.auto_cleanse_enabled = game.auto_cleanse_enabled;
        fresh.auto_cleanse_ignite = game.auto_cleanse_ignite;
        fresh.auto_cleanse_disarm = game.auto_cleanse_disarm;
        fresh.auto_cleanse_root = game.auto_cleanse_root;
        fresh.auto_cleanse_control = game.auto_cleanse_control;
        fresh.auto_cleanse_silence = game.auto_cleanse_silence;
        fresh.auto_cleanse_stun = game.auto_cleanse_stun;
        fresh.auto_cleanse_taunt = game.auto_cleanse_taunt;
        fresh.auto_cleanse_delay_ms = game.auto_cleanse_delay_ms;
        fresh.auto_cleanse_min_count = game.auto_cleanse_min_count;
        fresh.auto_cleanse_control_time_ms = game.auto_cleanse_control_time_ms;
        fresh.training_mode_command_revision = game.training_mode_command_revision;
        fresh.training_mode_enabled = game.training_mode_enabled;
        fresh.training_mode_unlimited_cooldown = game.training_mode_unlimited_cooldown;
        fresh.training_mode_attack_dummy = game.training_mode_attack_dummy;
        fresh.auto_smite_command_revision = game.auto_smite_command_revision;
        fresh.auto_smite_enabled = game.auto_smite_enabled;
        fresh.auto_smite_dragon = game.auto_smite_dragon;
        fresh.auto_smite_buffs = game.auto_smite_buffs;
        fresh.auto_smite_monster_catcher = game.auto_smite_monster_catcher;
        fresh.jungle_timer_command_revision = game.jungle_timer_command_revision;
        fresh.jungle_timer_enabled = game.jungle_timer_enabled;
        fresh.eye_timer_command_revision = game.eye_timer_command_revision;
        fresh.eye_timer_enabled = game.eye_timer_enabled;
        fresh.trap_timer_command_revision = game.trap_timer_command_revision;
        fresh.trap_timer_enabled = game.trap_timer_enabled;
        fresh.disappearance_hint_command_revision = game.disappearance_hint_command_revision;
        fresh.disappearance_hint_enabled = game.disappearance_hint_enabled;
        fresh.disappearance_hint_duration_seconds = game.disappearance_hint_duration_seconds;
        fresh.disappearance_hint_show_heroes = game.disappearance_hint_show_heroes;
        fresh.movement_trace_command_revision = game.movement_trace_command_revision;
        fresh.movement_trace_enabled = game.movement_trace_enabled;
        fresh.tower_range_command_revision = game.tower_range_command_revision;
        fresh.tower_range_enabled = game.tower_range_enabled;
        fresh.attack_range_command_revision = game.attack_range_command_revision;
        fresh.attack_range_enabled = game.attack_range_enabled;
        fresh.auto_afk_command_revision = game.auto_afk_command_revision;
        fresh.auto_afk_enabled = game.auto_afk_enabled;
        fresh.move_distance_command_revision = game.move_distance_command_revision;
        fresh.move_distance_bits = game.move_distance_bits;
        fresh.hero_keys_command_revision = game.hero_keys_command_revision;
        fresh.hero_combo_key = game.hero_combo_key;
        fresh.hero_toggle_key = game.hero_toggle_key;
        fresh.hero_lane_key = game.hero_lane_key;
        fresh.hero_delay_command_revision = game.hero_delay_command_revision;
        fresh.hero_delay_ms = game.hero_delay_ms;
        fresh.skin_block_command_revision = game.skin_block_command_revision;
        fresh.skin_block_enabled = game.skin_block_enabled;
        fresh.projectile_display_command_revision = game.projectile_display_command_revision;
        fresh.projectile_display_enabled = game.projectile_display_enabled;
        fresh.projectile_display_enemy = game.projectile_display_enemy;
        fresh.projectile_display_ally = game.projectile_display_ally;
        fresh.auto_dodge_command_revision = game.auto_dodge_command_revision;
        fresh.auto_dodge_enabled = game.auto_dodge_enabled;
        fresh.auto_dodge_rectangles = game.auto_dodge_rectangles;
        fresh.auto_dodge_circles = game.auto_dodge_circles;
        fresh.low_health_hint_command_revision = game.low_health_hint_command_revision;
        fresh.low_health_hint_enabled = game.low_health_hint_enabled;
        fresh.low_health_hint_threshold_percent = game.low_health_hint_threshold_percent;
        fresh.instant_pick_command_revision = game.instant_pick_command_revision;
        fresh.instant_pick_enabled = game.instant_pick_enabled;
        fresh.auto_accept_command_revision = game.auto_accept_command_revision;
        fresh.auto_accept_enabled = game.auto_accept_enabled;
        fresh.selection_shout_command_revision = game.selection_shout_command_revision;
        fresh.selection_shout_enabled = game.selection_shout_enabled;
        fresh.selection_shout_text = std::move(game.selection_shout_text);
        fresh.auto_ignite_command_revision = game.auto_ignite_command_revision;
        fresh.auto_ignite_enabled = game.auto_ignite_enabled;
        fresh.auto_fairy_command_revision = game.auto_fairy_command_revision;
        fresh.auto_fairy_enabled = game.auto_fairy_enabled;
        fresh.auto_devour_command_revision = game.auto_devour_command_revision;
        fresh.auto_devour_enabled = game.auto_devour_enabled;
        fresh.auto_devour_minions = game.auto_devour_minions;
        fresh.auto_glove_command_revision = game.auto_glove_command_revision;
        fresh.auto_glove_enabled = game.auto_glove_enabled;
        fresh.auto_lingzhou_command_revision = game.auto_lingzhou_command_revision;
        fresh.auto_lingzhou_enabled = game.auto_lingzhou_enabled;
        fresh.hero_select_command_revision = game.hero_select_command_revision;
        fresh.hero_select_id = game.hero_select_id;
        fresh.players_request_revision = game.players_request_revision;
        fresh.players = std::move(game.players);
        fresh.players_revision = game.players_revision;
        fresh.output_request_revision = game.output_request_revision;
        fresh.output_players = std::move(game.output_players);
        fresh.output_revision = game.output_revision;
        game = std::move(fresh);
        ++presence_game_revision_;
    }
    // Bounded per-connection event history; dropped marks the oldest lost revision.
    template<class Event> static void push_event(std::deque<Event>& events, uint64_t& dropped, Event event) {
        events.push_back(std::move(event));
        if (events.size() > 128) {
            dropped = events.front().revision;
            events.pop_front();
        }
    }
    template<class Result, class Event> Result events_since(const Binding& binding, uint64_t after,
        uint64_t Pair::* revision, uint64_t Pair::* dropped, std::deque<Event> Pair::* events) const {
        std::lock_guard lock(mutex_);
        Result result;
        const auto pair = bound_locked(binding, ClientRole::primary);
        if (!pair) return result;
        result.revision = pair->*revision;
        result.reset = after < pair->*dropped;
        for (const auto& event : pair->*events)
            if (event.revision > after) result.changes.push_back(event);
        return result;
    }
    static Game::StatsEntry unavailable_stats(uint32_t request_id, uint32_t uid) {
        Game::StatsEntry entry; // result defaults to GameStatsResult::unavailable
        entry.request_id = request_id; entry.uid = uid;
        return entry;
    }
    static Game::StatsBatch unavailable_stats_batch(uint32_t request_id, const std::set<uint32_t>& uids) {
        Game::StatsBatch batch; batch.request_id = request_id;
        for (const auto uid : uids) batch.entries.push_back(unavailable_stats(request_id, uid));
        return batch;
    }
    static void push_stats_result(Game& game, Game::StatsBatch batch) {
        if (game.stats_results.size() >= stats_result_limit) game.stats_results.pop_front();
        game.stats_results.push_back(std::move(batch));
    }
    // The primary that asked, unless it disconnected or was replaced since.
    Pair* route_requester_locked(const StatsRoute& route) {
        const auto it = pairs_.find(route.requester);
        return it != pairs_.end() && it->second.primary == route.requester_primary ? &it->second : nullptr;
    }
    // Answers and drops routes whose target endpoint is gone (target given) or
    // that timed out, so neither the table nor a waiting requester leaks.
    void expire_stats_routes_locked(const Key* target, SteadyTime now) {
        for (auto it = stats_routes_.begin(); it != stats_routes_.end();) {
            if ((target && it->second.target == *target) || now >= it->second.expires) {
                if (const auto requester = route_requester_locked(it->second))
                    push_stats_result(requester->game, unavailable_stats_batch(it->first, it->second.uids));
                // A slow target may not have taken the request yet. Remove it
                // as well so expired work cannot be sent or occupy queue space.
                const auto endpoint = pairs_.find(it->second.target);
                if (endpoint != pairs_.end()) {
                    auto& requests = endpoint->second.game.stats_requests;
                    for (auto request = requests.begin(); request != requests.end();) {
                        if (request->request_id == it->first) request = requests.erase(request);
                        else ++request;
                    }
                }
                it = stats_routes_.erase(it);
            } else ++it;
        }
    }
    void queue_game_stats_locked(const Key& requester_key, Pair& requester) {
        if (!requester.primary || requester.game.players.empty()) return;
        const auto now = std::chrono::steady_clock::now();
        expire_stats_routes_locked(nullptr, now);
        std::vector<Game::StatsTarget> query;
        std::set<uint32_t> expected;
        for (const auto& player : requester.game.players) {
            if (!player.uid || !expected.insert(player.uid).second) continue;
            if (player.identification_code)
                query.push_back({player.uid, player.identification_code});
        }
        if (expected.empty()) return;
        uint32_t request_id = 0;
        do { request_id = ++stats_request_id_; } while (!request_id || stats_routes_.count(request_id));
        Key target_key{};
        Pair* target_pair = nullptr;
        for (auto& [key, pair] : pairs_) {
            // Only a connected lobby or matchmaking endpoint is allowed to
            // issue the game's hidden-profile request.
            if (!pair.secondary || !pair.game.connected ||
                (pair.game.state != 1 && pair.game.state != 2)) continue;
            target_key = key; target_pair = &pair; break;
        }
        if (!target_pair || query.empty() || stats_routes_.size() >= stats_route_limit ||
            target_pair->game.stats_requests.size() >= stats_queue_limit) {
            push_stats_result(requester.game, unavailable_stats_batch(request_id, expected));
            return;
        }
        StatsRoute route;
        route.requester = requester_key;
        route.requester_primary = requester.primary;
        route.target = target_key;
        route.uids = std::move(expected);
        route.expires = now + stats_route_timeout;
        stats_routes_.emplace(request_id, std::move(route));
        target_pair->game.stats_requests.push_back({request_id, std::move(query)});
    }
    mutable std::mutex mutex_;
    std::map<Key, Pair> pairs_;
    uint64_t next_token_ = 0, revision_ = 0, game_revision_ = 0, primary_revision_ = 0;
    uint64_t next_explorer_request_ = 0;
    // Bumped only when a field shown in friends' lists changes (Presence).
    uint64_t presence_game_revision_ = 0;
    uint64_t game_close_revision_ = 0, team_command_revision_ = 0;
    uint64_t sight_revision_ = 0, sight_command_revision_ = 0;
    uint64_t skill_hint_command_revision_ = 0;
    uint64_t status_display_command_revision_ = 0;
    uint64_t auto_cleanse_command_revision_ = 0;
    uint64_t auto_disarm_command_revision_ = 0;
    uint64_t auto_disarm_roster_revision_ = 0, auto_disarm_exclusions_revision_ = 0;
    uint64_t auto_guisuo_command_revision_ = 0;
    std::array<uint64_t, chh::wire::game_auto_guisuo_side_count> auto_guisuo_roster_revisions_{}, auto_guisuo_exclusions_revisions_{};
    uint64_t auto_dihuo_command_revision_ = 0;
    uint64_t auto_dihuo_roster_revision_ = 0, auto_dihuo_exclusions_revision_ = 0;
    uint64_t auto_douji_command_revision_ = 0;
    uint64_t auto_douji_roster_revision_ = 0, auto_douji_exclusions_revision_ = 0;
    uint64_t auto_dark_shield_command_revision_ = 0;
    uint64_t auto_dark_shield_roster_revision_ = 0, auto_dark_shield_exclusions_revision_ = 0;
    uint64_t auto_heal_command_revision_ = 0;
    uint64_t auto_heal_roster_revision_ = 0, auto_heal_exclusions_revision_ = 0;
    std::array<uint64_t, chh::wire::game_near_item_count> near_item_command_revisions_{};
    std::array<uint64_t, chh::wire::game_near_item_count> near_item_roster_revisions_{}, near_item_exclusions_revisions_{};
    uint64_t training_mode_command_revision_ = 0;
    uint64_t auto_smite_command_revision_ = 0;
    uint64_t jungle_timer_command_revision_ = 0;
    uint64_t eye_timer_command_revision_ = 0;
    uint64_t trap_timer_command_revision_ = 0;
    uint64_t disappearance_hint_command_revision_ = 0;
    uint64_t movement_trace_command_revision_ = 0;
    uint64_t tower_range_command_revision_ = 0;
    uint64_t attack_range_command_revision_ = 0;
    uint64_t auto_afk_command_revision_ = 0;
    uint64_t move_distance_command_revision_ = 0;
    uint64_t hero_keys_command_revision_ = 0;
    uint64_t hero_delay_command_revision_ = 0;
    uint64_t skin_block_command_revision_ = 0;
    uint64_t projectile_display_command_revision_ = 0;
    uint64_t auto_dodge_command_revision_ = 0;
    uint64_t low_health_hint_command_revision_ = 0;
    uint64_t instant_pick_command_revision_ = 0;
    uint64_t auto_accept_command_revision_ = 0;
    uint64_t selection_shout_command_revision_ = 0;
    uint64_t auto_ignite_command_revision_ = 0;
    uint64_t auto_fairy_command_revision_ = 0, auto_devour_command_revision_ = 0;
    uint64_t auto_glove_command_revision_ = 0;
    uint64_t auto_lingzhou_command_revision_ = 0;
    uint64_t hero_select_command_revision_ = 0;
    uint64_t players_request_revision_ = 0, players_revision_ = 0;
    uint64_t output_request_revision_ = 0, output_revision_ = 0;
    uint32_t stats_request_id_ = 0;
    std::map<uint32_t, StatsRoute> stats_routes_;
    uint64_t conversation_revision_ = 0;
    uint64_t profile_revision_ = 0;
    uint64_t notes_revision_ = 0;
    uint64_t private_clear_revision_ = 0;
    uint64_t private_recall_revision_ = 0;
    mutable bool snapshot_cache_valid_ = false, presence_cache_valid_ = false;
    mutable Snapshot snapshot_cache_;
    mutable Presence presence_cache_;
};
}
