#pragma once
// Included inside chh::server after the existing stores by MySQL.cpp.
namespace {
constexpr size_t hero_usage_row_limit = wire::hero_rank_limit, hero_usage_name_limit = wire::hero_name_limit;
constexpr StoreSchema hero_usage_schema{"英雄统计表herouse不存在，请核对排行榜部署说明",
    "英雄统计表字段不匹配，请核对herouse字段定义", false, MYSQL_TYPE_STRING};

bool hero_usage_name_visible(std::string_view name) {
    if (name.empty() || name.size() > hero_usage_name_limit || !wire::valid_text(name) ||
        wire::text_characters(name) > 255) return false;
    for (size_t index = 0; index < name.size();) {
        uint32_t cp = static_cast<unsigned char>(name[index++]);
        unsigned extra = 0;
        if (cp >= 0xf0) { cp &= 7; extra = 3; }
        else if (cp >= 0xe0) { cp &= 15; extra = 2; }
        else if (cp >= 0xc0) { cp &= 31; extra = 1; }
        while (extra--) cp = (cp << 6) | (static_cast<unsigned char>(name[index++]) & 63);
        const bool space = cp == 0x20 || cp == 0x85 || cp == 0xa0 || cp == 0x1680 ||
            (cp >= 0x2000 && cp <= 0x200a) || cp == 0x2028 || cp == 0x2029 ||
            cp == 0x202f || cp == 0x205f || cp == 0x3000;
        if (!space) return true;
    }
    return false;
}
} // namespace

bool load_hero_usage(std::vector<HeroUsage>& out, std::string* diagnostic) noexcept {
    out.clear();
    if (diagnostic) diagnostic->clear();
    try {
        LibraryCall library;
        if (!library.active()) return audit_failure(diagnostic, "初始化英雄排行榜数据库", "数据库客户端库未初始化或正在停止");
        ThreadContext thread;
        if (!thread.active()) return audit_failure(diagnostic, "初始化英雄排行榜线程", "数据库线程环境初始化失败");
        auto connection = connect_database(1, diagnostic);
        if (!connection) return false;
        Statement statement(mysql_stmt_init(connection.get()));
        if (!statement) return connection_failure(diagnostic, "准备英雄统计读取", connection.get(), hero_usage_schema);
        constexpr char query[] = "SELECT heroID,heroName,jjcNum,dldNum,jjcWin,jjcLos,dldWin,dldLos "
            "FROM herouse ORDER BY ID LIMIT 4097";
        if (mysql_stmt_prepare(statement.get(), query, static_cast<unsigned long>(sizeof(query) - 1)) != 0 ||
            mysql_stmt_execute(statement.get()) != 0)
            return statement_failure(diagnostic, "读取英雄统计", statement.get(), hero_usage_schema);

        // Read names separately from numeric fields so malformed or oversized
        // names can use an ID label without rejecting valid heroes and counts.
        std::array<int64_t, 7> numbers{};
        std::array<char, 1021> name{}; // VARCHAR(255) can contain 1020 UTF-8 bytes.
        std::array<unsigned long, 8> lengths{};
        std::array<bool, 8> nulls{}, errors{};
        std::array<MYSQL_BIND, 8> columns{};
        for (size_t index = 0; index < columns.size(); ++index) {
            columns[index].length = &lengths[index];
            columns[index].is_null = &nulls[index]; columns[index].error = &errors[index];
            if (index == 1) {
                columns[index].buffer_type = MYSQL_TYPE_STRING;
                columns[index].buffer = name.data(); columns[index].buffer_length = static_cast<unsigned long>(name.size());
            } else {
                columns[index].buffer_type = MYSQL_TYPE_LONGLONG;
                columns[index].buffer = &numbers[index ? index - 1 : 0];
                columns[index].buffer_length = sizeof(int64_t);
            }
        }
        if (mysql_stmt_bind_result(statement.get(), columns.data()) != 0)
            return statement_failure(diagnostic, "绑定英雄统计读取", statement.get(), hero_usage_schema);
        std::vector<HeroUsage> candidate;
        size_t raw_rows = 0;
        for (;;) {
            const auto fetched = mysql_stmt_fetch(statement.get());
            if (fetched == MYSQL_NO_DATA) break;
            if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED)
                return statement_failure(diagnostic, "读取英雄统计记录", statement.get(), hero_usage_schema);
            if (++raw_rows > hero_usage_row_limit)
                return audit_failure(diagnostic, "读取英雄统计记录", "英雄统计原始记录超过4096行上限，未发布部分榜单");
            for (size_t index = 0; index < columns.size(); ++index)
                if (index != 1 && errors[index])
                    return audit_failure(diagnostic, "读取英雄统计记录", "英雄标识或计数无法完整读取");
            if (nulls[0] || numbers[0] < 0) continue;
            if (numbers[0] > INT32_MAX)
                return audit_failure(diagnostic, "读取英雄统计记录", "英雄标识超出原表INT范围");
            HeroUsage entry;
            entry.hero_id = static_cast<uint32_t>(numbers[0]);
            for (const auto& previous : candidate)
                if (previous.hero_id == entry.hero_id)
                    return audit_failure(diagnostic, "读取英雄统计记录", "有效heroID重复，未合并或发布重复统计");
            const std::string_view view(name.data(), lengths[1] <= name.size() ? lengths[1] : 0);
            entry.name = !nulls[1] && !errors[1] && hero_usage_name_visible(view)
                ? std::string(view) : "英雄 #" + std::to_string(entry.hero_id);
            uint64_t* counts[]{&entry.arena_uses,&entry.brawl_uses,&entry.arena_wins,
                &entry.arena_losses,&entry.brawl_wins,&entry.brawl_losses};
            for (size_t index = 0; index < 6; ++index) {
                const auto value = numbers[index + 1];
                if (!nulls[index + 2] && value > INT32_MAX)
                    return audit_failure(diagnostic, "读取英雄统计记录", "英雄计数超出原表INT范围");
                *counts[index] = nulls[index + 2] || value < 0 ? 0 : static_cast<uint64_t>(value);
            }
            candidate.push_back(std::move(entry));
        }
        out = std::move(candidate); return true;
    } catch (...) {
        return audit_failure(diagnostic, "处理英雄排行榜读取", "发生本地异常或内存不足");
    }
}

