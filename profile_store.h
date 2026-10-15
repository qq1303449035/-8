#pragma once
// Included inside chh::server after the other stores by MySQL.cpp.
namespace {
using AccountCode = wire::AccountResult;

// Never include SQL text, account identifiers, field values or raw errors.
constexpr StoreSchema profile_schema{"账号资料表不存在", "账号资料表字段不匹配", false, MYSQL_TYPE_STRING};
Statement profile_statement(MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, std::string* diagnostic, const char* stage) {
    return store_statement(profile_schema, connection, sql, values, diagnostic, stage);
}

template<class Operation> AccountCode profile_database(std::string* diagnostic, Operation operation) noexcept {
    return store_session<AccountCode>(profile_schema, {"初始化资料数据库", "初始化资料数据库线程",
        "设置资料数据库安全会话", "处理账号资料修改"}, 2, diagnostic, [&](MYSQL* connection) {
        // One schema-derived lock serializes all cooperating server processes.
        // The exclusive lease owns it through the final UPDATE. Returning the
        // lease resets (or closes) the connection and releases the named lock
        // on every return/exception, including for MyISAM tables.
        auto lock = profile_statement(connection,
            "SELECT GET_LOCK(SHA2(CONCAT(DATABASE(),':user8-profile'),256),1)",
            {}, diagnostic, "锁定账号资料修改");
        std::vector<SocialRow> rows;
        if (!lock || !social_rows(lock.get(), {1}, 1, rows, diagnostic, "读取资料修改锁") || rows.size() != 1)
            return AccountCode::unavailable;
        if (rows[0].nulls[0] || rows[0].values[0] != "1") {
            audit_failure(diagnostic, "锁定账号资料修改", "资料修改锁不可用或等待超时"); return AccountCode::unavailable;
        }
        return operation(connection);
    });
}

template<size_t Limit> struct ProfileField {
    std::array<char, Limit + 1> bytes{};
    unsigned long length = 0;
    ProfileField() = default;
    ~ProfileField() { SecureZeroMemory(bytes.data(), bytes.size()); }
    ProfileField(const ProfileField&) = delete;
    ProfileField& operator=(const ProfileField&) = delete;
    std::string_view view() const { return {bytes.data(), length}; }
};

// Bind directly into the caller's wiping buffer; never materialize a password
// in SocialRow, std::string or an unprotected copy for a comparison/CAS binding.
template<size_t Limit> bool read_profile_field(MYSQL* connection, const std::string& actor,
    const char* sql, ProfileField<Limit>& value, std::string* diagnostic) {
    auto statement = profile_statement(connection, sql, {actor,actor}, diagnostic, "查找账号资料");
    if (!statement) return false;
    std::array<char, kAccountLimit + 1> account{};
    unsigned long account_length = 0;
    bool account_null = false, value_null = false, account_error = false, value_error = false;
    std::array<MYSQL_BIND, 2> columns{};
    columns[0].buffer_type = MYSQL_TYPE_STRING; columns[0].buffer = account.data();
    columns[0].buffer_length = static_cast<unsigned long>(account.size()); columns[0].length = &account_length;
    columns[0].is_null = &account_null; columns[0].error = &account_error;
    columns[1].buffer_type = MYSQL_TYPE_STRING; columns[1].buffer = value.bytes.data();
    columns[1].buffer_length = static_cast<unsigned long>(value.bytes.size()); columns[1].length = &value.length;
    columns[1].is_null = &value_null; columns[1].error = &value_error;
    if (mysql_stmt_bind_result(statement.get(), columns.data()) != 0) {
        return statement_failure(diagnostic, "绑定账号资料读取", statement.get(), profile_schema);
    }
    const auto fetched = mysql_stmt_fetch(statement.get());
    if (fetched != 0 || account_error || value_error || account_null || value_null ||
        account_length != actor.size() || value.length == 0 || value.length > Limit ||
        std::memcmp(account.data(), actor.data(), actor.size()) != 0 ||
        std::memchr(value.bytes.data(), '\0', value.length) != nullptr) {
        return audit_failure(diagnostic, "读取账号资料", "账号不存在或字段为空、截断、不精确");
    }
    if (mysql_stmt_fetch(statement.get()) != MYSQL_NO_DATA)
        return audit_failure(diagnostic, "读取账号资料", "账号匹配不唯一或读取未完成");
    return true;
}

AccountCode finish_profile_update(MYSQL* connection, MYSQL_STMT* statement, std::string* diagnostic) {
    if (mysql_stmt_affected_rows(statement) != 1 || mysql_warning_count(connection) != 0) {
        audit_failure(diagnostic, "保存账号资料", "账号资料发生并发变化或数据库返回警告"); return AccountCode::unavailable;
    }
    return AccountCode::ok;
}
} // namespace

wire::AccountResult rename_account(const std::string& actor, const std::string& name,
    std::string* diagnostic) noexcept {
    if (diagnostic) diagnostic->clear();
    if (!social_id(actor) || !wire::valid_profile_name(name)) return AccountCode::invalid;
    return profile_database(diagnostic, [&](MYSQL* connection) {
        ProfileField<kNameLimit> previous;
        if (!read_profile_field(connection, actor,
            "SELECT accountID,accountName FROM user8 WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) LIMIT 2",
            previous, diagnostic)) return AccountCode::unavailable;
        // Legacy duplicates remain valid when this account keeps its exact name.
        if (previous.view() == name) return AccountCode::ok;
        auto duplicate = profile_statement(connection,
            "SELECT 1 FROM user8 WHERE accountName=? AND NOT(CAST(accountID AS BINARY)<=>CAST(? AS BINARY)) LIMIT 1",
            {name,actor}, diagnostic, "检查昵称是否重复");
        std::vector<SocialRow> rows;
        if (!duplicate || !social_rows(duplicate.get(), {1}, 1, rows, diagnostic, "读取昵称查重结果"))
            return AccountCode::unavailable;
        if (!rows.empty()) return AccountCode::name_taken;
        // STRING comparison above deliberately uses accountName's collation;
        // the actor and old value in this single-row CAS are byte-exact.
        auto changed = profile_statement(connection,
            "UPDATE user8 SET accountName=? WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) "
            "AND CAST(accountName AS BINARY)=CAST(? AS BINARY) LIMIT 1",
            {name,actor,actor,previous.view()}, diagnostic, "保存账号昵称");
        return changed ? finish_profile_update(connection, changed.get(), diagnostic) : AccountCode::unavailable;
    });
}

wire::AccountResult change_account_password(const std::string& actor,
    const std::string& current, const std::string& next, std::string* diagnostic) noexcept {
    if (diagnostic) diagnostic->clear();
    if (!social_id(actor) || !valid_input(current, kPasswordLimit, true) || !wire::valid_text(current) ||
        !wire::valid_profile_password(next)) return AccountCode::invalid;
    return profile_database(diagnostic, [&](MYSQL* connection) {
        ProfileField<kPasswordLimit> previous;
        if (!read_profile_field(connection, actor,
            "SELECT accountID,password FROM user8 WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) LIMIT 2",
            previous, diagnostic)) return AccountCode::unavailable;
        if (previous.view() != current) return AccountCode::password_mismatch;
        if (previous.view() == next) return AccountCode::ok;
        auto changed = profile_statement(connection,
            "UPDATE user8 SET password=? WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) "
            "AND CAST(password AS BINARY)=CAST(? AS BINARY) LIMIT 1",
            {next,actor,actor,previous.view()}, diagnostic, "保存账号密码");
        return changed ? finish_profile_update(connection, changed.get(), diagnostic) : AccountCode::unavailable;
    });
}
