// Run with run_account_online_time_tests.ps1; no sockets or database are used.
#include "../MySQL.h"
#include "../sessions.h"
#include "../chat_room.h"
#include "../social_diagnostics.h"
#include <chrono>
#include <iostream>
#include <stdexcept>

namespace {
unsigned reads = 0, checks = 0;
bool read_ok = true;
uint64_t stored_seconds = 0;
std::string last_account;
}
namespace chh::server {
bool load_online_seconds(const std::string& account, uint64_t& out, std::string*) noexcept {
    ++reads; last_account = account; out = stored_seconds;
    return read_ok;
}
}
namespace chh::tls {
struct Stream { std::vector<wire::Packet> sent; bool writable = true; };
}
namespace {
using Clock = std::chrono::steady_clock;
bool send_packet(chh::tls::Stream& stream, const chh::wire::Packet& packet) {
    if (!stream.writable) return false;
    stream.sent.push_back(packet);
    return true;
}
// Extracted from appServer.cpp by the launcher, never reimplemented here.
#include "../account_sender.inc"
#include "../social_state.inc"
#include "../friends_sender.inc"
#include "../social_join.inc"

void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
Clock::time_point at(int64_t seconds) {
    return Clock::time_point{} + std::chrono::seconds(seconds);
}
void cache_timing_and_failures() {
    AccountOnlineTime state;
    reads = 0; read_ok = true; stored_seconds = 0;
    require(state.refresh("authenticated-user", at(0)), "first join reads immediately");
    require(state.ready && state.seconds == 0 && reads == 1 && last_account == "authenticated-user",
        "zero persisted seconds is available for the authenticated account");
    stored_seconds = UINT64_MAX;
    require(!state.refresh("authenticated-user", at(59)) && reads == 1 && state.seconds == 0,
        "refresh before 60 seconds keeps cached result without reading");
    require(state.refresh("authenticated-user", at(60)) && reads == 2 && state.ready && state.seconds == UINT64_MAX,
        "60-second refresh preserves full unsigned duration");
    read_ok = false;
    require(state.refresh("authenticated-user", at(120)) && reads == 3 && !state.ready && state.seconds == 0,
        "read failure clears readiness and stale seconds");
    read_ok = true; stored_seconds = 90061;
    require(!state.refresh("authenticated-user", at(179)) && reads == 3 && !state.ready,
        "failed reads also respect the 60-second interval");
    require(state.refresh("authenticated-user", at(180)) && reads == 4 && state.ready && state.seconds == 90061,
        "later refresh recovers after database failure");
}
void sender_layouts() {
    const chh::server::AccountProfile profile{"self", "Login name", "2099-12-31 23:59:59"};
    chh::server::Sessions::Profile current;
    current.name = "Current name"; current.preferences_ready = true; current.hide_account_id = true;
    const unsigned reads_before = reads;
    for (const unsigned mode : {0u, 1u, 3u}) {
        for (const auto seconds : {uint64_t{0}, (uint64_t{1} << 53) + 1, UINT64_MAX}) {
            for (const bool available : {false, true}) {
                AccountOnlineTime time;
                time.ready = available; time.seconds = available ? seconds : 0;
                chh::tls::Stream stream;
                require(send_account_state(stream, profile, current, mode != 0, mode == 3 ? &time : nullptr),
                    "account sender succeeds");
                require(stream.sent.size() == 1 && stream.sent.front().type() == chh::wire::Type::account_state,
                    "account sender publishes exactly one state packet");
                chh::wire::Reader reader{stream.sent.front()};
                std::string id, name, expiration;
                require(reader.string(id, 64) && reader.string(name, 512) && reader.string(expiration, 19) &&
                    id == "self" && name == "Current name" && expiration == profile.expiration,
                    "account identity and expiry prefix stays unchanged");
                if (mode) {
                    uint32_t ready = 0, hidden = 0;
                    require(reader.number(ready) && reader.number(hidden) && ready == 1 && hidden == 1,
                        "legacy preferences remain before time extension");
                }
                if (mode == 3) {
                    uint32_t ready = 0; uint64_t value = 0;
                    require(reader.number(ready) && reader.u64(value) && ready == uint32_t(available) && value == time.seconds,
                        "opted-in account receives precise availability and uint64 seconds");
                }
                require(reader.done(), "legacy layouts have no extra fields and new layout has no trailing bytes");
            }
        }
    }
    require(reads == reads_before, "profile and repeated refresh replies reuse cache without database reads");
    chh::tls::Stream broken; broken.writable = false;
    require(!send_account_state(broken, profile, current, false, nullptr), "transport failure propagates");
    require(chh::wire::account_feature_preferences == 1 && chh::wire::account_feature_online_time == 2 &&
        chh::wire::account_features == 3, "capabilities use the agreed wire bits");
}
void social_join_capabilities() {
    for (const uint32_t capabilities : {0u, 1u, 3u, 7u, 15u, 31u, 127u, 255u, 511u, 1023u}) {
        chh::wire::Packet packet(chh::wire::Type::social_join);
        if (capabilities) require(packet.add_number(capabilities), "feature word fits");
        if (capabilities == 31 || capabilities == 127)
            require(packet.add_number(0) && packet.add_u64(0), "legacy notice cursor fits");
        uint32_t features = 0; bool has_cursor = true; uint64_t cursor = 99;
        require(parse_social_join(packet, features, has_cursor, cursor) && features == capabilities && !has_cursor && cursor == 0,
            "all supported legacy and new capabilities remain accepted");
    }
    for (const uint32_t capabilities : {512u, 639u, 767u, 1024u, 2047u}) {
        chh::wire::Packet packet(chh::wire::Type::social_join); packet.add_number(capabilities);
        uint32_t features = 0; bool has_cursor = false; uint64_t cursor = 0;
        require(!parse_social_join(packet, features, has_cursor, cursor), "partial or unknown feature combinations are rejected");
    }
    chh::wire::Packet packet(chh::wire::Type::social_join);
    packet.add_number(1023); packet.add_number(1); packet.add_u64(UINT64_MAX);
    uint32_t features = 0; bool has_cursor = false; uint64_t cursor = 0;
    require(parse_social_join(packet, features, has_cursor, cursor) && has_cursor && cursor == UINT64_MAX,
        "new capability keeps optional notice cursor format");
    packet.add_number(0);
    require(!parse_social_join(packet, features, has_cursor, cursor), "social join rejects trailing data");
}
void friend_time_layout_and_privacy() {
    using Relation = chh::wire::FriendRelation;
    const std::vector<chh::server::FriendRecord> friends{
        {"zero", "Zero", Relation::accepted, true}, {"huge", "Huge", Relation::accepted},
        {"missing", "Missing", Relation::accepted}, {"invite", "Invite", Relation::incoming},
        {"temporary", "Temporary", Relation::temporary}};
    SocialState social;
    social.enhanced = social.preferences = social.offline_enabled = social.game_presence = social.game_team = true;
    social.online_times = {{"zero", 0}, {"huge", UINT64_MAX}, {"invite", 123}, {"temporary", 456}};
    chh::server::Sessions::Presence presence;
    for (const bool time_enabled : {false, true}) {
        social.online_time_enabled = time_enabled;
        chh::tls::Stream stream;
        require(send_friends(stream, friends, presence, social), "friend state sender succeeds");
        require(stream.sent.size() == friends.size() + 2 && stream.sent.front().type() == chh::wire::Type::friends_begin &&
            stream.sent.front().number() == friends.size() && stream.sent.back().type() == chh::wire::Type::friends_end,
            "friend snapshot remains a complete begin/entries/end sequence");
        for (size_t index = 0; index < friends.size(); ++index) {
            chh::wire::Reader reader{stream.sent[index + 1]};
            std::string id, name, player, note;
            uint32_t relation = 0, is_online = 0, game = 0, connected = 0, team = 0, hidden = 0;
            uint64_t offline = 0;
            require(reader.string(id, 64) && reader.string(name, 512) && reader.number(relation) && reader.number(is_online) &&
                reader.number(game) && reader.number(connected) && reader.number(team) && reader.string(player, 144, true) &&
                reader.number(hidden) && reader.string(note, 144, true) && reader.u64(offline) && id == friends[index].id &&
                relation == uint32_t(friends[index].relation), "friend legacy prefix remains unchanged");
            if (time_enabled) {
                uint32_t ready = 99; uint64_t seconds = 99;
                require(reader.number(ready) && reader.u64(seconds), "new friend time tail is complete");
                const bool expected_ready = index < 2;
                const uint64_t expected_seconds = index == 1 ? UINT64_MAX : 0;
                require(ready == uint32_t(expected_ready) && seconds == expected_seconds,
                    "zero and uint64 durations work; missing, pending, and temporary contacts expose no time");
            }
            require(reader.done(), "old clients get no time tail; new clients get no trailing data");
        }
    }
}
}
int main() {
    try {
        cache_timing_and_failures(); sender_layouts(); social_join_capabilities(); friend_time_layout_and_privacy();
        std::cout << checks << " account online time checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAILED: " << error.what() << '\n'; return 1;
    }
}
