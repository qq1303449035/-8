// Only the isolated runner may invoke this test. It never reads default DB settings.
#include "../MySQL.cpp"
#include <iostream>
#include <stdexcept>

namespace {
using namespace chh::server;
unsigned checks = 0;
void require(bool value, const char* label) {
    ++checks;
    if (!value) throw std::runtime_error(label);
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
    return number > 1024 && number <= 65535 && number != 3306;
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
void run_tests() {
    Session session;
    auto* connection = session.get();
    sql(connection, "CREATE TABLE user8(accountID VARCHAR(64) NOT NULL,special TEXT NULL) ENGINE=InnoDB "
        "DEFAULT CHARSET=utf8mb4 COLLATE=utf8mb4_general_ci");
    sql(connection, "CREATE TABLE herouse(ID INT AUTO_INCREMENT PRIMARY KEY,heroID INT NULL,heroName VARCHAR(255) NULL,permissionID INT NULL DEFAULT NULL,"
        "jjcNum INT DEFAULT 0) ENGINE=InnoDB");
    sql(connection, "INSERT INTO user8 VALUES('Actor','43 46 2147483647'),('Other','53')");
    sql(connection, "INSERT INTO herouse(heroID,permissionID,jjcNum) VALUES"
        "(308,43,7),(69,46,8),(112,53,9),(1,0,10),(2,NULL,11),(3,-1,12),(4,-2147483648,13),"
        "(5,99,14),(65535,2147483647,15),(NULL,0,16)");
    std::vector<uint32_t> ids;
    std::string diagnostic;
    require(load_available_heroes("Actor", ids, &diagnostic) && ids == std::vector<uint32_t>({1,69,308,65535}),
        "free plus exact owned permissions sorted; NULL and negative denied");
    require(load_available_heroes("Other", ids, &diagnostic) && ids == std::vector<uint32_t>({1,112}), "per-account availability is independent");
    require(load_hero_permissions("Actor", ids, &diagnostic) && ids == std::vector<uint32_t>({43,46,2147483647}), "legacy query retains permission IDs");
    sql(connection, "UPDATE herouse SET heroName='自定义木更' WHERE heroID=308");
    sql(connection, "UPDATE herouse SET heroName='未授权小悟空' WHERE heroID=112");
    std::vector<HeroCatalogEntry> catalog;
    require(load_hero_catalog("Actor", catalog, &diagnostic) && catalog.size() == 9 && catalog[0].id == 1 && catalog[0].name == "英雄 #1",
        "catalog includes denied heroes and supplies fallback for NULL names");
    const auto find = [&](uint32_t id) -> const HeroCatalogEntry* {
        const auto row = std::find_if(catalog.begin(), catalog.end(), [id](const auto& item) { return item.id == id; });
        return row == catalog.end() ? nullptr : &*row;
    };
    require(find(308) && find(308)->name == "自定义木更" && find(308)->available &&
        find(112) && find(112)->name == "未授权小悟空" && !find(112)->available, "database names are authoritative regardless of permission");
    sql(connection, "ALTER TABLE herouse MODIFY heroName VARBINARY(2048) NULL");
    for (const char* expression : {"''", "'   '", "CONCAT('a',CHAR(0),'b')", "CONCAT('a',CHAR(9),'b')", "0xff", "REPEAT('a',256)", "REPEAT('a',1100)"}) {
        sql(connection, std::string("UPDATE herouse SET heroName=") + expression + " WHERE heroID=308");
        require(load_hero_catalog("Actor", catalog, &diagnostic) && find(308) && find(308)->name == "英雄 #308" && find(308)->available,
            "bad name falls back without changing authorization");
        require(load_available_heroes("Actor", ids, &diagnostic) && ids == std::vector<uint32_t>({1,69,308,65535}), "allowlist unaffected by malformed name");
    }
    sql(connection, "UPDATE herouse SET heroName=REPEAT(0xf09f9880,240) WHERE heroID=308");
    require(load_hero_catalog("Actor", catalog, &diagnostic) && find(308) && find(308)->name.size() == 960, "960 UTF8 bytes within 255 characters remain exact");
    require(scalar(connection, "SELECT SUM(jjcNum) FROM herouse") == "115", "queries preserve all statistics");
    require(!load_available_heroes("actor", ids, &diagnostic) && ids.empty(), "collation alias never impersonates exact actor");
    require(!load_available_heroes("Actor' OR 1=1 --", ids, &diagnostic) && ids.empty(), "actor payload cannot change parameterized query");
    require(!load_available_heroes("Missing", ids, &diagnostic) && ids.empty(), "missing account fails closed");
    sql(connection, "INSERT INTO user8 VALUES('Actor','53')");
    require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "duplicate account fails atomically");
    sql(connection, "DELETE FROM user8 WHERE special='53' AND BINARY accountID='Actor'");
    sql(connection, "INSERT INTO herouse(heroID,permissionID) VALUES(69,NULL)");
    require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "duplicate valid hero fails even when one permission is NULL");
    sql(connection, "DELETE FROM herouse WHERE heroID=69 AND permissionID IS NULL");
    sql(connection, "UPDATE user8 SET special='43junk' WHERE accountID='Actor'");
    require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "malformed grant field denies whole snapshot");
    sql(connection, "UPDATE user8 SET special='99 0' WHERE accountID='Actor'");
    require(load_available_heroes("Actor", ids, &diagnostic) && ids == std::vector<uint32_t>({1,5}), "zero and 99 are not wildcard grants");
    sql(connection, "UPDATE user8 SET special=NULL WHERE accountID='Actor'");
    require(load_available_heroes("Actor", ids, &diagnostic) && ids == std::vector<uint32_t>({1}), "empty grants still allow explicitly free hero");
    sql(connection, "ALTER TABLE herouse MODIFY permissionID VARCHAR(32) NULL");
    for (const char* invalid : {"43x", "2147483648", "-2147483649", "", "+43", "43.0"}) {
        sql(connection, std::string("UPDATE herouse SET permissionID='") + invalid + "' WHERE heroID=308");
        require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "malformed permission cannot produce partial availability");
    }
    sql(connection, "UPDATE herouse SET permissionID='43' WHERE heroID=308");
    for (const char* invalid : {"0", "-1", "65536"}) {
        sql(connection, std::string("INSERT INTO herouse(heroID,permissionID) VALUES(") + invalid + ",0)");
        require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "invalid hero number cannot publish partial snapshot");
        sql(connection, std::string("DELETE FROM herouse WHERE heroID=") + invalid);
    }
    sql(connection, "TRUNCATE TABLE herouse");
    require(load_hero_catalog("Actor", catalog, &diagnostic) && catalog.empty(), "empty table gives complete empty catalog");
    std::string insert = "INSERT INTO herouse(heroID,permissionID) VALUES";
    for (uint32_t hero = 1; hero <= 4096; ++hero) {
        if (hero > 1) insert += ',';
        insert += '(' + std::to_string(hero) + ",0)";
    }
    sql(connection, insert);
    require(load_available_heroes("Actor", ids, &diagnostic) && ids.size() == 4096 && ids.front() == 1 && ids.back() == 4096,
        "full 4096 row snapshot succeeds");
    require(load_hero_catalog("Actor", catalog, &diagnostic) && catalog.size() == 4096 && catalog.front().id == 1 && catalog.back().id == 4096,
        "full 4096 row named catalog succeeds");
    sql(connection, "INSERT INTO herouse(heroID,permissionID) VALUES(4097,0)");
    require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty(), "4097 raw rows fail without partial output");
    require(!load_hero_catalog("Actor", catalog, &diagnostic) && catalog.empty(), "oversized named catalog publishes no partial output");
    sql(connection, "ALTER TABLE herouse DROP COLUMN permissionID");
    require(!load_available_heroes("Actor", ids, &diagnostic) && ids.empty() && !diagnostic.empty(), "missing migration fails closed");
    require(load_hero_permissions("Actor", ids, &diagnostic) && ids.empty(), "legacy remains available without mapping schema");
}
}
int main() {
    if (!safe_environment()) { std::cerr << "REFUSED: isolated loopback test DB settings required.\n"; return 2; }
    if (!chh::server::initialize_database()) { std::cerr << "Database library unavailable.\n"; return 2; }
    int result = 0;
    try { run_tests(); std::cout << "PASS: " << checks << " isolated hero availability database checks\n"; }
    catch (const std::exception& error) { std::cerr << "FAIL after " << checks << ": " << error.what() << '\n'; result = 1; }
    chh::server::shutdown_database();
    return result;
}
