#pragma once
// Included inside chh::server after friends_store.h by MySQL.cpp.
bool load_hero_permissions(const std::string& actor, std::vector<uint32_t>& out,
    std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection, "SELECT accountID,special FROM user8 WHERE accountID=? LIMIT 2",
                {actor}, {64, static_cast<unsigned long>(wire::hero_permissions_text_limit)}, 2, rows,
                diagnostic, "读取本人英雄权限")) return SocialCode::unavailable;
        if (rows.size() != 1 || rows[0].nulls[0] || rows[0].values[0] != actor ||
            !parse_hero_permissions(rows[0].values[1], out)) {
            audit_failure(diagnostic, "读取本人英雄权限", "账号不精确、记录重复或权限字段无效");
            return SocialCode::unavailable;
        }
        return SocialCode::ok;
    }) == SocialCode::ok;
}

bool load_hero_catalog(const std::string& actor, std::vector<HeroCatalogEntry>& out,
    std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        std::vector<SocialRow> accounts;
        std::vector<uint32_t> permissions;
        if (!query_rows(social_schema, connection, "SELECT accountID,special FROM user8 WHERE accountID=? LIMIT 2",
                {actor}, {64, static_cast<unsigned long>(wire::hero_permissions_text_limit)}, 2, accounts,
                diagnostic, "读取本人可用英雄权限")) return SocialCode::unavailable;
        if (accounts.size() != 1 || accounts[0].nulls[0] || accounts[0].values[0] != actor ||
            !parse_hero_permissions(accounts[0].values[1], permissions)) {
            audit_failure(diagnostic, "读取本人可用英雄权限", "账号不精确、记录重复或权限字段无效");
            return SocialCode::unavailable;
        }
        auto statement = store_statement(hero_usage_schema, connection,
            "SELECT heroID,heroName,permissionID FROM herouse ORDER BY ID LIMIT 4097", {}, diagnostic, "读取英雄配置");
        if (!statement) return SocialCode::unavailable;
        // Names can fall back independently; numeric truncation must reject the snapshot.
        std::array<char, 12> hero_text{}, permission_text{};
        std::array<char, 1021> name_text{};
        const std::array<char*, 3> buffers{hero_text.data(), name_text.data(), permission_text.data()};
        const std::array<unsigned long, 3> capacities{12, 1021, 12};
        std::array<unsigned long, 3> lengths{};
        std::array<bool, 3> nulls{}, errors{};
        std::array<MYSQL_BIND, 3> columns{};
        for (size_t index = 0; index < columns.size(); ++index) {
            columns[index].buffer_type = MYSQL_TYPE_STRING;
            columns[index].buffer = buffers[index]; columns[index].buffer_length = capacities[index];
            columns[index].length = &lengths[index];
            columns[index].is_null = &nulls[index]; columns[index].error = &errors[index];
        }
        if (mysql_stmt_bind_result(statement.get(), columns.data()) != 0) {
            statement_failure(diagnostic, "绑定英雄配置", statement.get(), hero_usage_schema);
            return SocialCode::unavailable;
        }
        std::vector<HeroCatalogEntry> candidate;
        size_t raw_rows = 0;
        for (;;) {
            const auto fetched = mysql_stmt_fetch(statement.get());
            if (fetched == MYSQL_NO_DATA) break;
            if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED) {
                statement_failure(diagnostic, "读取英雄配置", statement.get(), hero_usage_schema);
                return SocialCode::unavailable;
            }
            if (++raw_rows > wire::hero_availability_id_limit ||
                (!nulls[0] && (errors[0] || lengths[0] >= hero_text.size())) ||
                (!nulls[2] && (errors[2] || lengths[2] >= permission_text.size()))) {
                audit_failure(diagnostic, "读取英雄配置", "行数超限或数字字段被截断");
                return SocialCode::unavailable;
            }
            uint64_t hero = 0, permission = 0;
            if (nulls[0]) continue;
            if (!social_uint64(std::string(hero_text.data(), lengths[0]), hero) || hero == 0 || hero > 65535) {
                audit_failure(diagnostic, "读取英雄权限配置", "英雄编号无效");
                return SocialCode::unavailable;
            }
            const auto id = static_cast<uint32_t>(hero);
            if (std::find_if(candidate.begin(), candidate.end(), [id](const HeroCatalogEntry& row) { return row.id == id; }) != candidate.end()) {
                audit_failure(diagnostic, "读取英雄权限配置", "有效heroID重复，未发布部分权限配置");
                return SocialCode::unavailable;
            }
            HeroCatalogEntry entry;
            entry.id = id;
            const std::string_view name(name_text.data(), lengths[1] <= name_text.size() ? lengths[1] : 0);
            entry.name = !nulls[1] && !errors[1] && name.size() <= wire::hero_catalog_name_limit && hero_usage_name_visible(name)
                ? std::string(name) : "英雄 #" + std::to_string(id);
            if (!nulls[2]) {
                const std::string value(permission_text.data(), lengths[2]);
                const bool negative = !value.empty() && value[0] == '-';
                if (!social_uint64(negative ? value.substr(1) : value, permission) ||
                    permission > (negative ? uint64_t{2147483648} : uint64_t{INT32_MAX})) {
                    audit_failure(diagnostic, "读取英雄权限配置", "权限编号不是完整INT字段");
                    return SocialCode::unavailable;
                }
                entry.available = !negative && (permission == 0 ||
                    std::binary_search(permissions.begin(), permissions.end(), static_cast<uint32_t>(permission)));
            }
            candidate.push_back(std::move(entry));
        }
        std::sort(candidate.begin(), candidate.end(), [](const HeroCatalogEntry& left, const HeroCatalogEntry& right) { return left.id < right.id; });
        out = std::move(candidate);
        return SocialCode::ok;
    }) == SocialCode::ok;
}

bool load_available_heroes(const std::string& actor, std::vector<uint32_t>& out,
    std::string* diagnostic) noexcept {
    out.clear();
    try {
        std::vector<HeroCatalogEntry> catalog;
        if (!load_hero_catalog(actor, catalog, diagnostic)) return false;
        std::vector<uint32_t> candidate;
        for (const auto& entry : catalog) if (entry.available) candidate.push_back(entry.id);
        out = std::move(candidate);
        return true;
    } catch (...) {
        return audit_failure(diagnostic, "处理可用英雄", "发生本地异常或内存不足");
    }
}
