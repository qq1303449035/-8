// Standalone C++17 integration test for separate 0x5BDE game-login audit rows.
// The authenticated LA account, numeric UID and raw name occupy separate fields.
// Requires an existing disposable loopback chh8_test_media_login_* schema,
// explicit non-3306 port/user, and an explicitly empty CHH_DB_PASSWORD.
// Creates userlogin8 if absent and clears only that table in the test schema.
#include "../MySQL.cpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace chh::server;
unsigned passed = 0, failed = 0;
void check(const char* label, bool good) {
    std::cout << (good ? "PASS " : "FAIL ") << label << '\n';
    good ? ++passed : ++failed;
}
void require(bool good, const char* label) { if (!good) throw std::runtime_error(label); }
bool safe_environment(std::string& schema) {
    const std::string missing = "<missing-test-setting>";
    std::string host, port, user, password;
    if (!environment_or_default("CHH_DB_HOST", missing, host) ||
        !environment_or_default("CHH_DB_NAME", missing, schema) ||
        !environment_or_default("CHH_DB_PORT", missing, port) ||
        !environment_or_default("CHH_DB_USER", missing, user) ||
        !environment_or_default("CHH_DB_PASSWORD", missing, password) ||
        host != "127.0.0.1" || schema.find("chh8_test_media_login_") != 0 ||
        schema.size() <= 22 || schema.size() > 64 || port.empty() || port.size() > 5 ||
        user.empty() || user == missing || !password.empty()) return false;
    for (char c : schema) if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
        (c >= '0' && c <= '9') || c == '_')) return false;
    unsigned number = 0;
    for (char c : port) { if (c < '0' || c > '9') return false; number = number * 10 + unsigned(c - '0'); }
    return number > 0 && number <= 65535 && number != 3306;
}
struct Session {
    LibraryCall library;
    ThreadContext thread;
    PooledConnection connection;
    Session() : connection(connect_database(1, nullptr)) {
        require(library.active() && thread.active() && bool(connection), "test connection unavailable");
    }
    MYSQL* get() { return connection.get(); }
};
void sql(MYSQL* connection, const std::string& query) {
    require(mysql_real_query(connection, query.data(), static_cast<unsigned long>(query.size())) == 0,
        "test SQL failed (details omitted)");
}
std::string scalar(MYSQL* connection, const std::string& query) {
    sql(connection, query);
    std::unique_ptr<MYSQL_RES, decltype(&mysql_free_result)> rows(mysql_store_result(connection), mysql_free_result);
    require(bool(rows), "test query returned no result");
    auto row = mysql_fetch_row(rows.get());
    require(row && row[0], "test scalar was absent or NULL");
    return std::string(row[0], mysql_fetch_lengths(rows.get())[0]);
}
std::string field(uint64_t id, const char* name) {
    Session s;
    return scalar(s.get(), std::string("SELECT ") + name + " FROM userlogin8 WHERE ID=" + std::to_string(id));
}
std::string fingerprint(uint64_t id) {
    return field(id, "CONCAT_WS('|',ID,HEX(accountID),HEX(password),HEX(machineCode),"
        "DATE_FORMAT(LoginTime,'%Y-%m-%d %H:%i:%s.%f'),HEX(accountName),HEX(notes),HEX(Explanation))");
}
uint64_t count() { Session s; return std::stoull(scalar(s.get(), "SELECT COUNT(*) FROM userlogin8")); }
uint64_t latest() { Session s; return std::stoull(scalar(s.get(), "SELECT COALESCE(MAX(ID),0) FROM userlogin8")); }
std::string schema_layout() {
    Session s;
    return scalar(s.get(), "SELECT GROUP_CONCAT(CONCAT_WS(':',COLUMN_NAME,COLUMN_TYPE,IS_NULLABLE,"
        "COALESCE(COLUMN_DEFAULT,'NULL'),EXTRA,COALESCE(COLLATION_NAME,'')) ORDER BY ORDINAL_POSITION) "
        "FROM information_schema.COLUMNS WHERE TABLE_SCHEMA=DATABASE() AND TABLE_NAME='userlogin8'");
}
uint64_t ordinary(const std::string& account = "AuditActor", bool primary = false,
    AuthResult result = AuthResult::ok, const std::string& notes = {}) {
    AuthAuditInfo audit;
    audit.credentials_verified = true; audit.account_name = "SyntheticAccount"; audit.notes = notes;
    const auto before = count();
    require(record_login(account, "synthetic-password", "synthetic-machine", audit, result, primary),
        "synthetic ordinary login insert failed");
    require(count() == before + 1, "ordinary login must insert one row");
    return latest();
}
uint64_t event(uint32_t uid, const std::string& name, const std::string& account = "AuditActor") {
    const auto before = count(), previous_id = latest();
    require(record_game_login(account, uid, name), "synthetic game-login event failed");
    require(count() == before + 1 && latest() > previous_id, "game-login event must insert one new row");
    return latest();
}
void prepare() {
    Session s;
    sql(s.get(), "CREATE TABLE IF NOT EXISTS userlogin8 (ID INT NOT NULL AUTO_INCREMENT PRIMARY KEY,"
        "accountID VARCHAR(255) NOT NULL,password VARCHAR(255) NOT NULL,machineCode VARCHAR(255) NOT NULL,"
        "LoginTime DATETIME(6) NOT NULL,accountName VARCHAR(255) NOT NULL,notes VARCHAR(255) NOT NULL,"
        "Explanation VARCHAR(255) NOT NULL) ENGINE=InnoDB DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    sql(s.get(), "DELETE FROM userlogin8");
}
void separate_event_tests() {
    const auto schema_before = schema_layout();
    const auto initial_event = event(42, "FirstPlayer");
    check("game event works without an ordinary login row", initial_event && count() == 1);
    const auto login = ordinary(), later_login = ordinary(), primary = ordinary("AuditActor", true);
    const auto existing_uid = ordinary("UID:42", false, AuthResult::ok, "existing note");
    const auto game_failed = ordinary("AuditActor", false, AuthResult::password_mismatch);
    const auto legacy_event = ordinary("UID:1234");
    {
        Session s;
        sql(s.get(), "UPDATE userlogin8 SET LoginTime='2024-01-02 03:04:05.123456' WHERE ID=" + std::to_string(login));
        sql(s.get(), "UPDATE userlogin8 SET password='',machineCode='',accountName='',notes='',Explanation='name:LegacyPlayer' WHERE ID=" + std::to_string(legacy_event));
    }
    const std::vector<uint64_t> protected_ids{initial_event, login, later_login, primary, existing_uid, game_failed, legacy_event};
    std::vector<std::string> before;
    for (const auto id : protected_ids) before.push_back(fingerprint(id));
    const auto initial_count = count();
    const auto first = event(42, "SamePlayer"), repeated = event(42, "SamePlayer"), changed = event(42, "OtherRole");
    const auto other_uid = event(43, "OtherUid"), other_account = event(42, "SamePlayer", "auditactor");
    check("repeated UID character changes and other accounts create separate rows", count() == initial_count + 5 &&
        first < repeated && repeated < changed && changed < other_uid && other_uid < other_account);
    check("accountID stores authenticated LA account", field(first, "accountID") == "AuditActor");
    check("notes stores decimal UID without a prefix", field(first, "notes") == "42");
    check("accountName stores raw player name", field(first, "accountName") == "SamePlayer");
    check("repeated name is retained in its own row", field(repeated, "accountID") == "AuditActor" &&
        field(repeated, "notes") == "42" && field(repeated, "accountName") == "SamePlayer");
    check("character change is retained in a later row", field(changed, "notes") == "42" && field(changed, "accountName") == "OtherRole");
    check("different UID has its own event", field(other_uid, "notes") == "43" && field(other_uid, "accountName") == "OtherUid");
    check("different LA accounts with the same UID remain separate and case-exact", field(other_account, "accountID") == "auditactor" &&
        field(other_account, "notes") == field(first, "notes") && field(first, "accountID") == "AuditActor");
    for (const auto id : {first, repeated, changed, other_uid, other_account}) {
        check("game event leaves password and machine empty", field(id,
            "OCTET_LENGTH(password)+OCTET_LENGTH(machineCode)") == "0");
        check("game event uses the fixed login-information explanation", field(id, "Explanation") == "游戏端:登入信息");
    }
    check("game event uses database current login time", field(other_uid, "TIMESTAMPDIFF(SECOND,LoginTime,CURRENT_TIMESTAMP) BETWEEN 0 AND 5") == "1");
    bool untouched = true;
    for (size_t i = 0; i < protected_ids.size(); ++i) untouched = untouched && fingerprint(protected_ids[i]) == before[i];
    check("events do not update any existing ordinary or game record", untouched);
    check("legacy UID/name-format event remains unchanged", field(legacy_event, "accountID") == "UID:1234" &&
        field(legacy_event, "Explanation") == "name:LegacyPlayer" && field(legacy_event, "notes").empty() && field(legacy_event, "accountName").empty());
    check("ordinary successful game login notes stay empty", field(login, "notes").empty() && field(later_login, "notes").empty());
    check("ordinary game login outcome remains unchanged", field(login, "Explanation") == "游戏端:登入成功");
    check("ordinary primary login keeps client prefix", field(primary, "Explanation") == "客户端:登入成功");
    check("events need no schema changes", schema_layout() == schema_before);
}
void input_tests() {
    const auto before_count = count();
    for (const auto& invalid : {std::string{}, std::string(145, 'x'), std::string("x\0y", 3),
        std::string("x\ny"), std::string("x\ty"), std::string("x\ry"), std::string(1, '\x7f'), std::string("\xc0\xaf", 2)})
        check("invalid game name rejected", !record_game_login("AuditActor", 123, invalid));
    for (const auto& invalid : {std::string{}, std::string(65, 'a'), std::string("Actor\0junk", 10)})
        check("invalid authenticated LA account rejected", !record_game_login(invalid, 123, "Name"));
    check("zero UID rejected", !record_game_login("AuditActor", 0, "Name"));
    check("invalid input inserts no partial row", count() == before_count);
    const std::string quoted = u8"茶会'O\\Neil%_😀";
    const std::string quoted_account = "La.Acct+MiXeD_%'42";
    const auto quote_id = event(1, quoted, quoted_account);
    check("quoted Unicode name remains literal without prefix", field(quote_id, "accountName") == quoted);
    check("authenticated account case and symbols are preserved", field(quote_id, "accountID") == quoted_account);
    check("minimum positive UID accepted", field(quote_id, "notes") == "1");
    const auto injection = event(2, "x'); DROP TABLE userlogin8; --");
    check("SQL syntax in name is parameter data", field(injection, "accountName") == "x'); DROP TABLE userlogin8; --");
    const std::string max_ascii(chh::wire::profile_name_limit, 'x');
    const auto ascii = event(UINT32_MAX, max_ascii);
    check("full unsigned 32-bit UID preserved in notes", field(ascii, "notes") == "4294967295");
    check("144-byte ASCII name stored without truncation", field(ascii, "accountName") == max_ascii);
    std::string max_unicode;
    for (unsigned i = 0; i < 36; ++i) max_unicode += u8"😀";
    const auto unicode = event(3000000000u, max_unicode, std::string(64, 'A'));
    check("144-byte UTF-8 name stored without truncation", max_unicode.size() == 144 && field(unicode, "accountName") == max_unicode);
    check("64-byte authenticated account accepted without truncation", field(unicode, "accountID") == std::string(64, 'A'));
    check("UID above signed integer range preserved", field(unicode, "notes") == "3000000000");
    check("player names do not replace fixed explanation", field(quote_id, "Explanation") == "游戏端:登入信息" &&
        field(injection, "Explanation") == "游戏端:登入信息" && field(ascii, "Explanation") == "游戏端:登入信息" && field(unicode, "Explanation") == "游戏端:登入信息");
}
void insert_failure_tests() {
    AuthAuditInfo audit;
    const auto before = count();
    check("unsupported ordinary login result rejected", !record_login("AuditActor", "synthetic-password", "machine", audit, AuthResult::unavailable, false));
    check("invalid ordinary login input rejected", !record_login(std::string(65, 'a'), "synthetic-password", "machine", audit, AuthResult::ok, false));
    check("invalid ordinary requests add no row", count() == before);
    Session s;
    sql(s.get(), "ALTER TABLE userlogin8 ADD CONSTRAINT test_reject_insert CHECK (accountID<>'RejectInsert' AND NOT (accountID='AuditActor' AND notes='31337'))");
    check("database constraint rejects game-login event", !record_game_login("AuditActor", 31337, "RejectedName"));
    check("failed event insert creates no partial row", count() == before);
    check("database constraint rejects ordinary login", !record_login("RejectInsert", "synthetic-password", "machine", audit, AuthResult::ok, false));
    check("failed ordinary insert creates no partial row", count() == before);
    sql(s.get(), "ALTER TABLE userlogin8 DROP CHECK test_reject_insert");
    const auto recovered = event(31337, "Recovered");
    check("game event recovers after database rejection", field(recovered, "accountID") == "AuditActor" && field(recovered, "notes") == "31337" &&
        field(recovered, "accountName") == "Recovered" && field(recovered, "Explanation") == "游戏端:登入信息");
}
void outcome_tests() {
    const std::pair<AuthResult, const char*> outcomes[] = {
        {AuthResult::ok, "登入成功"}, {AuthResult::account_not_found, "账号不存在"},
        {AuthResult::password_mismatch, "密码错误"}, {AuthResult::machine_mismatch, "机器码错误"},
        {AuthResult::expired, "茶会已过期"}, {AuthResult::banned, "茶会已封禁"}, {AuthResult::update_required, "版本不一致，请更新"}};
    for (const auto& outcome : outcomes) {
        const auto primary = ordinary("Outcomes", true, outcome.first), game = ordinary("Outcomes", false, outcome.first);
        check("ordinary primary outcome uses client prefix", field(primary, "Explanation") == std::string("客户端:") + outcome.second);
        check("ordinary secondary outcome uses game prefix", field(game, "Explanation") == std::string("游戏端:") + outcome.second);
    }
}
void permission_test(const std::string& schema) {
    const auto before = count();
    Session admin;
    const auto user = "chh_login_denied_" + std::to_string(GetCurrentProcessId());
    const auto account = "'" + user + "'@'127.0.0.1'";
    const auto create = "CREATE USER " + account + " IDENTIFIED BY ''";
    if (mysql_real_query(admin.get(), create.data(), static_cast<unsigned long>(create.size())) != 0) {
        std::cout << "SKIP insert-permission test: isolated test user cannot be created\n";
        return;
    }
    struct Cleanup {
        MYSQL* connection; std::string account, original_user;
        ~Cleanup() {
            if (!original_user.empty()) _putenv_s("CHH_DB_USER", original_user.c_str());
            const auto query = "DROP USER IF EXISTS " + account;
            mysql_real_query(connection, query.data(), static_cast<unsigned long>(query.size()));
        }
    } cleanup{admin.get(), account, {}};
    require(environment_or_default("CHH_DB_USER", "", cleanup.original_user) && !cleanup.original_user.empty(), "explicit test user missing");
    sql(admin.get(), "GRANT SELECT ON `" + schema + "`.userlogin8 TO " + account);
    require(_putenv_s("CHH_DB_USER", user.c_str()) == 0, "cannot switch synthetic test user");
    { Session denied; check("restricted account can read test table", scalar(denied.get(), "SELECT COUNT(*) FROM userlogin8") != "0"); }
    check("missing INSERT permission rejects game event", !record_game_login("AuditActor", 123, "Denied"));
    AuthAuditInfo audit;
    check("missing INSERT permission rejects ordinary login", !record_login("AuditActor", "synthetic-password", "machine", audit, AuthResult::ok, false));
    require(_putenv_s("CHH_DB_USER", cleanup.original_user.c_str()) == 0, "cannot restore test user");
    check("permission failure inserts no partial row", count() == before);
}
} // namespace

int main() {
    std::string schema;
    if (!safe_environment(schema)) {
        std::cerr << "REFUSED: explicit loopback chh8_test_media_login_* schema, nondefault port/user and empty password required.\n";
        return 2;
    }
    if (!initialize_database()) { std::cerr << "Database client initialization failed.\n"; return 2; }
    try {
        prepare();
        const auto initial_schema = schema_layout();
        separate_event_tests(); input_tests(); insert_failure_tests(); outcome_tests(); permission_test(schema);
        check("audit calls preserve existing columns and types", schema_layout() == initial_schema);
    } catch (const std::exception& error) { ++failed; std::cerr << "FAIL " << error.what() << '\n'; }
    shutdown_database();
    std::cout << "RESULT " << passed << " passed, " << failed << " failed\n";
    return failed ? 1 : 0;
}
