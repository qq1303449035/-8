#pragma once

#include <string>
#include <vector>
#include "network/protocol.h"

namespace chh {
namespace server {

struct AccountProfile {
    std::string account_id;
    std::string name;
    std::string expiration;
};

// Only populated from user8 after exact account and password verification.
struct AuthAuditInfo {
    std::string account_name;
    std::string notes;
    bool credentials_verified = false;
    bool metadata_valid = true;
};

enum class AuthResult {
    ok,
    account_not_found,
    password_mismatch,
    machine_mismatch,
    expired,
    banned,
    update_required,
    unavailable
};

// Initialize once before accepting requests; this does not connect to MySQL.
bool initialize_database();
// Memory cap for all in-memory lobby images together, from CHH_LOBBY_IMAGE_MEMORY_MB
// (whole MiB, 1..4096, default 256). False with a Chinese reason when the value is invalid.
bool lobby_image_memory_limit(size_t& bytes, std::string* error = nullptr);
// Call after stopping request workers. Reinitialization is not supported.
void shutdown_database();
// Read the sole authoritative appdata.version row for each primary login.
// There is no cache. Missing, duplicate or malformed configuration fails closed.
bool latest_app_version(std::string& out, std::string* diagnostic = nullptr) noexcept;
// Optional public release notes from the same sole appdata row. Failures,
// missing fields and a different version quietly produce no content.
bool latest_update_content(const std::string& version, std::string& out) noexcept;
AuthResult authenticate_account(const std::string& id,
    const std::string& password, const std::string& machine,
    AccountProfile& out, AuthAuditInfo* audit = nullptr);

// Actor comes from the authenticated session. Profile writes preserve the
// existing user8 password representation and do not modify account identity.
wire::AccountResult rename_account(const std::string& actor, const std::string& name,
    std::string* diagnostic = nullptr) noexcept;
wire::AccountResult change_account_password(const std::string& actor,
    const std::string& current, const std::string& next,
    std::string* diagnostic = nullptr) noexcept;

// Missing preference rows use the public default. A failed read leaves hide_id
// true so callers do not disclose an ID while its preference is unavailable.
wire::AccountResult get_account_preferences(const std::string& actor, bool& hide_id,
    std::string* diagnostic = nullptr) noexcept;
wire::AccountResult set_hide_account_id(const std::string& actor, bool hide_id,
    std::string* diagnostic = nullptr) noexcept;
struct FriendNote { std::string peer_id, note; };
wire::SocialResult list_friend_notes(const std::string& actor,
    std::vector<FriendNote>& out, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult set_friend_note(const std::string& actor, const std::string& peer,
    const std::string& note, std::string* diagnostic = nullptr) noexcept;
struct NoticeRecord {
    std::string account_id, account_name, uid, game_name, notes, notes_time;
};
struct NoticePage {
    uint32_t page = 0, matched = 0;
    std::string query, uid, account;   // account: marker account filter, empty for all markers
    std::vector<NoticeRecord> records;
};
wire::GameNoteResult list_notices(uint32_t page, std::string query, std::string uid, std::string account,
    NoticePage& out, std::string* diagnostic = nullptr) noexcept;
wire::GameNoteResult create_notice(const std::string& actor, const std::string& uid,
    const std::string& game_name, const std::string& notes, std::string* diagnostic = nullptr) noexcept;
wire::GameNoteResult update_notice(const std::string& actor, const NoticeRecord& original,
    const std::string& replacement, std::string* diagnostic = nullptr) noexcept;
wire::GameNoteResult delete_notice(const std::string& actor, const NoticeRecord& original,
    std::string* diagnostic = nullptr) noexcept;

// Separate from authentication so the caller records the final login result.
// Audit failures do not throw and must not change the authentication result.
// Optional diagnostics contain Chinese stages and safe database error details.
bool record_login(const std::string& id, const std::string& password_value,
    const std::string& machine, const AuthAuditInfo& audit,
    AuthResult result, bool primary_client,
    std::string* diagnostic = nullptr) noexcept;
// A separate row for each reported 0x5BDE game login, independent of TLS login.
// accountID is the authenticated LA account; accountName is the player name,
// notes is the decimal UID, and Explanation is the fixed game-login-info label.
bool record_game_login(const std::string& account_id, uint32_t uid,
    const std::string& player_name, std::string* diagnostic = nullptr) noexcept;

struct FriendRecord {
    std::string id, name;
    wire::FriendRelation relation = wire::FriendRelation::accepted;
    // The peer's own privacy preference.  It is sent only as display
    // metadata; the server still keeps the real ID for authenticated actions.
    bool hide_account_id = false;
};
struct FriendOffline { std::string peer_id; uint64_t offline_time = 0; };
struct FriendOnlineTime { std::string peer_id; uint64_t online_seconds = 0; };
// Called only for the authenticated account's final online session leaving.
// Older or repeated timestamps succeed without replacing a newer value.
bool record_offline_time(const std::string& account, uint64_t unix_seconds,
    std::string* diagnostic = nullptr) noexcept;
// Only current accepted friends are returned; NULL offlineTime is unknown (0).
bool list_friend_offline_times(const std::string& actor, std::vector<FriendOffline>& out,
    std::string* diagnostic = nullptr) noexcept;
// Only current accepted friends are returned; NULL onlineSeconds is zero.
bool list_friend_online_times(const std::string& actor, std::vector<FriendOnlineTime>& out,
    std::string* diagnostic = nullptr) noexcept;
// Only the server's online-time worker writes cumulative checkpoints. Repeating
// a checkpoint is safe even if the previous database acknowledgement was lost.
bool load_online_seconds(const std::string& account, uint64_t& out,
    std::string* diagnostic = nullptr) noexcept;
bool save_online_seconds(const std::string& account, uint64_t total,
    std::string* diagnostic = nullptr) noexcept;
struct OnlineTimeRecord {
    std::string account_id, name, avatar_hash;
    bool hide_account_id = true;
    uint64_t online_seconds = 0;
};
bool load_online_time_records(std::vector<OnlineTimeRecord>& out,
    std::string* diagnostic = nullptr) noexcept;
struct HeroUsage {
    uint32_t hero_id = 0;
    std::string name;
    uint64_t arena_uses = 0, brawl_uses = 0;
    uint64_t arena_wins = 0, arena_losses = 0, brawl_wins = 0, brawl_losses = 0;
};
// Read one bounded snapshot; ranking, search and paging use the server cache.
// Failure leaves out empty. This API never writes herouse or changes its schema.
bool load_hero_usage(std::vector<HeroUsage>& out, std::string* diagnostic = nullptr) noexcept;
// Read this authenticated account's current special field, without a cache.
bool load_hero_permissions(const std::string& actor, std::vector<uint32_t>& out,
    std::string* diagnostic = nullptr) noexcept;
// Resolve herouse.permissionID against this account's exact user8.special IDs.
// NULL/negative permissions deny access; failure leaves out empty.
bool load_available_heroes(const std::string& actor, std::vector<uint32_t>& out,
    std::string* diagnostic = nullptr) noexcept;
struct HeroCatalogEntry {
    uint32_t id = 0;
    std::string name;
    bool available = false;
};
// All configured hero IDs and database names, including unavailable heroes.
bool load_hero_catalog(const std::string& actor, std::vector<HeroCatalogEntry>& out,
    std::string* diagnostic = nullptr) noexcept;
// Read the bounded permission list from user8.special for the permission rank.
// The value is stored as a space-separated list of numeric permission IDs.
struct PermissionRecord {
    std::string account_id, name, permissions, avatar_hash;
    bool hide_account_id = false;
};
bool load_permission_records(std::vector<PermissionRecord>& out,
    std::string* diagnostic = nullptr) noexcept;
struct PrivateMessage {
    uint64_t id = 0;
    uint32_t timestamp = 0;
    std::string sender_id, recipient_id, text;
    bool recalled = false;
};
struct PrivatePage {
    std::vector<PrivateMessage> messages;
    bool has_older = false;
    uint64_t latest_id = 0;
};
struct PrivateClear { std::string peer_id; uint64_t through = 0; };
struct PrivateNotice { uint64_t id = 0; std::string peer; };
struct PrivateNoticeBatch {
    std::vector<PrivateNotice> notices;
    uint64_t cursor = 0; // Includes scanned messages suppressed by this owner's clear watermark.
    bool has_more = false;
};
// Recipient-only reads; no schema changes, acknowledgement writes or subscriptions.
wire::SocialResult private_notice_baseline(const std::string& actor,
    uint64_t& cursor, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult private_notices_since(const std::string& actor, uint64_t after,
    PrivateNoticeBatch& out, std::string* diagnostic = nullptr) noexcept;
// Not for accepted friends. Raises the owner's watermark (still used by the
// legacy export), drops its contact entry, the peer's undelivered rows and the
// owner's pending image rights from that peer. released: image hashes whose
// references were removed (candidates for collection), at most 2000.
wire::SocialResult clear_private_conversation(const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic = nullptr, std::vector<std::string>* released = nullptr) noexcept;
wire::SocialResult get_private_clear_watermark(const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic = nullptr) noexcept;
// Byte-ordered peer cursor, at most 50 entries; missing migration reads as empty.
wire::SocialResult list_private_clears(const std::string& actor, const std::string& after_peer,
    std::vector<PrivateClear>& out, bool& has_more, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult list_friends(const std::string& actor,
    std::vector<FriendRecord>& out, std::string* diagnostic = nullptr) noexcept;
// Recent message participants without an accepted friendship, from the
// content-free private_contacts8 index; opening a conversation creates nothing
// and a clear removes the entry. Pending requests remain separate.
wire::SocialResult list_private_contacts(const std::string& actor,
    std::vector<FriendRecord>& out, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult change_friend(const std::string& actor, const std::string& peer,
    wire::FriendAction action, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult private_history(const std::string& actor, const std::string& peer,
    uint64_t before, PrivatePage& out, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult private_since(const std::string& actor, const std::string& peer,
    uint64_t after, std::vector<PrivateMessage>& out, std::string* diagnostic = nullptr) noexcept;
// Ids come from private_sequence8 and are never reused. A retried nonce returns
// the original id and time, also after the row was delivered and deleted.
wire::SocialResult send_private(const std::string& actor, const std::string& peer,
    const std::string& nonce, const std::string& text, uint64_t& message_id, uint32_t& timestamp,
    bool& inserted, std::string* diagnostic = nullptr, bool allow_new = true) noexcept;
// Recalls one own private message. There is no age limit. A still-stored row
// gets the placeholder body; in every case a durable event is queued for the
// peer, whose client checks the sender itself. timestamp is 0 when no row was
// stored any more. Recalling a stored image also drops its link and, while it
// is undelivered, the recipient's image right (released gets the hash).
wire::SocialResult recall_private(const std::string& actor, const std::string& peer,
    uint64_t message_id, uint32_t& timestamp, std::string* diagnostic = nullptr,
    std::vector<std::string>* released = nullptr) noexcept;
// Store-and-forward for local-history clients. flags use wire::private_flag_*;
// body is the text, the image hash, or empty when recalled.
struct PrivateDelivery {
    uint64_t id = 0;
    uint32_t timestamp = 0, flags = 0;
    std::string sender_id, nonce, body;
};
struct PrivateRecallEvent { uint64_t id = 0, message_id = 0; std::string sender_id; };
struct PrivateInbox {
    std::vector<PrivateDelivery> messages;
    std::vector<PrivateRecallEvent> recalls;
};
// Pending rows above the legacy boundary and queued recall events, each after
// its own cursor; limits are at most 20.
wire::SocialResult private_inbox(const std::string& actor, uint64_t message_after, size_t message_limit,
    uint64_t recall_after, size_t recall_limit, PrivateInbox& out, std::string* diagnostic = nullptr) noexcept;
// Deletes delivered rows (and their image links) or events up to through.
// released: hashes of the deleted image links (at most 200 are reported).
wire::SocialResult ack_private_delivery(const std::string& actor, uint64_t through,
    std::string* diagnostic = nullptr, std::vector<std::string>* released = nullptr) noexcept;
wire::SocialResult ack_private_recalls(const std::string& actor, uint64_t through,
    std::string* diagnostic = nullptr) noexcept;
// Removes send tombstones older than seven days, at most 10000 per call;
// more is set when the batch was full, so older ones may remain.
wire::SocialResult purge_private_nonces(bool& more, std::string* diagnostic = nullptr) noexcept;
struct PrivateExportEntry {
    uint64_t id = 0;
    uint32_t timestamp = 0, flags = 0;
    std::string sender_id, recipient_id, body;
};
struct PrivateExportPage {
    std::vector<PrivateExportEntry> entries;
    uint64_t last_id = 0;
    bool more = false;
};
// One-time export of the owner's legacy history, 50 rows per page, above the
// owner's clear watermarks. Empty once the owner has completed its export.
wire::SocialResult private_export_page(const std::string& actor, uint64_t after,
    PrivateExportPage& out, std::string* diagnostic = nullptr) noexcept;
wire::SocialResult complete_private_export(const std::string& actor, uint64_t through,
    std::string* diagnostic = nullptr) noexcept;
struct PrivatePurge { uint64_t sent_after = 0, received_after = 0; bool finished = false; };
// Deletes one bounded batch of the owner's legacy rows that both sides no
// longer need. Call again until finished; does nothing before completion.
// released: hashes of the image links deleted with them.
wire::SocialResult purge_private_legacy(const std::string& actor, PrivatePurge& cursor,
    std::string* diagnostic = nullptr, std::vector<std::string>* released = nullptr) noexcept;
// Pending private image rights (media_grants8) of the recipient `actor`, after
// the id cursor, at most 20 (feature 256, pushed as type 117).
struct PrivateMediaRight { uint64_t id = 0; std::string hash; };
wire::SocialResult private_media_rights(const std::string& actor, uint64_t after, size_t limit,
    std::vector<PrivateMediaRight>& out, std::string* diagnostic = nullptr) noexcept;
// Type 118: the image is in the client's data cache (or definitively gone), so
// every right of `actor` for these hashes (at most 50) is deleted.
// released: those of the hashes that had a right (the others change nothing).
wire::SocialResult acknowledge_private_media(const std::string& actor, const std::vector<std::string>& hashes,
    std::string* diagnostic = nullptr, std::vector<std::string>* released = nullptr) noexcept;

struct AvatarMetadata { std::string account_id, hash; };
wire::AvatarResult avatar_metadata(const std::vector<std::string>& accounts,
    std::vector<AvatarMetadata>& out, std::string* diagnostic = nullptr) noexcept;
wire::AvatarResult store_avatar(const std::string& actor, const std::vector<uint8_t>& png,
    const std::string& hash, std::string* diagnostic = nullptr) noexcept;
wire::AvatarResult load_avatar(const std::string& hash, std::vector<uint8_t>& png,
    std::string* diagnostic = nullptr) noexcept;

struct PrivateImage { uint64_t message_id = 0; std::string hash; };
wire::MediaResult list_media_favorites(const std::string& actor, std::vector<std::string>& hashes,
    std::string* diagnostic = nullptr) noexcept;
// bytes: the canonical image the caller holds (a staged upload or the lobby's
// memory copy). Holding it proves access, and a missing asset is stored with
// the favourite; without it a missing asset is not_found.
wire::MediaResult change_media_favorite(const std::string& actor, const std::string& hash,
    bool add, bool lobby_visible, std::string* diagnostic = nullptr,
    const std::vector<uint8_t>* bytes = nullptr) noexcept;
wire::MediaResult store_media(const std::string& actor, const std::string& hash,
    const std::vector<uint8_t>& bytes, std::string* diagnostic = nullptr) noexcept;
wire::MediaResult authorize_media(const std::string& actor, const std::string& hash,
    bool lobby_visible, std::string* diagnostic = nullptr) noexcept;
wire::MediaResult load_media(const std::string& actor, const std::string& hash,
    bool lobby_visible, std::vector<uint8_t>& bytes, std::string* diagnostic = nullptr) noexcept;
wire::MediaResult send_private_image(const std::string& actor, const std::string& peer,
    const std::string& hash, const std::string& nonce, bool lobby_visible, bool allow_new,
    uint64_t& message_id, uint32_t& timestamp, bool& inserted, std::string* diagnostic = nullptr) noexcept;
wire::MediaResult private_images(const std::string& actor, const std::string& peer,
    const std::vector<uint64_t>& ids, std::vector<PrivateImage>& images,
    std::string* diagnostic = nullptr) noexcept;
// Collection of unreferenced chat images (private_media_migration.sql).
// An asset goes once no right, message link, favourite or upload younger than
// media_upload_grace_seconds references it.
constexpr uint32_t media_upload_grace_seconds = 3600;
enum class MediaCollect { deleted, gone, kept, young, busy, unavailable };
// young: wait_seconds until the youngest upload row leaves the grace period.
// busy: a lock wait timed out (1205) or NOWAIT refused (3572).
MediaCollect collect_media(const std::string& hash, uint32_t& wait_seconds,
    std::string* diagnostic = nullptr) noexcept;
struct MediaCollectionPage { std::vector<std::string> collectable; std::string last; size_t scanned = 0; };
// One page of media_assets8 in hash order after `after` (empty = from the
// start), without locks or image data; collectable lists the hashes that
// currently match the rule.
bool media_collection_page(const std::string& after, size_t limit, MediaCollectionPage& out,
    std::string* diagnostic = nullptr) noexcept;
enum class MediaCollection { enabled, paused, missing, unavailable };
// enabled once media_collection8.enabled_after <= NOW(); missing when the
// migration did not run.
MediaCollection media_collection_enabled(std::string* diagnostic = nullptr) noexcept;

} // namespace server
} // namespace chh
