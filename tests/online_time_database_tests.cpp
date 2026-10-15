// Standalone integration test; run via run_online_time_database_tests.ps1.
// Refuses all but explicitly configured disposable local test databases.
#include "../MySQL.cpp"
#include <atomic>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace {
using namespace chh::server;
unsigned passed = 0, failed = 0;
void require(bool value, const char* message) {
    if (!value) throw std::runtime_error(message);
}
void expect(const std::string& label, bool value) {
    std::cout << (value ? "PASS " : "FAIL ") << label << '\n';
    value ? ++passed : ++failed;
}
bool safe_environment() {
    std::string host, schema, port, user, password;
    const std::string missing = "<missing-test-setting>";
    if (!environment_or_default("CHH_DB_HOST", missing, host) ||
        !environment_or_default("CHH_DB_NAME", missing, schema) ||
        !environment_or_default("CHH_DB_PORT", missing, port) ||
        !environment_or_default("CHH_DB_USER", missing, user) ||
        !environment_or_default("CHH_DB_PASSWORD", missing, password) ||
        host != "127.0.0.1" || schema.find("chh8_test_") != 0 ||
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
std::string scalar(MYSQL* connection, const std::string& query) {
    sql(connection, query);
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(connection), mysql_free_result);
    require(bool(rows), "missing SQL result");
    auto row = mysql_fetch_row(rows.get());
    require(row && row[0], "missing scalar result");
    return std::string(row[0], mysql_fetch_lengths(rows.get())[0]);
}
void reset_user_table(const std::string& engine) {
    Session session;
    sql(session.get(), "DROP TABLE IF EXISTS user8");
    sql(session.get(), "CREATE TABLE user8(accountID VARCHAR(64) NOT NULL,accountName VARCHAR(144) NOT NULL,"
        "onlineSeconds BIGINT UNSIGNED NOT NULL DEFAULT 0) ENGINE=" + engine +
        " DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    sql(session.get(), "INSERT INTO user8 VALUES('Actor','Actor Name',123)");
}
void time_tests(const std::string& engine) {
    reset_user_table(engine);
    const auto label = [&](const char* text) { return engine + " " + text; };
    uint64_t loaded = 0;
    std::string diagnostic;
    expect(label("read persisted baseline"), load_online_seconds("Actor", loaded, &diagnostic) && loaded == 123);
    expect(label("save new checkpoint"), save_online_seconds("Actor", 456, &diagnostic));
    expect(label("duplicate checkpoint succeeds"), save_online_seconds("Actor", 456, &diagnostic));
    expect(label("older checkpoint cannot decrease total"), save_online_seconds("Actor", 100, &diagnostic) &&
        load_online_seconds("Actor", loaded, &diagnostic) && loaded == 456);
    for (uint64_t value : {(uint64_t{1} << 53) + 1, (uint64_t{1} << 63) + 1,
        (std::numeric_limits<uint64_t>::max)()}) {
        const bool saved = save_online_seconds("Actor", value, &diagnostic);
        if (!saved) std::cerr << "checkpoint diagnostic: " << diagnostic << '\n';
        expect(label("uint64 exact write/read ") + std::to_string(value),
            saved &&
            load_online_seconds("Actor", loaded, &diagnostic) && loaded == value);
        Session session;
        expect(label("SQL decimal exact ") + std::to_string(value),
            scalar(session.get(), "SELECT CAST(onlineSeconds AS CHAR) FROM user8") == std::to_string(value));
    }
    expect(label("UINT64_MAX retry safe"), save_online_seconds("Actor", (std::numeric_limits<uint64_t>::max)(), &diagnostic));
    expect(label("case-insensitive alias read rejected"), !load_online_seconds("actor", loaded, &diagnostic));
    expect(label("case-insensitive alias write rejected"), !save_online_seconds("actor", 1, &diagnostic));
    expect(label("invalid account rejected"), !save_online_seconds("", 1, &diagnostic));
    { Session session; sql(session.get(), "INSERT INTO user8 VALUES('Actor','Duplicate',0)"); }
    expect(label("duplicate account read rejected"), !load_online_seconds("Actor", loaded, &diagnostic));
    expect(label("duplicate account write rejected"), !save_online_seconds("Actor", 1, &diagnostic));
    { Session session; sql(session.get(), "DELETE FROM user8 WHERE accountName='Duplicate'");
      sql(session.get(), "INSERT INTO user8 VALUES('actor','Alias',0)"); }
    expect(label("collation-equivalent duplicates rejected"), !load_online_seconds("Actor", loaded, &diagnostic) &&
        !save_online_seconds("Actor", 1, &diagnostic));
    { Session session; sql(session.get(), "DELETE FROM user8"); }
    expect(label("deleted account read rejected"), !load_online_seconds("Actor", loaded, &diagnostic));
    expect(label("deleted account write rejected"), !save_online_seconds("Actor", 1, &diagnostic));
    { Session session; sql(session.get(), "INSERT INTO user8 VALUES('Actor','Actor Name',0)"); }
    std::atomic<unsigned> write_errors{0};
    std::vector<std::thread> workers;
    for (unsigned i = 1; i <= 8; ++i) workers.emplace_back([&, i] {
        if (!save_online_seconds("Actor", (uint64_t{1} << 63) + i)) ++write_errors;
    });
    for (auto& worker : workers) worker.join();
    expect(label("concurrent checkpoints preserve maximum"), write_errors == 0 &&
        load_online_seconds("Actor", loaded, &diagnostic) && loaded == (uint64_t{1} << 63) + 8);
    { Session session; sql(session.get(), "ALTER TABLE user8 DROP COLUMN onlineSeconds"); }
    expect(label("missing column read fails"), !load_online_seconds("Actor", loaded, &diagnostic) && !diagnostic.empty());
    expect(label("missing column write fails"), !save_online_seconds("Actor", 1, &diagnostic) && !diagnostic.empty());
    std::vector<OnlineTimeRecord> rows;
    expect(label("missing column ranking fails"), !load_online_time_records(rows, &diagnostic) && rows.empty());
}
void ranking_tests() {
    reset_user_table("InnoDB");
    { Session session;
      sql(session.get(), "DELETE FROM user8");
      sql(session.get(), "INSERT INTO user8 VALUES('a','Lower',100),('A','Upper',100),('zero','Zero',0),"
          "('maximum','Maximum',18446744073709551615),('large','Large',9223372036854775809)"); }
    std::vector<OnlineTimeRecord> rows;
    std::string diagnostic;
    expect("ranking available without optional tables", load_online_time_records(rows, &diagnostic) && rows.size() == 4);
    expect("ranking uint64 and binary ID order", rows.size() == 4 && rows[0].account_id == "maximum" &&
        rows[0].online_seconds == (std::numeric_limits<uint64_t>::max)() && rows[1].account_id == "large" &&
        rows[1].online_seconds == (uint64_t{1} << 63) + 1 && rows[2].account_id == "A" && rows[3].account_id == "a");
    bool hidden = rows.size() == 4;
    for (const auto& row : rows) hidden = hidden && row.hide_account_id && row.avatar_hash.empty();
    expect("missing privacy table hides all IDs", hidden);
    { Session session;
      sql(session.get(), "CREATE TABLE account_preferences8(owner_id VARBINARY(64) PRIMARY KEY,hide_id TINYINT UNSIGNED NOT NULL) ENGINE=InnoDB");
      sql(session.get(), "INSERT INTO account_preferences8 VALUES('a',1),('maximum',0)");
      sql(session.get(), "CREATE TABLE user_avatars8(accountID VARBINARY(64) PRIMARY KEY,hash BINARY(32) NOT NULL) ENGINE=InnoDB");
      sql(session.get(), "INSERT INTO user_avatars8 VALUES('maximum',UNHEX(REPEAT('ab',32)))"); }
    expect("ranking reads optional privacy and avatar metadata", load_online_time_records(rows, &diagnostic) && rows.size() == 4);
    expect("hidden preference respects exact account case", rows.size() == 4 && !rows[0].hide_account_id &&
        !rows[1].hide_account_id && !rows[2].hide_account_id && rows[3].hide_account_id);
    std::string expected_hash;
    for (unsigned i = 0; i < 32; ++i) expected_hash += "ab";
    expect("avatar hash attached to correct user", rows.size() == 4 && rows[0].avatar_hash == expected_hash && rows[1].avatar_hash.empty());
    { Session session;
      sql(session.get(), "DROP TABLE user_avatars8");
      sql(session.get(), "ALTER TABLE account_preferences8 RENAME COLUMN hide_id TO unavailable_hide_id"); }
    expect("optional broken privacy column permits ranking", load_online_time_records(rows, &diagnostic) && rows.size() == 4);
    hidden = rows.size() == 4;
    for (const auto& row : rows) hidden = hidden && row.hide_account_id;
    expect("broken privacy schema hides all IDs", hidden);
    { Session session;
      sql(session.get(), "DROP TABLE account_preferences8");
      sql(session.get(), "DELETE FROM user8");
      for (unsigned i = 0; i < 1005; ++i)
          sql(session.get(), "INSERT INTO user8 VALUES('user" + std::to_string(10000 + i) + "','Bulk'," + std::to_string(i + 1) + ")"); }
    expect("ranking capped at top 100 accounts", load_online_time_records(rows, &diagnostic) && rows.size() == 100);
    expect("ranking excludes 905 lowest accounts", rows.size() == 100 && rows.front().online_seconds == 1005 && rows.back().online_seconds == 906);
    bool descending = rows.size() == 100;
    for (size_t i = 1; i < rows.size(); ++i) descending = descending && rows[i - 1].online_seconds > rows[i].online_seconds;
    expect("ranking globally descending", descending);
    { Session session; sql(session.get(), "INSERT INTO user8 VALUES('user11004','Duplicate',1005)"); }
    expect("ranking duplicate exact account fails closed", !load_online_time_records(rows, &diagnostic) && rows.empty());
    { Session session; sql(session.get(), "DROP TABLE user8"); }
    uint64_t loaded = 42;
    expect("missing user table load resets output", !load_online_seconds("Actor", loaded, &diagnostic) && loaded == 0);
    expect("missing user table ranking fails", !load_online_time_records(rows, &diagnostic) && rows.empty());
}
}
int main() {
    if (!safe_environment()) {
        std::cerr << "REFUSED: explicit loopback test schema, nondefault port, user and empty test password required.\n";
        return 2;
    }
    if (!chh::server::initialize_database()) { std::cerr << "Database library unavailable.\n"; return 2; }
    try { time_tests("InnoDB"); time_tests("MyISAM"); ranking_tests(); }
    catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << error.what() << '\n'; }
    chh::server::shutdown_database();
    std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
    return failed == 0 ? 0 : 1;
}
