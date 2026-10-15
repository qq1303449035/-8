#pragma once
// Included inside chh::server after friends_store.h and profile_store.h.
namespace {
constexpr StoreSchema preferences_schema{"账号偏好或备注表不存在，请核对account_preferences.sql及好友迁移",
    "账号偏好或备注表字段不匹配", false, MYSQL_TYPE_STRING};
Statement preferences_statement(MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, std::string* diagnostic, const char* stage) {
    return store_statement(preferences_schema, connection, sql, values, diagnostic, stage);
}
template<class Result, class Operation> Result preferences_database(std::string* diagnostic, Operation operation) noexcept {
    return store_session<Result>(preferences_schema, {"初始化账号偏好数据库", "初始化账号偏好数据库线程",
        "设置账号偏好安全会话", "处理账号偏好请求"}, 1, diagnostic, std::move(operation));
}
bool preferences_account(MYSQL* connection, const std::string& actor, std::string* diagnostic) {
    const auto result = private_account_exists(connection, actor, diagnostic);
    if (result == SocialCode::ok) return true;
    if (result == SocialCode::not_found) audit_failure(diagnostic, "验证账号偏好所有者", "账号不存在或不精确");
    return false;
}
} // namespace

wire::AccountResult get_account_preferences(const std::string& actor, bool& hide_id,
    std::string* diagnostic) noexcept {
    hide_id = true;
    return preferences_database<AccountCode>(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return AccountCode::invalid;
        if (!preferences_account(connection, actor, diagnostic)) return AccountCode::unavailable;
        std::vector<SocialRow> rows;
        if (!query_rows(preferences_schema, connection, "SELECT hide_id FROM account_preferences8 WHERE owner_id=?",
            {actor}, {1}, 1, rows, diagnostic, "读取账号隐藏ID偏好")) return AccountCode::unavailable;
        if (!rows.empty() && (rows[0].nulls[0] || (rows[0].values[0] != "0" && rows[0].values[0] != "1"))) {
            audit_failure(diagnostic, "读取账号隐藏ID偏好", "偏好值不在允许范围"); return AccountCode::unavailable;
        }
        hide_id = !rows.empty() && rows[0].values[0] == "1";
        return AccountCode::ok;
    });
}

wire::AccountResult set_hide_account_id(const std::string& actor, bool hide_id,
    std::string* diagnostic) noexcept {
    return preferences_database<AccountCode>(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return AccountCode::invalid;
        if (!preferences_account(connection, actor, diagnostic)) return AccountCode::unavailable;
        const std::string_view value = hide_id ? "1" : "0";
        auto statement = preferences_statement(connection,
            "INSERT INTO account_preferences8(owner_id,hide_id) VALUES(?,?) ON DUPLICATE KEY UPDATE hide_id=?",
            {actor,value,value}, diagnostic, "保存账号隐藏ID偏好");
        if (!statement || mysql_warning_count(connection) != 0) return AccountCode::unavailable;
        return AccountCode::ok;
    });
}

wire::SocialResult list_friend_notes(const std::string& actor,
    std::vector<FriendNote>& out, std::string* diagnostic) noexcept {
    out.clear();
    return preferences_database<SocialCode>(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        if (!preferences_account(connection, actor, diagnostic)) return SocialCode::unavailable;
        std::vector<SocialRow> rows;
        if (!query_rows(preferences_schema, connection,
            "SELECT n.peer_id,n.note FROM friend_notes8 n JOIN friendships8 f "
            "ON f.account_low=LEAST(n.owner_id,n.peer_id) AND f.account_high=GREATEST(n.owner_id,n.peer_id) "
            "AND f.state=2 WHERE n.owner_id=? ORDER BY n.peer_id LIMIT 201",
            {actor}, {64,144}, wire::friend_limit, rows, diagnostic, "读取本人好友备注"))
            return SocialCode::unavailable;
        std::vector<FriendNote> candidate;
        for (auto& row : rows) {
            if (row.nulls[0] || row.nulls[1] || !social_id(row.values[0]) || row.values[0] == actor ||
                row.values[1].empty() || !wire::valid_friend_note(row.values[1])) {
                audit_failure(diagnostic, "读取本人好友备注", "好友备注记录内容无效"); return SocialCode::unavailable;
            }
            candidate.push_back({std::move(row.values[0]),std::move(row.values[1])});
        }
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult set_friend_note(const std::string& actor, const std::string& peer,
    const std::string& note, std::string* diagnostic) noexcept {
    return preferences_database<SocialCode>(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor == peer || !wire::valid_friend_note(note)) return SocialCode::invalid;
        const auto& low = actor < peer ? actor : peer; const auto& high = actor < peer ? peer : actor;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !lock_social_pair(connection, low, high, diagnostic)) return SocialCode::unavailable;
        if (!preferences_account(connection, actor, diagnostic) || !preferences_account(connection, peer, diagnostic))
            return SocialCode::unavailable;
        unsigned state = 0; std::string requester;
        if (!social_relation(connection, low, high, state, requester, diagnostic)) return SocialCode::unavailable;
        if (state != 2) return SocialCode::not_friend;
        auto statement = note.empty() ? preferences_statement(connection,
            "DELETE FROM friend_notes8 WHERE owner_id=? AND peer_id=?", {actor,peer}, diagnostic, "清除本人好友备注") :
            preferences_statement(connection,
                "INSERT INTO friend_notes8(owner_id,peer_id,note) VALUES(?,?,?) ON DUPLICATE KEY UPDATE note=?",
                {actor,peer,note,note}, diagnostic, "保存本人好友备注");
        if (!statement || mysql_warning_count(connection) != 0 || !transaction.commit()) return SocialCode::unavailable;
        return SocialCode::ok;
    });
}
