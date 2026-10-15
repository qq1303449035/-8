#pragma once
// Included inside chh::server after friends_store.h and avatar_store.h by MySQL.cpp.
namespace {
constexpr StoreSchema online_ranking_schema{"在线时间排行榜所需user8表不存在",
    "user8缺少onlineSeconds字段，请核对在线时间部署说明", false, MYSQL_TYPE_STRING};
}

bool load_online_time_records(std::vector<OnlineTimeRecord>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        std::vector<SocialRow> rows;
        // The list is intentionally the top 100, not an all-account snapshot.
        // Binary ID order agrees with the in-memory tie breaker.
        if (!query_rows(online_ranking_schema, connection,
            "SELECT accountID,accountName,onlineSeconds FROM user8 "
            "WHERE onlineSeconds>0 ORDER BY onlineSeconds DESC,BINARY accountID ASC LIMIT 100",
            {}, {static_cast<unsigned long>(wire::permission_account_limit),
                 static_cast<unsigned long>(wire::permission_name_limit), 20},
            wire::online_rank_limit, rows, diagnostic, "读取在线时间排行榜"))
            return SocialCode::unavailable;
        std::vector<OnlineTimeRecord> candidate;
        candidate.reserve(rows.size());
        std::unordered_set<std::string> ids;
        ids.reserve(rows.size());
        for (const auto& row : rows) {
            uint64_t seconds = 0;
            if (row.nulls[2] || !social_uint64(row.values[2], seconds) || !seconds) {
                audit_failure(diagnostic, "读取在线时间排行榜", "累计在线秒数无效或超出uint64范围");
                return SocialCode::unavailable;
            }
            if (row.nulls[0] || row.nulls[1] || !social_id(row.values[0]) || row.values[1].empty() ||
                row.values[1].size() > wire::permission_name_limit || !wire::valid_text(row.values[1])) continue;
            if (!ids.insert(row.values[0]).second) {
                audit_failure(diagnostic, "读取在线时间排行榜", "在线时间排行榜账号重复");
                return SocialCode::unavailable;
            }
            // Leave IDs hidden until this account's preferences are known.
            candidate.push_back({row.values[0], row.values[1], {}, true, seconds});
        }
        for (size_t start = 0; start < candidate.size(); start += wire::friend_limit) {
            const size_t end = (std::min)(candidate.size(), start + wire::friend_limit);
            std::string privacy_query = "SELECT owner_id,hide_id FROM account_preferences8 WHERE owner_id IN (";
            std::vector<std::string_view> accounts;
            accounts.reserve(end - start);
            for (size_t index = start; index < end; ++index) {
                if (!accounts.empty()) privacy_query += ',';
                privacy_query += '?'; accounts.push_back(candidate[index].account_id);
            }
            privacy_query += ')';
            std::string privacy_diagnostic;
            std::vector<SocialRow> privacy_rows;
            if (!query_rows(social_schema, connection, privacy_query, accounts, {64, 1}, wire::friend_limit,
                privacy_rows, &privacy_diagnostic, "读取在线时间榜隐私设置")) break;
            for (size_t index = start; index < end; ++index) candidate[index].hide_account_id = false;
            for (const auto& row : privacy_rows) {
                if (!row.nulls[1] && row.values[1] == "0") continue;
                for (size_t index = start; index < end; ++index)
                    if (candidate[index].account_id == row.values[0]) { candidate[index].hide_account_id = true; break; }
            }
        }
        // An optional avatar failure still permits a complete time ranking.
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
