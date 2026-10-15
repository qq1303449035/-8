#pragma once
// Included inside chh::server after friends_store.h by MySQL.cpp.
namespace {
constexpr StoreSchema online_time_schema{"在线时长所需的user8表不存在",
    "user8缺少onlineSeconds字段，请核对在线时长部署说明", false, MYSQL_TYPE_STRING};
bool read_online_seconds(MYSQL* connection, const std::string& account, uint64_t& out,
    std::string* diagnostic) {
    std::vector<SocialRow> rows;
    if (!query_rows(online_time_schema, connection,
        "SELECT accountID,onlineSeconds FROM user8 WHERE accountID=? LIMIT 2",
        {account}, {64,20}, 2, rows, diagnostic, "读取累计在线时长")) return false;
    if (rows.size() != 1 || rows[0].nulls[0] || rows[0].values[0] != account ||
        rows[0].nulls[1] || !social_uint64(rows[0].values[1], out))
        return audit_failure(diagnostic, "读取累计在线时长", "账号不唯一、不存在或onlineSeconds不是非负整数");
    return true;
}
template<class Operation> bool online_time_database(std::string* diagnostic, Operation operation) noexcept {
    return store_session<SocialCode>(online_time_schema,
        {"初始化在线时长数据库", "初始化在线时长线程", "设置在线时长安全会话", "处理在线时长保存"},
        1, diagnostic, [&](MYSQL* connection) {
            return operation(connection) ? SocialCode::ok : SocialCode::unavailable;
        }) == SocialCode::ok;
}
}
bool load_online_seconds(const std::string& account, uint64_t& out, std::string* diagnostic) noexcept {
    out = 0;
    return online_time_database(diagnostic, [&](MYSQL* connection) {
        return social_id(account) && read_online_seconds(connection, account, out, diagnostic);
    });
}
bool save_online_seconds(const std::string& account, uint64_t total, std::string* diagnostic) noexcept {
    return online_time_database(diagnostic, [&](MYSQL* connection) {
        uint64_t current = 0;
        if (!social_id(account) || !read_online_seconds(connection, account, current, diagnostic)) return false;
        if (current >= total) return true;
        const auto encoded = std::to_string(total);
        // Derive the placeholder as text first: MySQL 8 otherwise infers a
        // signed integer for CAST(? AS UNSIGNED) and rejects values > INT64_MAX
        // before the unsigned cast. Both casts keep all 64 bits exact.
        // One atomic statement supports both InnoDB and legacy MyISAM tables.
        auto update = store_statement(online_time_schema, connection,
            "UPDATE user8 SET onlineSeconds=GREATEST(onlineSeconds,CAST(CAST(? AS CHAR) AS UNSIGNED)) "
            "WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) LIMIT 1",
            {encoded,account,account}, diagnostic, "保存累计在线时长");
        if (!update) return false;
        if (mysql_warning_count(connection) != 0 || mysql_stmt_affected_rows(update.get()) > 1)
            return audit_failure(diagnostic, "保存累计在线时长", "数据库返回警告或更新行数异常");
        if (mysql_stmt_affected_rows(update.get()) == 1) return true;
        // A zero-row update may mean an identical retry, a concurrent larger
        // value, or a deleted account. Only a confirmed checkpoint succeeds.
        update.reset();
        return read_online_seconds(connection, account, current, diagnostic) && current >= total;
    });
}
