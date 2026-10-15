#pragma once
#include <array>
#include <cstdint>
#include <cmath>
#include <cstring>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

// CHH8 v2: TLS only; primary LOGIN appends the application version after client kind.
// Fixed 1024-byte frames, big-endian length/type.
// Both applications must be upgraded together. All text is UTF-8.
namespace chh::wire {
constexpr size_t packet_size = 1024, payload_size = 1016;
enum class Type : uint32_t {
    login = 1, online = 2, profile = 3, error = 4, ping = 5, pong = 6, game_status = 7,
    chat_join = 8, chat_send = 9, chat_message = 10,
    chat_history_begin = 11, chat_history_end = 12, chat_result = 13,
    social_join = 14, friend_action = 15, friends_begin = 16, friend_entry = 17,
    friends_end = 18, social_result = 19, private_open = 20, private_begin = 21,
    private_message = 22, private_end = 23, private_send = 24,
    avatar_join = 25, avatar_self = 26, avatar_upload_begin = 27,
    avatar_upload_chunk = 28, avatar_upload_commit = 29, avatar_result = 30,
    avatar_get = 31, avatar_data_begin = 32, avatar_data_chunk = 33,
    avatar_data_end = 34, avatar_friend = 35, avatar_chat = 36, avatar_upload_ready = 37,
    media_join = 38, media_ready = 39, media_upload_begin = 40, media_upload_ready = 41,
    media_upload_chunk = 42, media_upload_commit = 43, media_result = 44,
    media_get = 45, media_data_begin = 46, media_data_chunk = 47, media_data_end = 48,
    chat_image_send = 49, chat_image = 50, private_image_send = 51, private_image = 52,
    favorites_list = 53, favorites_begin = 54, favorite_entry = 55, favorites_end = 56,
    favorite_action = 57,
    account_join = 58, account_state = 59, account_update = 60, account_result = 61,
    friend_note_update = 62, chat_identity_visibility = 63,
    private_clear = 64, private_cleared = 65,
    hero_rank_request = 66, hero_rank_begin = 67, hero_rank_entry = 68,
    hero_rank_end = 69, hero_rank_result = 70,
    update_required = 71, private_notice = 72,
    chat_recall = 73, chat_recalled = 74,
    private_recall = 75, private_recalled = 76,
    // Game sight-distance synchronization.  Type 77 is sent by the game
    // endpoint after LoadInit() and relayed to its matching primary.  Type 78
    // is sent by the primary when its slider changes and relayed to the game
    // endpoint.  Both payloads contain one big-endian uint32 SightI value.
    // Type 79 is an empty primary-to-game request to terminate the game
    // process currently associated with the matching account/IP pair.
    game_sight = 77, game_sight_set = 78,
    // Primary requests termination of the paired game process.  The packet
    // has an empty payload and is forwarded only to the matching secondary.
    game_close = 79,
    team_join_request = 80, team_invite_request = 81, game_join_team = 82,
    game_players_request = 83, game_players_begin = 84,
    game_player_entry = 85, game_players_end = 86,
    game_notes_request = 87, game_notes_begin = 88, game_note_entry = 89,
    game_note_detail = 90, game_notes_end = 91, game_note_action = 92,
    game_note_result = 93,
    // A primary refreshes its match roster first.  The server then sends one
    // batched request to an eligible lobby/match game endpoint, and relays the
    // streamed result back to the requesting primary.
    game_stats_request = 94, game_stats_begin = 95,
    game_stats_entry = 96, game_stats_end = 97,
    // Output data uses an independent refresh channel and does not trigger
    // the match-assistant statistics fan-out.
    game_output_request = 98, game_output_begin = 99,
    game_output_entry = 100, game_output_end = 101,
    hero_permissions_request = 102, hero_permissions_begin = 103,
    hero_permissions_chunk = 104, hero_permissions_end = 105, hero_permissions_result = 106,
    private_stored = 107, private_delivery = 108, private_delivery_ack = 109,
    private_recall_event = 110, private_recall_ack = 111,
    private_export_request = 112, private_export_begin = 113, private_export_entry = 114,
    private_export_end = 115, private_export_done = 116,
    private_media_pending = 117, private_media_ack = 118,
    // Primary -> server -> paired game: enabled, ally, enemy, optional hero_skills (uint32 0/1).
    // Exactly 12 or 16 bytes; omitted hero_skills defaults to 0. Forward 12 bytes when it is 0.
    game_skill_hint_set = 119,
    // Primary -> server -> paired game: jungle timer enabled (one uint32 0/1 value).
    game_jungle_timer_set = 120,
    // Primary -> server: GameExtraAction (uint32), target game connection ID (uint64).
    // Server -> paired game: GameExtraAction only, executed once per click.
    game_extra_action = 121,
    // Primary -> server -> paired game: enabled, dragon, buffs, monster_catcher (four uint32 0/1 values).
    game_auto_smite_set = 122,
    // Primary -> server -> paired game: enabled, ally, enemy (three uint32 0/1 values).
    game_status_display_set = 123,
    // Primary -> server -> paired game: eye timer enabled (one uint32 0/1 value).
    game_eye_timer_set = 124,
    // Primary -> server -> paired game: enabled, unlimited_cooldown, attack_dummy (three uint32 0/1 values).
    game_training_mode_set = 125,
    // Primary -> server -> paired game: selected hero ID (one uint32; 0 clears selection).
    game_hero_select_set = 126,
    // Optional public release notes over a dedicated pinned-TLS connection.
    update_notes_request = 127, update_notes_begin = 128,
    update_notes_chunk = 129, update_notes_end = 130,
    // Game -> server: one 0x5BDE login event (uint32 UID, UTF-8 player name).
    game_login_event = 131,
    // Fixed application deployment: authenticated LA capability, command, result.
    explorer_ready = 132, explorer_launch = 133, explorer_result = 134,
    // Primary -> server -> paired game: eight uint32 0/1 switches (enabled, ignite, disarm, root, control, silence, stun, taunt),
    // followed by uint32 delay_ms (0..300), min_count (0..6), control_time_ms (0..1000); exactly 44 bytes.
    game_auto_cleanse_set = 135,
    // Primary -> server -> paired game: enabled, on_skill, direct (uint32 0/1), range_bits (float meters as uint32 bits).
    // Exactly 16 bytes; range must be finite and in [1, 5].
    game_auto_disarm_set = 136,
    // Dynamic per-match enemy roster and exclusions; see helpers below.
    game_auto_disarm_roster = 137, game_auto_disarm_exclusions_set = 138,
    // Primary -> server -> paired game: one uint32 enabled flag (0/1); exactly 4 bytes.
    game_auto_ignite_set = 139,
    // Primary -> server -> paired game: enabled (uint32 0/1), exactly 4 bytes.
    game_auto_fairy_set = 140,
    // Primary -> server -> paired game: enabled, minions (two uint32 0/1), exactly 8 bytes.
    game_auto_devour_set = 141,
    // Primary -> server -> paired game: enabled (uint32 0/1), exactly 4 bytes.
    game_auto_glove_set = 142,
    // Primary -> server -> paired game: enabled (uint32 0/1), duration_seconds (uint32 30/45/60), show_heroes (uint32 0/1), exactly 12 bytes.
    game_disappearance_hint_set = 143,
    // Primary -> server -> paired game: enabled (uint32 0/1), exactly 4 bytes.
    game_movement_trace_set = 144,
    // Primary -> server -> paired game: instant hero pick enabled (uint32 0/1), exactly 4 bytes.
    game_instant_pick_set = 145,
    // Primary -> server -> paired game: enabled (uint32 0/1), threshold_percent (uint32 30/40/50), exactly 8 bytes.
    game_low_health_hint_set = 146,
    // Primary -> server -> game: enabled, direct, on_attack, on_attacked (0/1), range_bits (3..6m), health_percent (0..100).
    game_near_item1_set = 147,
    game_near_item1_roster = 148, game_near_item1_exclusions_set = 149,
    // Multi-slot near-item messages prefix their existing payload with uint32 item_slot (1..6).
    game_near_item_set = 150, game_near_item_roster = 151, game_near_item_exclusions_set = 152,
    // Automatic healing: enabled (0/1), target health_percent (10..100), exactly 8 bytes.
    game_auto_heal_set = 153, game_auto_heal_roster = 154, game_auto_heal_exclusions_set = 155,
    // Ally item cleanse: enabled, min_count (0..6), use_dark_shield, use_dining_table; switches 0/1, exactly 16 bytes.
    game_auto_dark_shield_set = 156, game_auto_dark_shield_roster = 157, game_auto_dark_shield_exclusions_set = 158,
    // Protect an attacked ally: enabled (0/1), min_count (0..6); roster/exclusions prefix side (0 ally, 1 enemy).
    game_auto_guisuo_set = 159, game_auto_guisuo_roster = 160, game_auto_guisuo_exclusions_set = 161,
    // Chain root with ground fire: enabled (0/1), range_bits (finite float 3..100 meters), exactly 8 bytes.
    game_auto_dihuo_set = 162, game_auto_dihuo_roster = 163, game_auto_dihuo_exclusions_set = 164,
    // Selection-screen shout: enabled (0/1), length-prefixed UTF-8 text; empty text sends nothing.
    game_selection_shout_set = 165,
    // Automatic match acceptance: enabled (uint32 0/1), exactly 4 bytes.
    game_auto_accept_set = 166,
    // Automatic command seal: enabled (uint32 0/1), exactly 4 bytes.
    game_auto_lingzhou_set = 167,
    // Auto arena: flags (enabled/use_one/use_two/use_three), range_bits (finite float 3..6 meters), exactly 8 bytes.
    game_auto_douji_set = 168, game_auto_douji_roster = 169, game_auto_douji_exclusions_set = 170,
    // Primary -> server -> paired game: tower attack/sight ranges enabled (uint32 0/1), exactly 4 bytes.
    game_tower_range_set = 171,
    // Primary -> server -> paired game: projectile display enabled, enemy, ally (three uint32 0/1), exactly 12 bytes.
    game_projectile_display_set = 172,
    // Primary -> server -> paired game: auto dodge enabled, rectangles, circles (three uint32 0/1), exactly 12 bytes.
    game_auto_dodge_set = 173,
    // Primary -> server -> paired game: enemy trap timer enabled (uint32 0/1), exactly 4 bytes.
    game_trap_timer_set = 174,
    // Primary -> server: action, skin ID (uint32), target game connection ID (uint64).
    // Server -> paired game: action and skin ID only; each request executes once.
    game_skin_action = 175,
    // Primary -> server -> paired game: enemy basic attack range enabled (uint32 0/1), exactly 4 bytes.
    game_attack_range_set = 176,
    // Primary -> server -> paired game: other heroes' skin blocking enabled (uint32 0/1), exactly 4 bytes.
    game_skin_block_set = 177,
    // Primary -> server -> paired game: automatic AFK movement enabled (uint32 0/1), exactly 4 bytes.
    game_auto_afk_set = 178,
    // Primary -> server -> paired game: hero-function run delay in milliseconds (uint32 100-300), exactly 4 bytes.
    game_hero_delay_set = 179,
    // Primary -> server -> paired game: walk-attack minimum move distance in metres (float32 bits, 0-1), exactly 4 bytes.
    game_move_distance_set = 180,
    // Primary -> server -> paired game: hero-function keys as Windows virtual-key codes, exactly 12 bytes:
    // uint32 combo key (连招键), uint32 toggle key (切换键), uint32 lane key (推线键); each valid_game_hero_key,
    // all three different.
    game_hero_keys_set = 181
};
constexpr uint32_t game_hero_combo_key_default = 0x20;   // VK_SPACE
constexpr uint32_t game_hero_toggle_key_default = 0x05;  // VK_XBUTTON1, the mouse's lower side button
constexpr uint32_t game_hero_lane_key_default = 0x06;    // VK_XBUTTON2, the mouse's upper side button
// Mouse middle / side buttons (VK_MBUTTON..VK_XBUTTON2) or a keyboard key (0x08..0xFE). Left and right buttons,
// VK_CANCEL and the reserved 0x07 are not hero keys.
inline bool valid_game_hero_key(uint32_t key) {
    return (key >= 0x04 && key <= 0x06) || (key >= 0x08 && key <= 0xFE);
}
inline bool valid_game_hero_keys(uint32_t combo_key, uint32_t toggle_key, uint32_t lane_key) {
    return valid_game_hero_key(combo_key) && valid_game_hero_key(toggle_key) && valid_game_hero_key(lane_key) &&
        combo_key != toggle_key && combo_key != lane_key && toggle_key != lane_key;
}
constexpr float game_move_distance_min = 0.0f, game_move_distance_max = 1.0f,
    game_move_distance_default = 0.0f, game_move_distance_step = 0.1f;
inline uint32_t game_move_distance_to_bits(float value) {
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
inline float game_move_distance_from_bits(uint32_t bits) {
    float value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
inline bool valid_game_move_distance(float value) {
    return std::isfinite(value) && value >= game_move_distance_min && value <= game_move_distance_max;
}
inline bool valid_game_move_distance_bits(uint32_t bits) {
    return valid_game_move_distance(game_move_distance_from_bits(bits));
}
constexpr uint32_t game_hero_delay_min_ms = 100, game_hero_delay_max_ms = 300, game_hero_delay_default_ms = 100;
inline bool valid_game_hero_delay(uint32_t delay_ms) {
    return delay_ms >= game_hero_delay_min_ms && delay_ms <= game_hero_delay_max_ms;
}
enum class ExplorerStatus : uint32_t { downloading = 1, launched = 2, failed = 3, busy = 4, cancelled = 5 };
constexpr size_t explorer_message_limit = 384;
inline bool valid_explorer_status(uint32_t status) { return status >= 1 && status <= 5; }
constexpr uint32_t game_auto_guisuo_min_count_default = 0, game_auto_guisuo_min_count_max = 6;
constexpr uint32_t game_auto_guisuo_ally = 0, game_auto_guisuo_enemy = 1, game_auto_guisuo_side_count = 2;
inline bool valid_game_auto_guisuo_config(uint32_t enabled, uint32_t min_count) {
    return enabled <= 1 && min_count <= game_auto_guisuo_min_count_max;
}
inline bool valid_game_auto_guisuo_side(uint32_t side) { return side < game_auto_guisuo_side_count; }
constexpr uint32_t game_auto_dark_shield_min_count_default = 0, game_auto_dark_shield_min_count_max = 6;
inline bool valid_game_auto_dark_shield_config(uint32_t enabled, uint32_t min_count,
    uint32_t use_dark_shield = 1, uint32_t use_dining_table = 0) {
    return enabled <= 1 && min_count <= game_auto_dark_shield_min_count_max &&
        use_dark_shield <= 1 && use_dining_table <= 1;
}
constexpr uint32_t game_auto_heal_health_min = 10, game_auto_heal_health_max = 100,
    game_auto_heal_health_default = 80;
inline bool valid_game_auto_heal_config(uint32_t enabled, uint32_t health_percent) {
    return enabled <= 1 && health_percent >= game_auto_heal_health_min && health_percent <= game_auto_heal_health_max;
}
constexpr float game_auto_disarm_range_min = 1.0f, game_auto_disarm_range_max = 5.0f,
    game_auto_disarm_range_default = 5.0f, game_auto_disarm_range_step = 0.1f;
inline uint32_t game_auto_disarm_range_to_bits(float value) {
    static_assert(sizeof(float) == sizeof(uint32_t));
    uint32_t bits = 0; std::memcpy(&bits, &value, sizeof(bits)); return bits;
}
inline float game_auto_disarm_range_from_bits(uint32_t bits) {
    float value = 0; std::memcpy(&value, &bits, sizeof(value)); return value;
}
inline bool valid_game_auto_disarm_range(float value) {
    return std::isfinite(value) && value >= game_auto_disarm_range_min && value <= game_auto_disarm_range_max;
}
constexpr float game_auto_dihuo_range_min = 3.0f, game_auto_dihuo_range_max = 100.0f,
    game_auto_dihuo_range_default = 3.0f, game_auto_dihuo_range_step = 0.1f;
inline uint32_t game_auto_dihuo_range_to_bits(float value) { return game_auto_disarm_range_to_bits(value); }
inline float game_auto_dihuo_range_from_bits(uint32_t bits) { return game_auto_disarm_range_from_bits(bits); }
inline bool valid_game_auto_dihuo_range(float value) {
    return std::isfinite(value) && value >= game_auto_dihuo_range_min && value <= game_auto_dihuo_range_max;
}
inline bool valid_game_auto_dihuo_config(uint32_t enabled, uint32_t range_bits) {
    return enabled <= 1 && valid_game_auto_dihuo_range(game_auto_dihuo_range_from_bits(range_bits));
}
constexpr uint32_t game_auto_douji_enabled = 1, game_auto_douji_use_one = 2,
    game_auto_douji_use_two = 4, game_auto_douji_use_three = 8,
    game_auto_douji_flags_default = 14, game_auto_douji_flags_mask = 15;
constexpr float game_auto_douji_range_min = 3.0f, game_auto_douji_range_max = 6.0f,
    game_auto_douji_range_default = 3.0f, game_auto_douji_range_step = 0.1f;
inline uint32_t game_auto_douji_range_to_bits(float value) { return game_auto_disarm_range_to_bits(value); }
inline float game_auto_douji_range_from_bits(uint32_t bits) { return game_auto_disarm_range_from_bits(bits); }
inline bool valid_game_auto_douji_range(float value) {
    return std::isfinite(value) && value >= game_auto_douji_range_min && value <= game_auto_douji_range_max;
}
inline bool valid_game_auto_douji_config(uint32_t flags, uint32_t range_bits) {
    return flags <= game_auto_douji_flags_mask && valid_game_auto_douji_range(game_auto_douji_range_from_bits(range_bits));
}
constexpr float game_near_item1_range_min = 3.0f, game_near_item1_range_max = 6.0f,
    game_near_item1_range_default = 6.0f, game_near_item1_range_step = 0.1f;
constexpr uint32_t game_near_item1_health_default = 100;
constexpr uint32_t game_near_item_count = 6;
inline bool valid_game_near_item_slot(uint32_t item_slot) { return item_slot >= 1 && item_slot <= game_near_item_count; }
inline uint32_t game_near_item1_range_to_bits(float value) { return game_auto_disarm_range_to_bits(value); }
inline float game_near_item1_range_from_bits(uint32_t bits) { return game_auto_disarm_range_from_bits(bits); }
inline bool valid_game_near_item1_range(float value) {
    return std::isfinite(value) && value >= game_near_item1_range_min && value <= game_near_item1_range_max;
}
inline bool valid_game_near_item1_config(uint32_t enabled, uint32_t direct, uint32_t on_attack,
    uint32_t on_attacked, uint32_t range_bits, uint32_t health_percent) {
    return enabled <= 1 && direct <= 1 && on_attack <= 1 && on_attacked <= 1 && health_percent <= 100 &&
        valid_game_near_item1_range(game_near_item1_range_from_bits(range_bits));
}
enum class GameExtraAction : uint32_t { talent = 1, fate = 2, switch_account = 3 };
inline bool valid_game_extra_action(uint32_t action) {
    return action >= uint32_t(GameExtraAction::talent) && action <= uint32_t(GameExtraAction::switch_account);
}
enum class GameSkinAction : uint32_t { set = 0, previous = 1, next = 2 };
inline bool valid_game_skin_action(uint32_t action, uint32_t skin_id) {
    return action == uint32_t(GameSkinAction::set) ? skin_id <= 255 :
        (action == uint32_t(GameSkinAction::previous) || action == uint32_t(GameSkinAction::next)) && skin_id == 0;
}
constexpr size_t app_version_limit = 32;
constexpr size_t update_notes_limit = 16 * 1024;
inline bool valid_app_version(std::string_view text) {
    if (text.empty() || text.size() > app_version_limit) return false;
    unsigned parts = 1, digits = 0;
    for (const char c : text) {
        if (c >= '0' && c <= '9') { ++digits; continue; }
        if (c != '.' || !digits || ++parts > 4) return false;
        digits = 0;
    }
    return parts == 4 && digits != 0;
}
constexpr uint32_t hero_rank_limit = 4096, hero_rank_page_size = 10;
constexpr uint32_t online_rank_limit = 100, permission_rank_limit = 100;
// The account's complete special field has a separate limit from ranking rows.
constexpr size_t hero_permissions_text_limit = 4096;
constexpr uint32_t hero_permissions_id_limit = 2048,
    hero_permissions_chunk_limit = static_cast<uint32_t>((payload_size - 12) / 4);
constexpr uint32_t hero_permissions_scope_heroes = 1, hero_availability_id_limit = 4096;
constexpr uint32_t hero_permissions_scope_catalog = 2;
constexpr size_t hero_catalog_name_limit = 960;
enum class HeroPermissionsResult : uint32_t { ok = 0, rate_limited = 1, unavailable = 2 };
constexpr size_t hero_name_limit = 960, hero_query_limit = 96, hero_query_characters = 32;
constexpr size_t permission_account_limit = 64, permission_name_limit = 512, permission_text_limit = 256;
enum class HeroRankMode : uint32_t { arena = 1, brawl = 2, permissions = 3, online_time = 4 };
enum class HeroRankSort : uint32_t { uses = 1, win_rate = 2 };
enum class HeroRankResult : uint32_t { ok = 0, invalid = 1, rate_limited = 2, unavailable = 3 };
constexpr size_t profile_name_limit = 144, profile_name_characters = 36;
constexpr size_t profile_password_limit = 256, profile_password_characters = 64;
enum class AccountAction : uint32_t { rename = 1, password = 2, hide_account_id = 3 };
enum class AccountResult : uint32_t {
    ok = 0, invalid = 1, name_taken = 2, password_mismatch = 3,
    confirmation_mismatch = 4, rate_limited = 5, unavailable = 6
};
// Canonical chat image (original-size PNG or GIF), per transfer and per stored row.
constexpr size_t media_size_limit = 15 * 1024 * 1024, favorite_limit = 100;
enum class MediaResult : uint32_t {
    ok = 0, invalid = 1, too_large = 2, busy = 3, unavailable = 4,
    not_found = 5, forbidden = 6, not_friend = 7, limit = 8,
    // Upload refused by a server whose database still has the old 2 MiB limit
    // (media_original_migration.sql not run). Only images over 2 MiB, which old
    // clients never send, can get it.
    upgrade_pending = 9
};
// Deadline for one media upload (begin to commit) or download (begin to end) of
// `bytes`: 60 s plus 1 s per 64 KiB, so 15 MiB gets 300 s (about 0.45 Mbit/s).
constexpr uint32_t media_transfer_seconds(size_t bytes) {
    return 60 + static_cast<uint32_t>(bytes / (64 * 1024));
}
constexpr size_t avatar_size_limit = 384 * 1024, avatar_chunk_limit = payload_size - 8;
enum class AvatarResult : uint32_t { ok = 0, invalid = 1, too_large = 2, busy = 3, unavailable = 4, not_found = 5 };
constexpr size_t chat_text_limit = 480, chat_character_limit = 120, chat_history_limit = 100;
constexpr size_t friend_limit = 200, private_history_limit = 50;
constexpr size_t conversation_limit = 50, enhanced_friend_limit = friend_limit + conversation_limit;
constexpr uint32_t social_features = 127; // Bits: 1 enhanced social, 2 notes/ID visibility, 4 conversation clearing, 8 friend offline times, 16 private notices, 32 game connection state, 64 game team/player name.
constexpr uint32_t social_game_presence = 32;
constexpr uint32_t social_game_team = 64;
// Bit 128: private history lives on the client; the server keeps only undelivered messages.
constexpr uint32_t social_feature_local_history = 128;
constexpr uint32_t private_flag_recalled = 1, private_flag_image = 2;
// Bit 256 (only with 128): private images are store-and-forward. The server pushes each pending image
// right (117) and deletes it when the client acknowledges (118) that the image is in its data cache.
constexpr uint32_t social_feature_private_media = 256;
// Bit 512 (with the complete 511 feature set): accepted friends' persisted online seconds.
constexpr uint32_t social_feature_online_time = 512;
constexpr size_t game_team_name_limit = 21; // UTF-8 bytes, excluding the null terminator.
constexpr size_t game_note_account_name_limit = 144;
constexpr size_t game_note_uid_limit = 64;
constexpr size_t game_note_name_limit = 144;
constexpr size_t game_note_time_limit = 19;
constexpr size_t game_note_text_limit = 360;
constexpr size_t game_note_limit = 10000;
constexpr size_t game_note_page_size = 20;
constexpr size_t game_note_query_limit = 96, game_note_query_characters = 32;
enum class GameNoteAction : uint32_t { list = 1, update = 2, remove = 3, create = 4 };
enum class GameNoteResult : uint32_t {
    ok = 0, invalid = 1, not_found = 2, forbidden = 3, unavailable = 4
};
constexpr uint32_t game_stats_player_limit = 14, game_stats_mode_count = 3,
    game_stats_recent_limit = 8;
enum class GameStatsResult : uint32_t {
    ok = 0, invalid = 1, hidden = 2, unavailable = 3
};
inline bool valid_game_stats_result(uint32_t value) {
    return value <= static_cast<uint32_t>(GameStatsResult::unavailable);
}
// The game-side control and the primary slider both use the original 0..100
// range from the overlay. Keep the same bound at the server boundary so a
// malformed primary cannot queue a value the game endpoint will reject.
constexpr uint32_t game_sight_min = 0;
constexpr uint32_t game_sight_max = 100;
constexpr uint32_t game_sight_limit = game_sight_max;
inline bool valid_game_sight(uint32_t value) { return value <= game_sight_limit; }
// Runtime map classification reported by the game endpoint.  Zero means the
// map is not in the known dump catalog yet; values are stable wire enums.
enum class GameMode : uint32_t {
    unknown = 0, arena = 1, battlefield = 2, brawl = 3,
    cultivation = 4, hero_training = 5, dragon = 6
};
constexpr uint32_t game_mode_max = static_cast<uint32_t>(GameMode::dragon);
inline bool valid_game_mode(uint32_t value) { return value <= game_mode_max; }
constexpr uint32_t account_feature_preferences = 1, account_feature_online_time = 2;
constexpr uint32_t account_features = account_feature_preferences | account_feature_online_time;
enum class FriendAction : uint32_t { request = 1, accept = 2, reject = 3, remove = 4, cancel = 5 };
enum class FriendRelation : uint32_t { accepted = 1, incoming = 2, outgoing = 3, temporary = 4 };
enum class SocialResult : uint32_t {
    ok = 0, not_found = 1, self = 2, not_friend = 3, no_request = 4,
    already = 5, rate_limited = 6, unavailable = 7, invalid = 8, limit = 9,
    recall_not_found = 10, recall_forbidden = 11, recall_too_old = 12
};
enum class ChatResult : uint32_t {
    accepted = 0, invalid_text = 1, rate_limited = 2, recalled = 3,
    recall_not_found = 4, recall_forbidden = 5, recall_too_old = 6, recall_unavailable = 7
};
// Wire value 2 remains compatible with older builds; call it game in the UI.
enum class ClientKind : uint32_t { primary = 1, secondary = 2, game = secondary };
// Successful login is a PROFILE packet containing the Java-compatible proof.
// Account and password failures use 1 and 2. Error 0 is not defined.
enum class Error : uint32_t {
    account_not_found = 1, password_mismatch = 2,
    machine_mismatch = 3, expired = 4, banned = 5,
    unavailable = 100, protocol = 101, busy = 102, session_limit = 103, credentials_changed = 104,
    version_unavailable = 105
};
inline bool is_auth_error(Error error) {
    const auto code = static_cast<uint32_t>(error);
    return code >= 1 && code <= 5;
}
inline uint32_t read32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
inline void write32(uint8_t* p, uint32_t n) {
    p[0] = uint8_t(n >> 24); p[1] = uint8_t(n >> 16); p[2] = uint8_t(n >> 8); p[3] = uint8_t(n);
}
inline bool valid_text(std::string_view s) {
    for (size_t i = 0; i < s.size();) {
        const uint8_t c = uint8_t(s[i++]);
        if (c < 0x20 || c == 0x7f) return false;
        if (c < 0x80) continue;
        unsigned extra = 0; uint32_t code = 0, minimum = 0;
        if (c >= 0xc2 && c <= 0xdf) { extra = 1; code = c & 31; minimum = 0x80; }
        else if (c >= 0xe0 && c <= 0xef) { extra = 2; code = c & 15; minimum = 0x800; }
        else if (c >= 0xf0 && c <= 0xf4) { extra = 3; code = c & 7; minimum = 0x10000; }
        else return false;
        if (i + extra > s.size()) return false;
        while (extra--) { const uint8_t b = uint8_t(s[i++]); if ((b & 0xc0) != 0x80) return false; code = (code << 6) | (b & 63); }
        if (code < minimum || code > 0x10ffff || (code >= 0xd800 && code <= 0xdfff)) return false;
    }
    return true;
}
// Canonical content identifier: 64 lowercase hexadecimal digits (SHA-256).
inline bool valid_sha256_hex(std::string_view text) {
    return text.size() == 64 && text.find_first_not_of("0123456789abcdef") == std::string_view::npos;
}
// Client nonce: 32 hexadecimal digits, either case.
inline bool valid_nonce_hex(std::string_view text) {
    return text.size() == 32 && text.find_first_not_of("0123456789abcdefABCDEF") == std::string_view::npos;
}
inline bool valid_update_notes_content(std::string_view text) {
    if (text.size() > update_notes_limit) return false;
    size_t begin = 0;
    for (;;) {
        const size_t end = text.find_first_of("\r\n\t", begin);
        if (!valid_text(text.substr(begin, end == std::string_view::npos ? end : end - begin))) return false;
        if (end == std::string_view::npos) return true;
        begin = end + 1;
    }
}
constexpr size_t game_selection_shout_text_limit = 177;
inline bool valid_game_selection_shout_config(uint32_t enabled, std::string_view text) {
    return enabled <= 1 && text.size() <= game_selection_shout_text_limit && valid_text(text);
}
inline size_t text_characters(std::string_view text) {
    size_t count = 0;
    for (unsigned char byte : text) if ((byte & 0xc0) != 0x80) ++count;
    return count;
}
// Strips ASCII spaces only (search queries); other whitespace is rejected by valid_text.
inline std::string trim_spaces(std::string_view text) {
    const auto first = text.find_first_not_of(' ');
    if (first == std::string_view::npos) return {};
    return std::string(text.substr(first, text.find_last_not_of(' ') - first + 1));
}
// valid_text accepts the empty string, so these also admit optional (empty) fields.
inline bool valid_bounded_text(std::string_view text, size_t max_bytes, size_t max_characters) {
    return text.size() <= max_bytes && valid_text(text) && text_characters(text) <= max_characters;
}
inline bool valid_bounded_text(std::string_view text, size_t max_bytes) {
    return text.size() <= max_bytes && valid_text(text);
}
inline std::string normalize_game_note_query(std::string_view text) { return trim_spaces(text); }
inline bool valid_game_note_query(std::string_view text) {
    return valid_bounded_text(text, game_note_query_limit, game_note_query_characters);
}
inline bool valid_game_note_uid_filter(std::string_view text) {
    return valid_bounded_text(text, game_note_uid_limit);
}
// Optional trailing marker-account filter of game_notes_request/begin: sent only when non-empty,
// so unfiltered requests keep the original packet shape.
constexpr size_t game_note_account_id_limit = 64;
inline bool valid_game_note_account_filter(std::string_view text) {
    return valid_bounded_text(text, game_note_account_id_limit);
}
inline std::string normalize_hero_query(std::string_view text) { return trim_spaces(text); }
inline bool valid_hero_query(std::string_view text) {
    return valid_bounded_text(text, hero_query_limit, hero_query_characters);
}
inline bool valid_hero_request(uint32_t mode, uint32_t sort, uint32_t page, std::string_view query) {
    if (mode == uint32_t(HeroRankMode::online_time))
        return sort == 1 && page <= (online_rank_limit - 1) / hero_rank_page_size && valid_hero_query(query);
    if (mode == uint32_t(HeroRankMode::permissions))
        return sort >= 1 && sort <= 2 && page <= (permission_rank_limit - 1) / hero_rank_page_size && valid_hero_query(query);
    return mode >= 1 && mode <= 2 && sort >= 1 && sort <= 2 &&
        page <= (hero_rank_limit - 1) / hero_rank_page_size && valid_hero_query(query);
}
inline bool valid_permission_list(std::string_view text) {
    if (text.empty() || text.size() > permission_text_limit || !valid_text(text)) return false;
    size_t begin = 0, count = 0;
    while (begin < text.size()) {
        while (begin < text.size() && text[begin] == ' ') ++begin;
        if (begin == text.size()) break;
        const size_t end = text.find(' ', begin);
        const size_t limit = end == std::string_view::npos ? text.size() : end;
        if (limit == begin || limit - begin > 10 || ++count > 64) return false;
        for (size_t index = begin; index < limit; ++index)
            if (text[index] < '0' || text[index] > '9') return false;
        begin = limit;
    }
    return count != 0;
}
inline uint32_t permission_count(std::string_view text) {
    if (!valid_permission_list(text)) return 0;
    uint32_t count = 0; bool in_token = false;
    for (const char value : text) {
        if (value == ' ') { in_token = false; continue; }
        if (!in_token) { ++count; in_token = true; }
    }
    return count;
}
inline bool valid_profile_name(std::string_view text) {
    if (text.empty() || text.size() > profile_name_limit || !valid_text(text) ||
        text_characters(text) > profile_name_characters) return false;
    // Reject invisible formatting and boundary whitespace without silently
    // changing the user's spelling. Existing stored names remain readable.
    for (size_t i = 0; i < text.size();) {
        const size_t start = i;
        uint32_t cp = static_cast<unsigned char>(text[i++]);
        unsigned extra = 0;
        if (cp >= 0xf0) { cp &= 7; extra = 3; }
        else if (cp >= 0xe0) { cp &= 15; extra = 2; }
        else if (cp >= 0xc0) { cp &= 31; extra = 1; }
        while (extra--) cp = (cp << 6) | (static_cast<unsigned char>(text[i++]) & 63);
        if ((cp >= 0x80 && cp <= 0x9f) || cp == 0xad || cp == 0x61c || cp == 0x180e ||
            (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x2028 && cp <= 0x202e) ||
            (cp >= 0x2060 && cp <= 0x206f) || cp == 0xfeff) return false;
        const bool space = cp == 0x20 || cp == 0xa0 || cp == 0x1680 ||
            (cp >= 0x2000 && cp <= 0x200a) || cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (space && (!start || i == text.size())) return false;
    }
    return true;
}
inline bool valid_friend_note(std::string_view text) {
    return text.empty() || valid_profile_name(text);
}
inline bool valid_profile_password(std::string_view text) {
    return !text.empty() && text.size() <= profile_password_limit && valid_text(text) &&
        text_characters(text) <= profile_password_characters;
}
inline bool valid_chat_text(std::string_view text) {
    if (text.empty() || text.size() > chat_text_limit || !valid_text(text) ||
        text_characters(text) > chat_character_limit) return false;
    // Reject empty-looking messages made only of ordinary/full-width spaces.
    for (size_t i = 0; i < text.size();) {
        if (text[i] == ' ') ++i;
        else if (text.substr(i, 3) == "\xe3\x80\x80") i += 3;
        else return true;
    }
    return false;
}
struct Packet {
    std::array<uint8_t, packet_size> bytes{};
    explicit Packet(Type type = Type::ping) { write32(bytes.data() + 4, uint32_t(type)); }
    uint32_t length() const { return read32(bytes.data()); }
    Type type() const { return Type(read32(bytes.data() + 4)); }
    bool valid() const { return length() <= payload_size && uint32_t(type()) >= 1 && uint32_t(type()) <= uint32_t(Type::game_hero_keys_set); }
    bool add_bytes(const uint8_t* data, size_t size) {
        const auto n = length();
        if (n > payload_size || size > payload_size - n || (!data && size)) return false;
        if (size) memcpy(bytes.data() + 8 + n, data, size);
        write32(bytes.data(), static_cast<uint32_t>(n + size)); return true;
    }
    bool add_string(std::string_view value, size_t limit, bool allow_empty = true) {
        const auto n = length();
        if ((!allow_empty && value.empty()) || value.size() > limit || n + value.size() + 2 > payload_size || !valid_text(value)) return false;
        auto* p = bytes.data() + 8 + n;
        p[0] = uint8_t(value.size() >> 8); p[1] = uint8_t(value.size());
        memcpy(p + 2, value.data(), value.size()); write32(bytes.data(), uint32_t(n + value.size() + 2));
        return true;
    }
    bool add_number(uint32_t value) {
        const auto n = length();
        if (n > payload_size - 4) return false;
        write32(bytes.data() + 8 + n, value); write32(bytes.data(), n + 4); return true;
    }
    bool add_u64(uint64_t value) {
        if (length() > payload_size - 8) return false;
        return add_number(uint32_t(value >> 32)) && add_number(uint32_t(value));
    }
    void set_number(uint32_t value) { write32(bytes.data(), 4); write32(bytes.data() + 8, value); }
    uint32_t number() const { return read32(bytes.data() + 8); }
    void set_game_status(uint32_t uid, uint32_t state, bool connected,
        uint64_t available_virtual_memory = 0, std::string_view player_name = {},
        uint32_t map_id = 0, uint32_t mode = 0, uint32_t team = 0) {
        write32(bytes.data(), 20); write32(bytes.data() + 8, uid);
        write32(bytes.data() + 12, state); write32(bytes.data() + 16, connected ? 1 : 0);
        write32(bytes.data() + 20, uint32_t(available_virtual_memory >> 32));
        write32(bytes.data() + 24, uint32_t(available_virtual_memory));
        add_string(player_name, profile_name_limit);
        add_number(map_id); add_number(mode); add_number(team);
    }
};
struct Reader {
    const Packet& packet; size_t offset = 0;
    bool string(std::string& value, size_t limit, bool allow_empty = false) {
        const auto n = packet.length();
        if (!packet.valid() || offset + 2 > n) return false;
        const auto* p = packet.bytes.data() + 8 + offset;
        size_t count = (size_t(p[0]) << 8) | p[1];
        if (count > limit || offset + 2 + count > n || (!allow_empty && !count)) return false;
        value.assign(reinterpret_cast<const char*>(p + 2), count); offset += count + 2;
        return valid_text(value);
    }
    bool done() const { return offset == packet.length(); }
    bool number(uint32_t& value) {
        if (!packet.valid() || offset + 4 > packet.length()) return false;
        value = read32(packet.bytes.data() + 8 + offset); offset += 4; return true;
    }
    bool u64(uint64_t& value) {
        uint32_t high = 0, low = 0;
        if (!number(high) || !number(low)) return false;
        value = (uint64_t(high) << 32) | low;
        return true;
    }
};

inline bool make_game_selection_shout_config(uint32_t enabled, std::string_view text, Packet& packet) {
    if (!valid_game_selection_shout_config(enabled, text)) return false;
    Packet result(Type::game_selection_shout_set);
    if (!result.add_number(enabled) || !result.add_string(text, game_selection_shout_text_limit)) return false;
    packet = std::move(result);
    return true;
}
inline bool read_game_selection_shout_config(const Packet& packet, uint32_t& enabled, std::string& text) {
    if (packet.type() != Type::game_selection_shout_set) return false;
    Reader reader{packet};
    uint32_t parsed_enabled = 0;
    std::string parsed_text;
    if (!reader.number(parsed_enabled) || !reader.string(parsed_text, game_selection_shout_text_limit, true) ||
        !reader.done() || !valid_game_selection_shout_config(parsed_enabled, parsed_text)) return false;
    enabled = parsed_enabled;
    text = std::move(parsed_text);
    return true;
}

// Per-match automatic-disarm roster. This state is never an INI setting.
constexpr uint32_t game_auto_disarm_player_limit = 14;
struct GameAutoDisarmPlayer { uint32_t uid = 0, hero_id = 0; };
struct GameAutoDisarmRoster {
    uint64_t match_id = 0;
    std::vector<GameAutoDisarmPlayer> players;
    std::vector<uint32_t> excluded_hero_ids;
};
struct GameAutoDisarmExclusions {
    uint64_t match_id = 0, request_id = 0;
    std::vector<uint32_t> hero_ids;
};
inline bool game_auto_disarm_roster_contains(const GameAutoDisarmRoster& roster, uint32_t hero_id) {
    for (const auto& player : roster.players) if (player.hero_id == hero_id) return true;
    return false;
}
inline bool valid_game_auto_disarm_hero_ids(const std::vector<uint32_t>& ids) {
    if (ids.size() > game_auto_disarm_player_limit) return false;
    for (size_t i = 0; i < ids.size(); ++i) {
        if (ids[i] > 0xffffu) return false;
        for (size_t j = 0; j < i; ++j) if (ids[j] == ids[i]) return false;
    }
    return true;
}
inline bool valid_game_auto_disarm_roster(const GameAutoDisarmRoster& roster) {
    if (!roster.match_id || roster.players.size() > game_auto_disarm_player_limit ||
        !valid_game_auto_disarm_hero_ids(roster.excluded_hero_ids)) return false;
    for (size_t i = 0; i < roster.players.size(); ++i) {
        const auto& player = roster.players[i];
        if (!player.uid || player.hero_id > 0xffffu) return false;
        for (size_t j = 0; j < i; ++j) if (roster.players[j].uid == player.uid) return false;
    }
    for (uint32_t hero_id : roster.excluded_hero_ids)
        if (!game_auto_disarm_roster_contains(roster, hero_id)) return false;
    return true;
}
inline bool valid_game_auto_disarm_exclusions(const GameAutoDisarmExclusions& exclusions) {
    // Request zero is the server's initial state; LA clicks use nonzero IDs.
    return exclusions.match_id && valid_game_auto_disarm_hero_ids(exclusions.hero_ids);
}
inline bool make_game_auto_disarm_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!valid_game_auto_disarm_roster(roster)) return false;
    packet = Packet(Type::game_auto_disarm_roster);
    if (!packet.add_u64(roster.match_id) || !packet.add_number(static_cast<uint32_t>(roster.players.size()))) return false;
    for (const auto& player : roster.players)
        if (!packet.add_number(player.uid) || !packet.add_number(player.hero_id)) return false;
    if (!packet.add_number(static_cast<uint32_t>(roster.excluded_hero_ids.size()))) return false;
    for (uint32_t hero_id : roster.excluded_hero_ids) if (!packet.add_number(hero_id)) return false;
    return true;
}
inline bool read_game_auto_disarm_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_disarm_roster) return false;
    Reader reader{packet}; GameAutoDisarmRoster value; uint32_t count = 0;
    if (!reader.u64(value.match_id) || !reader.number(count) || count > game_auto_disarm_player_limit) return false;
    value.players.resize(count);
    for (auto& player : value.players) if (!reader.number(player.uid) || !reader.number(player.hero_id)) return false;
    if (!reader.number(count) || count > game_auto_disarm_player_limit) return false;
    value.excluded_hero_ids.resize(count);
    for (auto& hero_id : value.excluded_hero_ids) if (!reader.number(hero_id)) return false;
    if (!reader.done() || !valid_game_auto_disarm_roster(value)) return false;
    roster = std::move(value); return true;
}
inline bool make_game_auto_disarm_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!valid_game_auto_disarm_exclusions(exclusions)) return false;
    packet = Packet(Type::game_auto_disarm_exclusions_set);
    if (!packet.add_u64(exclusions.match_id) || !packet.add_u64(exclusions.request_id) ||
        !packet.add_number(static_cast<uint32_t>(exclusions.hero_ids.size()))) return false;
    for (uint32_t hero_id : exclusions.hero_ids) if (!packet.add_number(hero_id)) return false;
    return true;
}
inline bool read_game_auto_disarm_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_disarm_exclusions_set) return false;
    Reader reader{packet}; GameAutoDisarmExclusions value; uint32_t count = 0;
    if (!reader.u64(value.match_id) || !reader.u64(value.request_id) || !reader.number(count) ||
        count > game_auto_disarm_player_limit) return false;
    value.hero_ids.resize(count);
    for (auto& hero_id : value.hero_ids) if (!reader.number(hero_id)) return false;
    if (!reader.done() || !valid_game_auto_disarm_exclusions(value)) return false;
    exclusions = std::move(value); return true;
}
inline bool make_game_auto_guisuo_roster(uint32_t side, const GameAutoDisarmRoster& roster, Packet& packet) {
    Packet payload;
    if (!valid_game_auto_guisuo_side(side) || !make_game_auto_disarm_roster(roster, payload)) return false;
    packet = Packet(Type::game_auto_guisuo_roster);
    return packet.add_number(side) && packet.add_bytes(payload.bytes.data() + 8, payload.length());
}
inline bool read_game_auto_guisuo_roster(const Packet& packet, uint32_t& side, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_guisuo_roster || !packet.valid() || packet.length() < 4) return false;
    Reader reader{packet}; uint32_t value = 0;
    if (!reader.number(value) || !valid_game_auto_guisuo_side(value)) return false;
    Packet payload(Type::game_auto_disarm_roster);
    if (!payload.add_bytes(packet.bytes.data() + 12, packet.length() - 4) || !read_game_auto_disarm_roster(payload, roster)) return false;
    side = value; return true;
}
inline bool make_game_auto_guisuo_exclusions(uint32_t side, const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    Packet payload;
    if (!valid_game_auto_guisuo_side(side) || !make_game_auto_disarm_exclusions(exclusions, payload)) return false;
    packet = Packet(Type::game_auto_guisuo_exclusions_set);
    return packet.add_number(side) && packet.add_bytes(payload.bytes.data() + 8, payload.length());
}
inline bool read_game_auto_guisuo_exclusions(const Packet& packet, uint32_t& side, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_guisuo_exclusions_set || !packet.valid() || packet.length() < 4) return false;
    Reader reader{packet}; uint32_t value = 0;
    if (!reader.number(value) || !valid_game_auto_guisuo_side(value)) return false;
    Packet payload(Type::game_auto_disarm_exclusions_set);
    if (!payload.add_bytes(packet.bytes.data() + 12, packet.length() - 4) || !read_game_auto_disarm_exclusions(payload, exclusions)) return false;
    side = value; return true;
}
inline bool make_game_auto_dark_shield_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!make_game_auto_disarm_roster(roster, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_dark_shield_roster));
    return true;
}
inline bool read_game_auto_dark_shield_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_dark_shield_roster) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_roster));
    return read_game_auto_disarm_roster(copy, roster);
}
inline bool make_game_auto_dark_shield_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!make_game_auto_disarm_exclusions(exclusions, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_dark_shield_exclusions_set));
    return true;
}
inline bool read_game_auto_dark_shield_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_dark_shield_exclusions_set) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_exclusions_set));
    return read_game_auto_disarm_exclusions(copy, exclusions);
}
inline bool make_game_auto_dihuo_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!make_game_auto_disarm_roster(roster, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_dihuo_roster));
    return true;
}
inline bool read_game_auto_dihuo_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_dihuo_roster) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_roster));
    return read_game_auto_disarm_roster(copy, roster);
}
inline bool make_game_auto_dihuo_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!make_game_auto_disarm_exclusions(exclusions, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_dihuo_exclusions_set));
    return true;
}
inline bool read_game_auto_dihuo_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_dihuo_exclusions_set) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_exclusions_set));
    return read_game_auto_disarm_exclusions(copy, exclusions);
}
inline bool make_game_auto_douji_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!make_game_auto_disarm_roster(roster, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_douji_roster));
    return true;
}
inline bool read_game_auto_douji_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_douji_roster) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_roster));
    return read_game_auto_disarm_roster(copy, roster);
}
inline bool make_game_auto_douji_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!make_game_auto_disarm_exclusions(exclusions, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_douji_exclusions_set));
    return true;
}
inline bool read_game_auto_douji_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_douji_exclusions_set) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_exclusions_set));
    return read_game_auto_disarm_exclusions(copy, exclusions);
}
inline bool make_game_auto_heal_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!make_game_auto_disarm_roster(roster, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_heal_roster));
    return true;
}
inline bool read_game_auto_heal_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_auto_heal_roster) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_roster));
    return read_game_auto_disarm_roster(copy, roster);
}
inline bool make_game_auto_heal_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!make_game_auto_disarm_exclusions(exclusions, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_auto_heal_exclusions_set));
    return true;
}
inline bool read_game_auto_heal_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_auto_heal_exclusions_set) return false;
    Packet copy = packet;
    write32(copy.bytes.data() + 4, uint32_t(Type::game_auto_disarm_exclusions_set));
    return read_game_auto_disarm_exclusions(copy, exclusions);
}
inline bool make_game_near_item1_roster(const GameAutoDisarmRoster& roster, Packet& packet) {
    if (!make_game_auto_disarm_roster(roster, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_near_item1_roster)); return true;
}
inline bool read_game_near_item1_roster(const Packet& packet, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_near_item1_roster) return false;
    auto value = packet; write32(value.bytes.data() + 4, uint32_t(Type::game_auto_disarm_roster));
    return read_game_auto_disarm_roster(value, roster);
}
inline bool make_game_near_item1_exclusions(const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    if (!make_game_auto_disarm_exclusions(exclusions, packet)) return false;
    write32(packet.bytes.data() + 4, uint32_t(Type::game_near_item1_exclusions_set)); return true;
}
inline bool read_game_near_item1_exclusions(const Packet& packet, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_near_item1_exclusions_set) return false;
    auto value = packet; write32(value.bytes.data() + 4, uint32_t(Type::game_auto_disarm_exclusions_set));
    return read_game_auto_disarm_exclusions(value, exclusions);
}
inline bool make_game_near_item_roster(uint32_t item_slot, const GameAutoDisarmRoster& roster, Packet& packet) {
    Packet payload;
    if (!valid_game_near_item_slot(item_slot) || !make_game_auto_disarm_roster(roster, payload)) return false;
    packet = Packet(Type::game_near_item_roster);
    return packet.add_number(item_slot) && packet.add_bytes(payload.bytes.data() + 8, payload.length());
}
inline bool read_game_near_item_roster(const Packet& packet, uint32_t& item_slot, GameAutoDisarmRoster& roster) {
    if (packet.type() != Type::game_near_item_roster || !packet.valid() || packet.length() < 4 ||
        !valid_game_near_item_slot(packet.number())) return false;
    Packet payload(Type::game_auto_disarm_roster);
    if (!payload.add_bytes(packet.bytes.data() + 12, packet.length() - 4) || !read_game_auto_disarm_roster(payload, roster)) return false;
    item_slot = packet.number(); return true;
}
inline bool make_game_near_item_exclusions(uint32_t item_slot, const GameAutoDisarmExclusions& exclusions, Packet& packet) {
    Packet payload;
    if (!valid_game_near_item_slot(item_slot) || !make_game_auto_disarm_exclusions(exclusions, payload)) return false;
    packet = Packet(Type::game_near_item_exclusions_set);
    return packet.add_number(item_slot) && packet.add_bytes(payload.bytes.data() + 8, payload.length());
}
inline bool read_game_near_item_exclusions(const Packet& packet, uint32_t& item_slot, GameAutoDisarmExclusions& exclusions) {
    if (packet.type() != Type::game_near_item_exclusions_set || !packet.valid() || packet.length() < 4 ||
        !valid_game_near_item_slot(packet.number())) return false;
    Packet payload(Type::game_auto_disarm_exclusions_set);
    if (!payload.add_bytes(packet.bytes.data() + 12, packet.length() - 4) || !read_game_auto_disarm_exclusions(payload, exclusions)) return false;
    item_slot = packet.number(); return true;
}
}