namespace {
// Joins the tokens of user8.special with single spaces, the published form. The
// ban check and the hero-permission parser accept the same separators.
std::string normalize_permission_list(std::string_view value) {
    std::string result;
    size_t begin = 0;
    while ((begin = value.find_first_not_of(permission_whitespace, begin)) != std::string_view::npos) {
        const size_t limit = (std::min)(value.find_first_of(permission_whitespace, begin), value.size());
        if (!result.empty()) result.push_back(' ');
        result.append(value.substr(begin, limit - begin));
        begin = limit;
    }
    return result;
}
}

bool load_permission_records(std::vector<PermissionRecord>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT accountID,accountName,special FROM user8 "
            "WHERE special IS NOT NULL AND TRIM(special)<>'' ORDER BY accountID LIMIT 4097",
            {}, {static_cast<unsigned long>(wire::permission_account_limit),
                 static_cast<unsigned long>(wire::permission_name_limit),
                 static_cast<unsigned long>(wire::hero_permissions_text_limit)},
            wire::hero_rank_limit + 1, rows, diagnostic, "读取权限排行榜"))
            return SocialCode::unavailable;
        if (rows.size() > wire::hero_rank_limit) {
            audit_failure(diagnostic, "读取权限排行榜", "权限排行榜原始记录超过4096行上限，未发布部分榜单");
            return SocialCode::unavailable;
        }
        std::vector<PermissionRecord> candidate;
        candidate.reserve(rows.size());
        for (const auto& row : rows) {
            // A row the ranking cannot publish is left out; it does not
            // withhold the ranking from everyone else.
            auto permissions = normalize_permission_list(row.values[2]);
            if (row.nulls[0] || row.nulls[1] || row.nulls[2] ||
                !social_id(row.values[0]) || row.values[1].empty() ||
                row.values[1].size() > wire::permission_name_limit ||
                !wire::valid_text(row.values[1]) || !wire::valid_permission_list(permissions)) continue;
            for (const auto& previous : candidate)
                if (previous.account_id == row.values[0]) {
                    audit_failure(diagnostic, "读取权限排行榜", "权限账号重复");
                    return SocialCode::unavailable;
                }
            // Hidden until the account's privacy preference has been read.
            candidate.push_back({row.values[0], row.values[1], std::move(permissions), {}, true});
        }
        // Apply the same privacy preference used by the friends and chat
        // views. A batch whose preferences cannot be read keeps its IDs
        // hidden, as when the optional table is absent; an account without a
        // preference row uses the public default.
        for (size_t start = 0; start < candidate.size(); start += wire::friend_limit) {
            const size_t end = (std::min)(candidate.size(), start + wire::friend_limit);
            std::string privacy_query = "SELECT owner_id,hide_id FROM account_preferences8 WHERE owner_id IN (";
            std::vector<std::string_view> ids;
            ids.reserve(end - start);
            for (size_t index = start; index < end; ++index) {
                if (!ids.empty()) privacy_query += ',';
                privacy_query += '?'; ids.push_back(candidate[index].account_id);
            }
            privacy_query += ')';
            std::string privacy_diagnostic;
            std::vector<SocialRow> privacy_rows;
            if (!query_rows(social_schema, connection, privacy_query, ids, {64, 1}, wire::friend_limit,
                privacy_rows, &privacy_diagnostic, "读取权限榜隐私设置")) break;
            for (size_t index = start; index < end; ++index) candidate[index].hide_account_id = false;
            for (const auto& row : privacy_rows) {
                if (!row.nulls[1] && row.values[1] == "0") continue;
                for (size_t index = start; index < end; ++index)
                    if (candidate[index].account_id == row.values[0]) { candidate[index].hide_account_id = true; break; }
            }
        }
        // Permission users are not necessarily friends, so their avatar
        // hashes are loaded separately from the optional avatar table, on this
        // connection. A missing/unavailable avatar table must not make the
        // permission ranking unavailable; those rows use the default avatar.
        for (size_t start = 0; start < candidate.size(); start += wire::friend_limit) {
            const size_t end = (std::min)(candidate.size(), start + wire::friend_limit);
            std::vector<std::string> accounts;
            accounts.reserve(end - start);
            for (size_t index = start; index < end; ++index) accounts.push_back(candidate[index].account_id);
            std::vector<AvatarMetadata> metadata;
            std::string avatar_diagnostic;
            if (avatar_metadata_on(connection, accounts, metadata, &avatar_diagnostic) != wire::AvatarResult::ok) break;
            for (const auto& avatar : metadata)
                for (size_t index = start; index < end; ++index)
                    if (candidate[index].account_id == avatar.account_id) { candidate[index].avatar_hash = avatar.hash; break; }
        }
        out = std::move(candidate);
        return SocialCode::ok;
    }) == SocialCode::ok;
}
