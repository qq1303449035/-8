// C++17 tests of ranking logic and the production packet sender.
// Run with run_online_ranking_tests.ps1; no sockets or database are used.
#include "../hero_ranking.h"
#include <iostream>
#include <stdexcept>

namespace chh::tls {
struct Stream { std::vector<wire::Packet> sent; };
}
namespace {
bool send_packet(chh::tls::Stream& stream, const chh::wire::Packet& packet) {
    stream.sent.push_back(packet);
    return true;
}
// Extracted from appServer.cpp by the test launcher, never reimplemented here.
#include "../rank_sender.inc"

using namespace chh::server;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
OnlineTimeRecord record(std::string id, std::string name, uint64_t seconds, bool hidden = false) {
    return {std::move(id), std::move(name), {}, hidden, seconds};
}
void ranking_and_privacy() {
    OnlineTimeRankSnapshot snapshot;
    snapshot.updated_at = 1234;
    snapshot.records = {record("second", "Runner", uint64_t(INT64_MAX) + 1),
        record("tie-z", "Last tie", 100), record("secret-731", "Hidden Name", UINT64_MAX, true),
        record("tie-a", "First tie", 100), record("visible-42", "Visitor", 50)};
    require(prepare_online_time_ranks(snapshot), "full uint64 online totals are accepted");
    auto page = online_time_rank_page(snapshot, 0, "");
    require(page.mode == 4 && page.sort == 1 && page.updated_at == 1234 && page.total == 5 && page.matched == 5,
        "page metadata identifies the online ranking snapshot");
    require(page.rows.size() == 5 && page.rows[0].online_seconds == UINT64_MAX &&
        page.rows[1].online_seconds == uint64_t(INT64_MAX) + 1, "totals sort without signed overflow");
    require(page.rows[2].account_id == "tie-a" && page.rows[3].account_id == "tie-z", "equal totals use ID order");
    require(page.rows[0].hide_account_id && page.rows[0].account_id.empty(), "hidden ID is absent from public row");
    require(online_time_rank_page(snapshot, 0, "secret-731").matched == 0, "hidden full ID cannot be searched");
    require(online_time_rank_page(snapshot, 0, "731").matched == 0, "hidden ID substring cannot be searched");
    page = online_time_rank_page(snapshot, 0, "  hIdDeN  ");
    require(page.query == "hIdDeN" && page.matched == 1 && page.rows[0].rank == 1 &&
        page.rows[0].account_id.empty(), "hidden account remains searchable by its displayed name");
    page = online_time_rank_page(snapshot, 0, "VISIBLE-42");
    require(page.matched == 1 && page.rows[0].rank == 5, "public ID search retains unfiltered rank");
}
void pagination_and_validation() {
    OnlineTimeRankSnapshot snapshot;
    for (uint32_t index = 0; index < 23; ++index)
        snapshot.records.push_back(record("account-" + std::to_string(index), "User " + std::to_string(index), 100 - index));
    require(prepare_online_time_ranks(snapshot), "valid page fixture prepares");
    auto page = online_time_rank_page(snapshot, 1, "");
    require(page.rows.size() == 10 && page.rows.front().rank == 11 && page.rows.back().rank == 20,
        "middle page contains ten consecutive global ranks");
    page = online_time_rank_page(snapshot, 9, "");
    require(page.page == 2 && page.rows.size() == 3 && page.rows.back().rank == 23, "excess page clamps to last page");
    page = online_time_rank_page(snapshot, 9, "missing");
    require(page.page == 0 && page.matched == 0 && page.rows.empty(), "empty search produces empty page zero");
    auto invalid = snapshot; invalid.records.push_back(invalid.records.front());
    require(!prepare_online_time_ranks(invalid), "duplicate account IDs reject snapshot");
    invalid = snapshot; invalid.records[0].online_seconds = 0;
    require(!prepare_online_time_ranks(invalid), "zero-time records reject snapshot");
    invalid = snapshot; invalid.records[0].name = "\xff";
    require(!prepare_online_time_ranks(invalid), "invalid UTF-8 names reject snapshot");
    invalid = snapshot; invalid.records[0].account_id = "\xff";
    require(!prepare_online_time_ranks(invalid), "invalid UTF-8 IDs reject snapshot");
    invalid = snapshot; invalid.records[0].avatar_hash = "bad-hash";
    require(!prepare_online_time_ranks(invalid), "invalid avatar hashes reject snapshot");
    snapshot.records.clear();
    for (uint32_t index = 0; index < chh::wire::online_rank_limit; ++index)
        snapshot.records.push_back(record(std::to_string(index), "User", index + 1));
    require(prepare_online_time_ranks(snapshot), "top 100 snapshot is accepted");
    page = online_time_rank_page(snapshot, 9, "");
    require(page.page == 9 && page.total == 100 && page.rows.size() == 10 && page.rows.front().rank == 91 &&
        page.rows.back().rank == 100, "top 100 tenth page remains available");
    snapshot.records.push_back(record("extra", "User", 1));
    require(!prepare_online_time_ranks(snapshot), "loader contract rejects a 101-row online snapshot");
}
void permission_top_100() {
    PermissionRankSnapshot snapshot; snapshot.updated_at = 1234;
    for (uint32_t index = 0; index < 102; ++index) {
        const auto suffix = std::to_string(1000 + index);
        snapshot.records.push_back({"account-" + suffix, "Name-" + suffix,
            index >= 100 ? "1 2 3" : "1", {}, false});
    }
    require(prepare_permission_ranks(snapshot), "102 permission candidates are accepted before truncation");
    require(snapshot.records.size() == 102 && snapshot.orders[0].size() == 100 && snapshot.orders[1].size() == 100,
        "keep validated candidates but bound each sorted permission list to 100");
    auto page = permission_rank_page(snapshot, 1, 0, "");
    require(page.total == 100 && page.matched == 100 && page.rows.size() == 10 &&
        page.rows[0].account_id == "account-1100" && page.rows[1].account_id == "account-1101" &&
        page.rows[2].account_id == "account-1000", "permission count sort ranks highest counts and stable ID ties first");
    page = permission_rank_page(snapshot, 1, 9, "");
    require(page.page == 9 && page.rows.size() == 10 && page.rows.front().rank == 91 && page.rows.back().rank == 100 &&
        page.rows.front().account_id == "account-1088" && page.rows.back().account_id == "account-1097",
        "permission count sort tenth page stops at its hundredth account");
    require(permission_rank_page(snapshot, 1, 0, "Name-1098").matched == 0 &&
        permission_rank_page(snapshot, 1, 0, "account-1099").matched == 0, "count-sort search cannot find excluded names or IDs");
    page = permission_rank_page(snapshot, 2, 0, "");
    require(page.total == 100 && page.matched == 100 && page.rows.front().account_id == "account-1000",
        "ID sort independently chooses its own first 100 accounts");
    page = permission_rank_page(snapshot, 2, 9, "");
    require(page.page == 9 && page.rows.size() == 10 && page.rows.front().rank == 91 && page.rows.back().rank == 100 &&
        page.rows.front().account_id == "account-1090" && page.rows.back().account_id == "account-1099",
        "permission ID sort tenth page stops at account rank 100");
    require(permission_rank_page(snapshot, 2, 0, "Name-1100").matched == 0 &&
        permission_rank_page(snapshot, 2, 0, "account-1101").matched == 0, "ID-sort search cannot find count-sort-only accounts");
    page = permission_rank_page(snapshot, 1, 9, "Name-1100");
    require(page.total == 100 && page.matched == 1 && page.page == 0 && page.rows.front().rank == 1,
        "search keeps total 100, original rank, and clamps filtered pages");
    snapshot.records[100].hide_account_id = true;
    require(prepare_permission_ranks(snapshot) && permission_rank_page(snapshot, 1, 0, "account-1100").matched == 0 &&
        permission_rank_page(snapshot, 1, 0, "Name-1100").matched == 1, "top-100 permission search retains hidden-ID privacy");
    auto invalid = snapshot; invalid.records.back().account_id = invalid.records.front().account_id;
    require(!prepare_permission_ranks(invalid), "duplicate IDs outside one top-100 list still fail candidate validation");
    invalid = snapshot; invalid.records.back().permissions = "invalid";
    require(!prepare_permission_ranks(invalid), "invalid candidates are not hidden by early truncation");
    snapshot.records.resize(3);
    require(prepare_permission_ranks(snapshot) && permission_rank_page(snapshot, 1, 9, "").total == 3 &&
        permission_rank_page(snapshot, 1, 9, "").page == 0, "small permission lists retain actual total and last page");
}
void request_validation() {
    using chh::wire::valid_hero_request;
    require(valid_hero_request(4, 1, 0, "") && valid_hero_request(4, 1, 9, "User"), "online mode accepts pages zero through nine");
    require(!valid_hero_request(4, 2, 0, "") && !valid_hero_request(4, 0, 0, ""), "online mode only accepts time ordering");
    require(!valid_hero_request(4, 1, 10, ""), "online mode rejects the eleventh page");
    require(!valid_hero_request(4, 1, 0, std::string(97, 'a')), "online mode retains bounded query validation");
    require(valid_hero_request(3, 1, 9, "") && valid_hero_request(3, 2, 9, ""), "both permission sorts accept the tenth page");
    require(!valid_hero_request(3, 1, 10, "") && !valid_hero_request(3, 2, 10, ""), "both permission sorts reject the eleventh page");
    require(!valid_hero_request(3, 0, 0, "") && !valid_hero_request(3, 3, 0, ""), "permission sort restrictions remain unchanged");
    require(valid_hero_request(1, 2, 409, "") && valid_hero_request(2, 2, 409, ""), "hero modes retain the 4096-record request limit");
    require(!valid_hero_request(5, 1, 0, ""), "unknown ranking modes are rejected");
}
void cache_refresh() {
    OnlineTimeRankingCache cache;
    const auto now = OnlineTimeRankingCache::Clock::now();
    unsigned loads = 0; bool available = true;
    auto load = [&](std::vector<OnlineTimeRecord>& rows, std::string*) {
        ++loads;
        rows = {record("test", "Test", 100)};
        return available;
    };
    const auto first = cache.get(now, 1000, load);
    require(first && first->updated_at == 1000 && loads == 1, "first request loads persisted totals");
    require(cache.get(now + std::chrono::seconds(29), 1029, load) == first && loads == 1,
        "requests share one immutable thirty-second snapshot");
    available = false;
    require(!cache.get(now + std::chrono::seconds(30), 1030, load) && loads == 2,
        "failed refresh returns unavailable without stale success");
    require(first->records[0].online_seconds == 100, "in-progress old page keeps immutable snapshot");
    available = true;
    require(!cache.get(now + std::chrono::seconds(34), 1034, load) && loads == 2, "failed loader retries after five seconds");
    require(cache.get(now + std::chrono::seconds(35), 1035, load) && loads == 3, "ranking recovers on next eligible request");
}
void page_envelope(const chh::tls::Stream& stream, uint32_t request, const HeroRankPage& page) {
    using namespace chh::wire;
    require(stream.sent.size() == page.rows.size() + 2, "sender emits one complete begin/entries/end sequence");
    for (const auto& packet : stream.sent) require(packet.valid(), "every captured packet is valid");
    require(stream.sent.front().type() == Type::hero_rank_begin && stream.sent.back().type() == Type::hero_rank_end,
        "page envelope retains packet types 67 and 69");
    Reader begin{stream.sent.front()};
    const std::array<uint32_t, 8> expected{request, page.mode, page.sort, page.page, hero_rank_page_size,
        page.total, page.matched, static_cast<uint32_t>(page.rows.size())};
    for (const auto number : expected) {
        uint32_t actual = 0;
        require(begin.number(actual) && actual == number, "begin retains all eight uint32 fields in order");
    }
    uint64_t updated_at = 0; std::string query;
    require(begin.u64(updated_at) && updated_at == page.updated_at && begin.string(query, hero_query_limit, true) &&
        query == page.query && begin.done(), "begin timestamp and query parse completely without trailing bytes");
    Reader end{stream.sent.back()}; uint32_t ended_request = 0;
    require(end.number(ended_request) && ended_request == request && end.done(), "end contains only the matching request ID");
}
void online_wire_format() {
    using namespace chh::wire;
    OnlineTimeRankSnapshot snapshot;
    snapshot.updated_at = UINT64_MAX;
    snapshot.records = {record("private-account", "隐藏用户", UINT64_MAX, true),
        record("public-account", "Public user", uint64_t(INT64_MAX) + 1),
        record("exact-integer", "Integer user", (uint64_t(1) << 53) + 1)};
    snapshot.records[0].avatar_hash = std::string(64, 'a');
    require(prepare_online_time_ranks(snapshot), "wire fixture prepares with large totals");
    auto page = online_time_rank_page(snapshot, 0, "");
    // Assert the sender also enforces privacy if a future caller supplies the ID.
    page.rows[0].account_id = "must-not-appear-on-wire";
    chh::tls::Stream stream;
    require(send_hero_rank_page(stream, 123, page), "production online page sender succeeds");
    page_envelope(stream, 123, page);
    for (size_t index = 0; index < page.rows.size(); ++index) {
        const auto& packet = stream.sent[index + 1]; const auto& row = page.rows[index];
        require(packet.type() == Type::hero_rank_entry, "online rows retain packet type 68");
        Reader entry{packet}; uint32_t request = 0, rank = 0, hidden = 0; uint64_t seconds = 0;
        std::string id, name, avatar;
        require(entry.number(request) && request == 123 && entry.number(rank) && rank == row.rank &&
            entry.string(id, permission_account_limit, true) && entry.string(name, permission_name_limit) &&
            entry.string(avatar, 64, true) && entry.number(hidden) && entry.u64(seconds) && entry.done(),
            "online entry parses request/rank/ID/name/avatar/hide/uint64 seconds with no trailing fields");
        require(id == (row.hide_account_id ? std::string{} : row.account_id) && name == row.name &&
            avatar == row.avatar_hash && hidden == (row.hide_account_id ? 1u : 0u), "wire preserves identity metadata and hides private ID");
        require(seconds == row.online_seconds, "wire preserves uint64 max, values over INT64_MAX, and values over 2^53 exactly");
    }
}
void legacy_wire_formats() {
    using namespace chh::wire;
    for (uint32_t mode = 1; mode <= 2; ++mode) {
        HeroRankPage page; page.mode = mode; page.sort = 2; page.total = 1; page.matched = 1;
        page.updated_at = 321; page.query = "Hero";
        HeroRankRow row; row.rank = 1; row.hero_id = 307; row.name = "Hero";
        row.uses = 99; row.wins = 66; row.losses = 33; row.online_seconds = UINT64_MAX;
        page.rows.push_back(row);
        chh::tls::Stream stream;
        require(send_hero_rank_page(stream, 201 + mode, page), "production legacy hero page sender succeeds");
        page_envelope(stream, 201 + mode, page);
        const auto& packet = stream.sent[1]; Reader entry{packet};
        uint32_t request = 0, rank = 0, hero = 0; uint64_t uses = 0, wins = 0, losses = 0; std::string name;
        require(packet.type() == Type::hero_rank_entry && entry.number(request) && request == 201 + mode &&
            entry.number(rank) && rank == 1 && entry.number(hero) && hero == 307 &&
            entry.string(name, hero_name_limit) && name == "Hero" && entry.u64(uses) && uses == 99 &&
            entry.u64(wins) && wins == 66 && entry.u64(losses) && losses == 33 && entry.done(),
            "arena/brawl keep legacy hero ID/name/uses/wins/losses layout without online seconds");
    }
    HeroRankPage page; page.mode = 3; page.sort = 1; page.total = 1; page.matched = 1; page.updated_at = 456;
    HeroRankRow row; row.rank = 7; row.account_id = "old-id"; row.name = "Permission user";
    row.permissions = "1 3 307"; row.avatar_hash = std::string(64, 'b'); row.hide_account_id = true;
    row.online_seconds = UINT64_MAX; page.rows.push_back(row);
    chh::tls::Stream stream;
    require(send_hero_rank_page(stream, 303, page), "production legacy permission page sender succeeds");
    page_envelope(stream, 303, page);
    const auto& packet = stream.sent[1]; Reader entry{packet}; uint32_t request = 0, rank = 0, hidden = 0;
    std::string id, name, permissions, avatar;
    require(packet.type() == Type::hero_rank_entry && entry.number(request) && request == 303 &&
        entry.number(rank) && rank == 7 && entry.string(id, permission_account_limit) && id == row.account_id &&
        entry.string(name, permission_name_limit) && name == row.name &&
        entry.string(permissions, permission_text_limit) && permissions == row.permissions &&
        entry.string(avatar, 64) && avatar == row.avatar_hash && entry.number(hidden) && hidden == 1 && entry.done(),
        "permission mode keeps legacy ID/name/permissions/avatar/hide layout without online seconds");
}
}
int main() {
    try {
        ranking_and_privacy(); pagination_and_validation(); permission_top_100(); request_validation(); cache_refresh();
        online_wire_format(); legacy_wire_formats();
        std::cout << "Online ranking tests passed: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "Online ranking test failed: " << error.what() << '\n';
        return 1;
    }
}
