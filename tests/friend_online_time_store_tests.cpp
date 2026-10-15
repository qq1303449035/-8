// Exercises the production row validation with an in-memory database boundary.
// No MySQL library or live database is used by this test.
#include "../MySQL.h"
#include <iostream>
#include <stdexcept>
#include <string_view>
#include <utility>

namespace chh { namespace server {
namespace {
using SocialCode = wire::SocialResult;
struct MYSQL {};
struct StoreSchema {};
constexpr StoreSchema friend_online_time_schema{};
struct SocialRow { std::vector<std::string> values; std::vector<bool> nulls; };
std::vector<SocialRow> database_rows;
SocialCode owner_result = SocialCode::ok;
bool query_success = true;
unsigned owner_checks = 0, queries = 0;

bool audit_failure(std::string* diagnostic, const char*, const char* detail) {
    if (diagnostic) *diagnostic = detail;
    return false;
}
bool social_id(const std::string& id) { return !id.empty() && id.size() <= 64 && wire::valid_text(id); }
SocialCode private_account_exists(MYSQL*, const std::string&, std::string*) {
    ++owner_checks;
    return owner_result;
}
template<class Operation> SocialCode social_database(std::string*, Operation operation) noexcept {
    MYSQL connection;
    return operation(&connection);
}
bool query_rows(const StoreSchema&, MYSQL*, std::string_view query,
    const std::vector<std::string_view>& values, const std::vector<unsigned long>& limits,
    size_t row_limit, std::vector<SocialRow>& rows, std::string*, const char*) {
    ++queries;
    const std::vector<std::string_view> expected_values(4, "Actor");
    if (values != expected_values || limits != std::vector<unsigned long>({64,64,64,20}) ||
        row_limit != wire::friend_limit || query.find("f.state=2") == std::string_view::npos ||
        query.find("CAST(u.accountID AS BINARY)") == std::string_view::npos ||
        query.find("COALESCE(u.onlineSeconds,0)") == std::string_view::npos)
        return false;
    if (!query_success || database_rows.size() > row_limit) return false;
    rows = database_rows;
    return true;
}
#include "../friend_online_uint64.inc"
}
#include "../friend_online_store.inc"
} }

namespace {
using namespace chh::server;
unsigned passed = 0, failed = 0;
void expect(const char* label, bool value) {
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
    value ? ++passed : ++failed;
}
SocialRow row(std::string low, std::string high, std::string peer, std::string seconds, bool null = false) {
    return {{std::move(low),std::move(high),std::move(peer),std::move(seconds)}, {false,false,false,null}};
}
void reset(std::vector<SocialRow> rows = {}) {
    database_rows = std::move(rows); owner_result = SocialCode::ok;
    query_success = true; owner_checks = queries = 0;
}
}

int main() {
    using namespace chh::server;
    std::vector<FriendOnlineTime> output;
    std::string diagnostic;
    reset({row("Actor", "Zero", "Zero", "0"),
        row("Aardvark", "Actor", "Aardvark", "9007199254740993"),
        row("Actor", "Maximum", "Maximum", "18446744073709551615")});
    const bool loaded = list_friend_online_times("Actor", output, &diagnostic);
    expect("one owner check and one batch query", loaded && owner_checks == 1 && queries == 1);
    expect("zero and exact large unsigned totals", loaded && output.size() == 3 &&
        output[0].online_seconds == 0 && output[1].online_seconds == 9007199254740993ULL &&
        output[2].online_seconds == UINT64_MAX);
    expect("both accepted-pair positions select their peer", loaded && output.size() == 3 &&
        output[0].peer_id == "Zero" && output[1].peer_id == "Aardvark");
    reset();
    expect("no friends is a successful empty snapshot", list_friend_online_times("Actor", output) && output.empty());
    for (const auto result : {SocialCode::not_found, SocialCode::unavailable}) {
        reset(); owner_result = result; output = {{"Old",1}};
        expect("owner lookup failure stops before friend query", !list_friend_online_times("Actor", output) &&
            output.empty() && owner_checks == 1 && queries == 0);
    }
    reset(); output = {{"Old",1}};
    expect("invalid owner clears stale snapshot before querying", !list_friend_online_times("", output) &&
        output.empty() && owner_checks == 0 && queries == 0);
    reset(); query_success = false; output = {{"Old",1}};
    expect("query failure clears stale snapshot", !list_friend_online_times("Actor", output) && output.empty());
    for (const char* invalid : {"", "-1", "+1", "1.5", "18446744073709551616", "1x"}) {
        reset({row("Actor", "Good", "Good", "42"), row("Actor", "Peer", "Peer", invalid)});
        output = {{"Old",1}}; diagnostic.clear();
        expect("invalid decimal rejects whole snapshot", !list_friend_online_times("Actor", output, &diagnostic) &&
            output.empty() && !diagnostic.empty());
    }
    const std::vector<SocialRow> invalid_rows{
        row("Actor", "Peer", "Peer", "0", true),
        row("Else", "Peer", "Peer", "1"),
        row("Peer", "Actor", "Peer", "1"),
        row("Actor", "Actor", "Actor", "1"),
        row("Actor", "Peer", "peer", "1"),
        row("Actor", "Peer", "Other", "1"),
        row("Actor", "Peer", "", "1")};
    for (const auto& invalid : invalid_rows) {
        reset({row("Actor", "Good", "Good", "42"), invalid});
        expect("invalid peer relation or null total rejects whole snapshot",
            !list_friend_online_times("Actor", output) && output.empty());
    }
    reset({row("Actor", "Peer", "Peer", "1"), row("Actor", "Peer", "Peer", "2")});
    expect("duplicate exact peer rejects whole snapshot", !list_friend_online_times("Actor", output) && output.empty());
    std::vector<SocialRow> many;
    for (unsigned i = 0; i < chh::wire::friend_limit; ++i) {
        const auto peer = "Peer" + std::to_string(i);
        many.push_back(row("Actor", peer, peer, std::to_string(i)));
    }
    reset(many);
    expect("maximum accepted friend snapshot succeeds in one query", list_friend_online_times("Actor", output) &&
        output.size() == chh::wire::friend_limit && queries == 1);
    many.push_back(row("Actor", "Overflow", "Overflow", "0")); reset(many);
    expect("over-limit result clears complete snapshot", !list_friend_online_times("Actor", output) && output.empty());
    std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
