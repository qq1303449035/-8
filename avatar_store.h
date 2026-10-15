#pragma once
// Included inside chh::server by MySQL.cpp; binary values never use social_rows.
namespace {
using AvatarCode = wire::AvatarResult;
constexpr StoreSchema avatar_schema{"头像数据表不存在，请执行avatar_schema.sql部署步骤",
    "头像表字段不匹配，请核对avatar_schema.sql", true, MYSQL_TYPE_BLOB};
Statement avatar_statement(MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, std::string* diagnostic, const char* stage) {
    return store_statement(avatar_schema, connection, sql, values, diagnostic, stage);
}
template<class Operation> AvatarCode avatar_database(std::string* diagnostic, Operation operation) noexcept {
    return store_session<AvatarCode>(avatar_schema, {"初始化头像数据库", "初始化头像数据库线程",
        "设置头像数据库安全会话", "处理头像数据库请求"}, 1, diagnostic, std::move(operation));
}
// Also used by the permission ranking on its own connection. Only the hash is
// read: loading an avatar validates its size without reading every PNG here.
AvatarCode avatar_metadata_on(MYSQL* connection, const std::vector<std::string>& accounts,
    std::vector<AvatarMetadata>& out, std::string* diagnostic) {
    if (accounts.size() > wire::friend_limit) return AvatarCode::invalid;
    if (accounts.empty()) return AvatarCode::ok;
    std::vector<std::string_view> ids;
    std::vector<AvatarMetadata> candidate;
    std::string sql = "SELECT accountID,LOWER(HEX(hash)) FROM user_avatars8 WHERE accountID IN (";
    for (const auto& id : accounts) {
        if (!social_id(id)) return AvatarCode::invalid;
        bool duplicate = false;
        for (const auto& prior : ids) if (prior == id) duplicate = true;
        if (duplicate) continue;
        if (!ids.empty()) sql += ',';
        sql += '?'; ids.push_back(id); candidate.push_back({id,{}});
    }
    sql += ") LIMIT 201";
    auto statement = avatar_statement(connection, sql, ids, diagnostic, "查询头像标识");
    if (!statement) return AvatarCode::unavailable;
    std::vector<SocialRow> rows;
    if (!social_rows(statement.get(), {64,64}, wire::friend_limit, rows, diagnostic, "读取头像标识")) return AvatarCode::unavailable;
    for (const auto& row : rows) {
        bool found = false;
        if (row.nulls[0] || row.nulls[1] || !wire::valid_sha256_hex(row.values[1])) {
            audit_failure(diagnostic, "读取头像标识", "头像记录标识无效"); return AvatarCode::unavailable;
        }
        for (auto& entry : candidate) if (entry.account_id == row.values[0]) {
            if (!entry.hash.empty()) {
                audit_failure(diagnostic, "读取头像标识", "头像账号记录重复"); return AvatarCode::unavailable;
            }
            entry.hash = row.values[1]; found = true; break;
        }
        if (!found) { audit_failure(diagnostic, "读取头像标识", "返回了未请求的账号记录"); return AvatarCode::unavailable; }
    }
    out = std::move(candidate); return AvatarCode::ok;
}
}

wire::AvatarResult avatar_metadata(const std::vector<std::string>& accounts,
    std::vector<AvatarMetadata>& out, std::string* diagnostic) noexcept {
    out.clear();
    return avatar_database(diagnostic, [&](MYSQL* connection) {
        return avatar_metadata_on(connection, accounts, out, diagnostic);
    });
}

wire::AvatarResult store_avatar(const std::string& actor, const std::vector<uint8_t>& png,
    const std::string& hash, std::string* diagnostic) noexcept {
    return avatar_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !wire::valid_sha256_hex(hash) || png.empty()) return AvatarCode::invalid;
        if (png.size() > wire::avatar_size_limit) return AvatarCode::too_large;
        auto statement = avatar_statement(connection,
            "INSERT INTO user_avatars8(accountID,hash,png) VALUES(?,UNHEX(?),?) "
            "ON DUPLICATE KEY UPDATE hash=VALUES(hash),png=VALUES(png),updated_at=CURRENT_TIMESTAMP",
            {actor,hash,std::string_view(reinterpret_cast<const char*>(png.data()),png.size())}, diagnostic, "保存头像");
        return statement ? AvatarCode::ok : AvatarCode::unavailable;
    });
}

wire::AvatarResult load_avatar(const std::string& hash, std::vector<uint8_t>& png,
    std::string* diagnostic) noexcept {
    png.clear();
    return avatar_database(diagnostic, [&](MYSQL* connection) {
        if (!wire::valid_sha256_hex(hash)) return AvatarCode::invalid;
        auto statement = avatar_statement(connection,
            "SELECT png FROM user_avatars8 WHERE hash=UNHEX(?) LIMIT 1", {hash}, diagnostic, "查询头像图片");
        if (!statement) return AvatarCode::unavailable;
        // A fixed maximum buffer bounds allocation even if stored data is corrupt.
        std::vector<uint8_t> candidate(wire::avatar_size_limit);
        MYSQL_BIND column{}; unsigned long length = 0; bool is_null = false, truncated = false;
        column.buffer_type = MYSQL_TYPE_BLOB; column.buffer = candidate.data();
        column.buffer_length = static_cast<unsigned long>(candidate.size());
        column.length = &length; column.is_null = &is_null; column.error = &truncated;
        if (mysql_stmt_bind_result(statement.get(), &column) != 0) {
            statement_failure(diagnostic, "绑定头像图片读取", statement.get(), avatar_schema); return AvatarCode::unavailable;
        }
        const int result = mysql_stmt_fetch(statement.get());
        if (result == MYSQL_NO_DATA) return AvatarCode::not_found;
        if (result != 0 || truncated || is_null || !length || length > wire::avatar_size_limit) {
            audit_failure(diagnostic, "读取头像图片", "头像图片为空、超出大小限制或无法完整读取");
            return AvatarCode::unavailable;
        }
        candidate.resize(length); png = std::move(candidate); return AvatarCode::ok;
    });
}
