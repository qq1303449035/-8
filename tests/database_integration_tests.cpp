// Compile this file alone: it includes the database implementation for pool tests.
// Run only against a disposable, already-created local chh8_test_* database.
// Required: CHH_DB_HOST=127.0.0.1, explicit non-3306 CHH_DB_PORT,
// CHH_DB_NAME=chh8_test_..., CHH_DB_USER, and an explicitly empty CHH_DB_PASSWORD.
// This program creates/clears notice, user7 and pool_probe in that test database.
#include "../MySQL.cpp"
#include <atomic>
#include <cstdlib>
#include <iostream>
#include <set>
#include <stdexcept>
#include <thread>

namespace {
using namespace chh::server;
int passed = 0, failed = 0;
void expect(const char* label, bool value) {
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
    value ? ++passed : ++failed;
}
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
bool safe_environment() {
    std::string host, name, port, user, password;
    const std::string missing = "<missing-test-setting>";
    if (!environment_or_default("CHH_DB_HOST", missing, host) ||
        !environment_or_default("CHH_DB_NAME", missing, name) ||
        !environment_or_default("CHH_DB_PORT", missing, port) ||
        !environment_or_default("CHH_DB_USER", missing, user) ||
        !environment_or_default("CHH_DB_PASSWORD", missing, password) ||
        host != "127.0.0.1" || name.find("chh8_test_") != 0 ||
        port.empty() || user.empty() || user == missing || !password.empty()) return false;
    unsigned number = 0;
    for (const char digit : port) {
        if (digit < '0' || digit > '9' || number > 6553) return false;
        number = number * 10 + unsigned(digit - '0');
    }
    return number > 0 && number <= 65535 && number != 3306;
}
struct Session {
    LibraryCall library;
    ThreadContext thread;
    decltype(connect_database(1, nullptr)) connection;
    Session() : connection(connect_database(1, nullptr)) {
        require(library.active() && thread.active() && bool(connection), "test connection unavailable");
    }
    MYSQL* get() { return connection.get(); }
};
void sql(MYSQL* connection, const std::string& query) {
    if (mysql_real_query(connection, query.data(), static_cast<unsigned long>(query.size())) != 0)
        throw std::runtime_error("test SQL failed, errno=" + std::to_string(mysql_errno(connection)));
}
using Result = std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)>;
Result result(MYSQL* connection) {
    Result rows(mysql_store_result(connection), mysql_free_result);
    require(bool(rows), "missing test SQL result");
    return rows;
}
std::string scalar(MYSQL* connection, const std::string& query) {
    sql(connection, query);
    auto rows = result(connection);
    auto row = mysql_fetch_row(rows.get());
    require(row && row[0], "missing scalar result");
    return std::string(row[0], mysql_fetch_lengths(rows.get())[0]);
}
std::string literal(const std::string& value) {
    if (value.empty()) return "''";
    const char* digits = "0123456789abcdef";
    std::string hex = "CONVERT(0x";
    for (unsigned char c : value) { hex += digits[c >> 4]; hex += digits[c & 15]; }
    return hex + " USING utf8mb4)";
}
void prepare_tables() {
    Session session;
    sql(session.get(), "CREATE TABLE IF NOT EXISTS pool_probe(id INT PRIMARY KEY) ENGINE=InnoDB");
    sql(session.get(), "CREATE TABLE IF NOT EXISTS user7(accountID VARCHAR(64),`account` VARCHAR(144)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    sql(session.get(), "CREATE TABLE IF NOT EXISTS notice(AccountID VARCHAR(64) NOT NULL,"
        "UID VARCHAR(64) NOT NULL,GameName VARCHAR(144) NOT NULL,Notes VARCHAR(360) NOT NULL,"
        "NotesTime DATETIME(6) NOT NULL,INDEX note_order(NotesTime DESC,AccountID,UID,GameName)) "
        "ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    sql(session.get(), "ALTER TABLE notice MODIFY NotesTime DATETIME(6) NOT NULL");
    for (const char* table : {"pool_probe", "notice", "user7"}) sql(session.get(), std::string("DELETE FROM ") + table);
    sql(session.get(), "INSERT INTO user7 VALUES('Actor','MarkerName'),('Other','另一位玩家')");
}
void configuration_tests() {
    LibraryCall library;
    ThreadContext thread;
    std::string original_id;
    { auto lease = connect_database(1, nullptr); require(bool(lease), "configuration probe unavailable"); original_id = scalar(lease.get(), "SELECT CONNECTION_ID()"); }
    // Mutate only non-secret, nonempty settings; restoring an empty password
    // with _putenv_s would remove it and allow the production default.
    for (const char* key : {"CHH_DB_PORT", "CHH_DB_USER"}) {
        struct Restore {
            const char* key; std::string value;
            ~Restore() { _putenv_s(key, value.c_str()); }
        } restore{key, {}};
        require(environment_or_default(key, "", restore.value) && !restore.value.empty(), "missing explicit test setting");
        const auto wrong = std::string(key) == "CHH_DB_PORT" ? "invalid" : "chh_invalid_" + std::to_string(GetCurrentProcessId());
        require(_putenv_s(key, wrong.c_str()) == 0, "cannot set synthetic invalid configuration");
        { auto lease = connect_database(1, nullptr); expect("idle pool rejects changed invalid configuration", !lease); }
        require(_putenv_s(key, restore.value.c_str()) == 0, "cannot restore synthetic configuration");
        auto recovered = connect_database(1, nullptr);
        expect("restored configuration reuses its own idle connection", recovered && scalar(recovered.get(), "SELECT CONNECTION_ID()") == original_id);
    }
    std::vector<PooledConnection> held;
    std::vector<std::string> ids;
    for (unsigned timeout : {1u, 2u, 5u}) {
        auto lease = connect_database(timeout, nullptr);
        require(bool(lease), "timeout probe unavailable");
        bool options_match = true;
        for (auto option : {MYSQL_OPT_CONNECT_TIMEOUT, MYSQL_OPT_READ_TIMEOUT, MYSQL_OPT_WRITE_TIMEOUT}) {
            unsigned actual = 0;
            options_match = options_match && mysql_get_option(lease.get(), option, &actual) == 0 && actual == timeout;
        }
        expect("lease retains requested connect/read/write timeouts", options_match);
        ids.push_back(scalar(lease.get(), "SELECT CONNECTION_ID()"));
        held.push_back(std::move(lease));
    }
    held.clear();
    size_t index = 0;
    for (unsigned timeout : {1u, 2u, 5u}) {
        auto lease = connect_database(timeout, nullptr);
        expect("timeout-specific connection is reused", lease && scalar(lease.get(), "SELECT CONNECTION_ID()") == ids[index++]);
    }
}
void pool_tests() {
    std::string id, isolation, sql_mode, time_zone;
    { Session s; id = scalar(s.get(), "SELECT CONNECTION_ID()"); }
    { Session s; expect("repeated checkout reuses connection", scalar(s.get(), "SELECT CONNECTION_ID()") == id); }
    const auto lock = literal("chh_pool_reset_" + std::to_string(GetCurrentProcessId()));
    {
        Session s;
        isolation = scalar(s.get(), "SELECT @@session.transaction_isolation");
        sql_mode = scalar(s.get(), "SELECT @@session.sql_mode");
        time_zone = scalar(s.get(), "SELECT @@session.time_zone");
        sql(s.get(), "SET SESSION transaction_isolation='SERIALIZABLE',sql_mode='',time_zone='+03:00'");
        sql(s.get(), "SET @chh_pool_probe=123");
        expect("set named lock", scalar(s.get(), "SELECT GET_LOCK(" + lock + ",0)") == "1");
        expect("set last insert id", scalar(s.get(), "SELECT LAST_INSERT_ID(12345)") == "12345");
        require(mysql_autocommit(s.get(), false) == 0, "cannot begin rollback probe");
        sql(s.get(), "INSERT INTO pool_probe VALUES(1)");
    }
    {
        Session s;
        expect("reset preserves reusable connection id", scalar(s.get(), "SELECT CONNECTION_ID()") == id);
        expect("uncommitted write rolled back", scalar(s.get(), "SELECT COUNT(*) FROM pool_probe") == "0");
        expect("named lock released", scalar(s.get(), "SELECT IS_FREE_LOCK(" + lock + ")") == "1");
        expect("autocommit restored", scalar(s.get(), "SELECT @@autocommit") == "1");
        expect("isolation restored", scalar(s.get(), "SELECT @@session.transaction_isolation") == isolation);
        expect("SQL mode restored", scalar(s.get(), "SELECT @@session.sql_mode") == sql_mode);
        expect("time zone restored", scalar(s.get(), "SELECT @@session.time_zone") == time_zone);
        expect("user variables cleared", scalar(s.get(), "SELECT COALESCE(@chh_pool_probe,'unset')") == "unset");
        expect("last insert id cleared", scalar(s.get(), "SELECT LAST_INSERT_ID()") == "0");
        expect("UTF-8 connection restored", scalar(s.get(), "SELECT @@character_set_connection") == "utf8mb4");
    }
    {
        Session killer;
        std::string victim;
        {
            auto lease = connect_database(1, nullptr);
            require(bool(lease), "cannot borrow kill victim");
            victim = scalar(lease.get(), "SELECT CONNECTION_ID()");
        }
        sql(killer.get(), "KILL CONNECTION " + victim);
        auto recovered = connect_database(1, nullptr);
        require(bool(recovered), "cannot replace dead idle connection");
        expect("dead idle connection replaced", scalar(recovered.get(), "SELECT CONNECTION_ID()") != victim);
        expect("replacement connection works", scalar(recovered.get(), "SELECT 1") == "1");
    }
    std::mutex mutex;
    std::condition_variable cv;
    std::set<std::string> ids;
    unsigned ready = 0;
    bool release = false;
    std::atomic<unsigned> errors{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        try {
            Session s;
            const auto own_id = scalar(s.get(), "SELECT CONNECTION_ID()");
            std::unique_lock<std::mutex> guard(mutex);
            if (!ids.insert(own_id).second) ++errors;
            ++ready; cv.notify_all();
            cv.wait(guard, [&] { return release; });
            guard.unlock();
            if (scalar(s.get(), "SELECT CONNECTION_ID()") != own_id) ++errors;
        } catch (...) {
            ++errors;
            std::lock_guard<std::mutex> guard(mutex);
            ++ready; cv.notify_all();
        }
    });
    {
        std::unique_lock<std::mutex> guard(mutex);
        cv.wait(guard, [&] { return ready == 8; });
        release = true;
    }
    cv.notify_all();
    for (auto& worker : workers) worker.join();
    expect("concurrent leases are exclusive", errors == 0 && ids.size() == 8);
    { Session s; expect("connection reused across threads", ids.count(scalar(s.get(), "SELECT CONNECTION_ID()")) != 0); }
    {
        LibraryCall library;
        ThreadContext thread;
        std::vector<decltype(connect_database(1, nullptr))> held;
        std::set<std::string> full_ids;
        for (unsigned i = 0; i < 32; ++i) {
            auto lease = connect_database(1, nullptr);
            require(bool(lease), "pool cannot provide its 32 configured leases");
            full_ids.insert(scalar(lease.get(), "SELECT CONNECTION_ID()"));
            held.push_back(std::move(lease));
        }
        expect("all 32 held connections are distinct", full_ids.size() == 32);
        const auto start = std::chrono::steady_clock::now();
        auto overflow = connect_database(1, nullptr);
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        expect("pool capacity bounded at 32", !overflow);
        expect("saturated wait bounded by timeout", elapsed >= 800 && elapsed < 3500);
        overflow.reset();
        std::atomic<bool> acquired{false};
        std::thread waiter([&] {
            try { Session s; acquired = scalar(s.get(), "SELECT 1") == "1"; } catch (...) {}
        });
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        held.pop_back();
        waiter.join();
        expect("return wakes waiting checkout", acquired);
    }
}
std::vector<NoticeRecord> oracle() {
    Session s;
    sql(s.get(), "SELECT n.AccountID,COALESCE(u.`account`,''),n.UID,n.GameName,n.Notes,"
        "DATE_FORMAT(n.NotesTime,'%Y-%m-%d %H:%i:%s') FROM notice n "
        "LEFT JOIN user7 u ON u.accountID=n.AccountID "
        "ORDER BY n.NotesTime DESC,n.AccountID,n.UID,n.GameName LIMIT 10000");
    auto rows = result(s.get());
    std::vector<NoticeRecord> records;
    while (auto row = mysql_fetch_row(rows.get())) {
        auto lengths = mysql_fetch_lengths(rows.get());
        std::vector<std::string> fields;
        for (unsigned i = 0; i < 6; ++i) { require(row[i] != nullptr, "null oracle field"); fields.emplace_back(row[i], lengths[i]); }
        records.push_back({fields[0],fields[1],fields[2],fields[3],fields[4],fields[5]});
    }
    return records;
}
bool same(const NoticeRecord& a, const NoticeRecord& b) {
    return a.account_id == b.account_id && a.account_name == b.account_name && a.uid == b.uid &&
        a.game_name == b.game_name && a.notes == b.notes && a.notes_time == b.notes_time;
}
void compare_page(const char* label, uint32_t requested = 0, std::string query = {},
    std::string uid = {}, std::string account = {}) {
    const auto normalized = chh::wire::normalize_game_note_query(query);
    auto candidates = oracle();
    std::vector<NoticeRecord> matches;
    for (auto& row : candidates) if ((uid.empty() || row.uid == uid) &&
        (account.empty() || row.account_id == account) && (normalized.empty() ||
        row.game_name.find(normalized) != std::string::npos || row.account_name.find(normalized) != std::string::npos ||
        row.notes.find(normalized) != std::string::npos)) matches.push_back(std::move(row));
    const auto window = page_window(matches.size(), requested, chh::wire::game_note_page_size);
    NoticePage actual;
    const auto code = list_notices(requested, query, uid, account, actual);
    bool good = code == chh::wire::GameNoteResult::ok && actual.matched == matches.size() &&
        actual.page == window.page && actual.query == normalized && actual.uid == uid &&
        actual.account == account && actual.records.size() == window.end - window.first;
    for (size_t i = 0; good && i < actual.records.size(); ++i) good = same(actual.records[i], matches[window.first + i]);
    expect(label, good);
}
void insert_note(MYSQL* connection, unsigned second, const std::string& uid, const std::string& game,
    const std::string& notes, const std::string& account = "Actor") {
    sql(connection, "INSERT INTO notice VALUES(" + literal(account) + "," + literal(uid) + "," +
        literal(game) + "," + literal(notes) + ",TIMESTAMPADD(SECOND," + std::to_string(second) + ",'2025-01-01 00:00:00'))");
}
void pagination_tests() {
    compare_page("empty first page");
    compare_page("empty out-of-range page", UINT32_MAX);
    {
        Session s;
        for (unsigned i = 0; i < 20; ++i) insert_note(s.get(), 0, "UID" + std::to_string(100 + i), "Game", "body");
    }
    compare_page("exactly 20 records with same-second ordering");
    compare_page("20 records out-of-range clamp", 1);
    { Session s; insert_note(s.get(), 1, "UID120", "Game", "body"); }
    compare_page("21 records first page");
    compare_page("21 records last page", 1);
    compare_page("21 records far out-of-range clamp", UINT32_MAX);
    { Session s; sql(s.get(), "UPDATE notice SET NotesTime=TIMESTAMPADD(MICROSECOND,CAST(SUBSTRING(UID,4) AS UNSIGNED),'2025-01-01 00:00:00')"); }
    compare_page("fractional timestamps retain database ordering");
    compare_page("fractional timestamp last page", 1);
    {
        Session s;
        sql(s.get(), "DELETE FROM notice");
        insert_note(s.get(), 0, "UID", "CaseName", "plain");
        insert_note(s.get(), 1, "UID ", "casename", "plain", "Actor ");
        insert_note(s.get(), 2, "uid", "茶会名字", "中文笔记", "actor");
        insert_note(s.get(), 3, "percent", "literal", "%_\\", "Other");
        insert_note(s.get(), 4, "accent", "Café", "cafe");
        insert_note(s.get(), 5, "inject", "safe", "x' OR 1=1 --");
        insert_note(s.get(), 6, "missing", "no marker", "", "Missing");
    }
    for (const char* query : {"Case", "case", "茶会", "中文", "%", "_", "\\", "%_\\", "Café", "Cafe", "MarkerName", "另一位", "x' OR 1=1 --", "  Case  ", "   "})
        compare_page(query, 0, query);
    compare_page("UID exact case", 0, {}, "UID");
    compare_page("UID trailing space significant", 0, {}, "UID ");
    compare_page("account exact case", 0, {}, {}, "Actor");
    compare_page("account lowercase significant", 0, {}, {}, "actor");
    compare_page("account trailing space significant", 0, {}, {}, "Actor ");
    compare_page("combined filters", 0, "case", "UID ", "Actor ");
    compare_page("filter injection literal", 0, {}, "' OR 1=1 --", "Actor");
    compare_page("missing user7 marker row", 0, {}, {}, "Missing");
    { Session s; sql(s.get(), "INSERT INTO user7 VALUES('Actor','DuplicateMarker')"); }
    compare_page("duplicate join row remains searchable", 0, "DuplicateMarker");
    NoticePage duplicated;
    const auto joined_count = oracle().size();
    expect("duplicate joined rows count toward pagination", list_notices(0, {}, {}, {}, duplicated) == chh::wire::GameNoteResult::ok &&
        joined_count > 7 && duplicated.matched == joined_count && duplicated.records.size() == joined_count);
    { Session s; sql(s.get(), "DELETE FROM user7 WHERE `account`='DuplicateMarker'"); }
    NoticePage invalid;
    expect("oversized search rejected", list_notices(0, std::string(97, 'x'), {}, {}, invalid) == chh::wire::GameNoteResult::invalid);
    {
        Session s;
        sql(s.get(), "DELETE FROM notice");
        for (unsigned first = 0; first < 10001; first += 500) {
            std::string insert = "INSERT INTO notice VALUES";
            for (unsigned i = first; i < (std::min)(first + 500, 10001u); ++i) {
                if (i != first) insert += ',';
                insert += "('Actor','bulk-" + std::to_string(i) + "','Game'," +
                    literal(i == 0 ? "oldest-excluded" : i == 10000 ? "newest-included" : "bulk") +
                    ",TIMESTAMPADD(SECOND," + std::to_string(i) + ",'2025-01-01 00:00:00'))";
            }
            sql(s.get(), insert);
        }
    }
    compare_page("10001 source rows bounded to newest 10000");
    compare_page("10000-window last page", 499);
    compare_page("10000-window out-of-range clamp", 500);
    compare_page("oldest excluded before filtering", 0, "oldest-excluded");
    compare_page("newest included", 0, "newest-included");
}
template<class Predicate> bool eventually(Predicate predicate, unsigned milliseconds = 1500) {
    const auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(milliseconds);
    while (!predicate() && std::chrono::steady_clock::now() < deadline)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    return predicate();
}
// Must run last: no MySQL API or new ThreadContext is used after this returns.
void shutdown_tests(bool& stopped) {
    std::atomic<bool> waiter_started{false}, waiter_done{false}, waiter_rejected{false}, shutdown_done{false};
    std::thread waiter, stopper;
    try {
        LibraryCall library;
        ThreadContext thread;
        std::vector<PooledConnection> held;
        for (unsigned i = 0; i < 32; ++i) {
            auto lease = connect_database(1, nullptr);
            require(bool(lease), "cannot fill pool for shutdown probe");
            held.push_back(std::move(lease));
        }
        waiter = std::thread([&] {
            try {
                LibraryCall call;
                ThreadContext context;
                waiter_started = true;
                auto lease = connect_database(5, nullptr);
                waiter_rejected = !lease;
            } catch (...) { waiter_started = true; }
            waiter_done = true;
        });
        expect("shutdown waiter started", eventually([&] { return waiter_started.load(); }));
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
        expect("shutdown waiter blocked by held leases", !waiter_done);
        stopper = std::thread([&] { shutdown_database(); shutdown_done = true; });
        expect("shutdown entered stopping state", eventually([] {
            auto& state = library_state(); std::lock_guard<std::mutex> lock(state.mutex); return state.stopping;
        }));
        expect("shutdown waits for active lease owners", !shutdown_done);
        { LibraryCall rejected; expect("shutdown rejects new library calls", !rejected.active()); }
        expect("shutdown promptly wakes saturated waiter", eventually([&] { return waiter_done.load(); }) && waiter_rejected);
        waiter.join();
        held.clear();
        expect("shutdown still waits for library guard", !shutdown_done);
    } catch (...) {
        if (waiter.joinable()) waiter.join();
        if (stopper.joinable()) { stopper.join(); stopped = true; }
        throw;
    }
    stopper.join();
    stopped = true;
    expect("shutdown finishes after guards and leases exit", shutdown_done);
}
} // namespace

int main() {
    if (!safe_environment()) {
        std::cerr << "REFUSED: explicit loopback test schema, non-default port, user and empty test password required.\n";
        return 2;
    }
    if (!chh::server::initialize_database()) { std::cerr << "Database library unavailable.\n"; return 2; }
    bool stopped = false;
    try { prepare_tables(); configuration_tests(); pool_tests(); pagination_tests(); shutdown_tests(stopped); }
    catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << error.what() << '\n'; }
    if (!stopped) chh::server::shutdown_database();
    std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
