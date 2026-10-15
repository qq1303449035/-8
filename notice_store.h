// Included inside chh::server after the shared MySQL/social helpers.
namespace {
bool valid_notice_field(const std::string& value, std::size_t limit, bool empty = false) {
    return value.size() <= limit && (empty || !value.empty()) && wire::valid_text(value);
}
template<class Operation> wire::GameNoteResult notice_database(std::string* diagnostic, Operation operation) noexcept {
    return preferences_database<wire::GameNoteResult>(diagnostic, std::move(operation));
}
// Update and delete name one note by these four fields.
bool valid_notice_key(const std::string& actor, const NoticeRecord& original) {
    return valid_notice_field(actor, 64) && valid_notice_field(original.uid, wire::game_note_uid_limit) &&
        valid_notice_field(original.game_name, wire::game_note_name_limit) &&
        valid_notice_field(original.notes_time, wire::game_note_time_limit);
}
}

wire::GameNoteResult list_notices(uint32_t requested_page, std::string query, std::string uid, std::string account,
    NoticePage& out, std::string* diagnostic) noexcept {
    out = {};
    return notice_database(diagnostic, [&](MYSQL* connection) {
        if (!wire::valid_game_note_query(query)) return wire::GameNoteResult::invalid;
        if (!wire::valid_game_note_uid_filter(uid)) return wire::GameNoteResult::invalid;
        if (!wire::valid_game_note_account_filter(account)) return wire::GameNoteResult::invalid;
        query = wire::normalize_game_note_query(std::move(query));
        // Limit the joined rows before filtering, matching the original search
        // scope. Only the requested page crosses the database connection.
        std::string scope =
            " FROM (SELECT n.AccountID,COALESCE(u.`account`,'') AS account_name,"
            "n.UID,n.GameName,n.Notes,n.NotesTime "
            "FROM notice n LEFT JOIN user7 u ON u.accountID=n.AccountID "
            "ORDER BY n.NotesTime DESC,n.AccountID,n.UID,n.GameName LIMIT " +
            std::to_string(wire::game_note_limit) + ") AS recent WHERE 1";
        std::vector<std::string_view> parameters;
        // Compare the UTF-8 bytes returned to the client. Binary comparisons
        // keep case and trailing spaces significant; LOCATE treats % and _ as
        // ordinary characters, just like std::string::find.
        if (!uid.empty()) {
            scope += " AND CAST(CONVERT(recent.UID USING utf8mb4) AS BINARY)=CAST(? AS BINARY)";
            parameters.push_back(uid);
        }
        if (!account.empty()) {
            scope += " AND CAST(CONVERT(recent.AccountID USING utf8mb4) AS BINARY)=CAST(? AS BINARY)";
            parameters.push_back(account);
        }
        if (!query.empty()) {
            scope += " AND (LOCATE(CAST(? AS BINARY),CAST(CONVERT(recent.GameName USING utf8mb4) AS BINARY))>0"
                " OR LOCATE(CAST(? AS BINARY),CAST(CONVERT(recent.account_name USING utf8mb4) AS BINARY))>0"
                " OR LOCATE(CAST(? AS BINARY),CAST(CONVERT(recent.Notes USING utf8mb4) AS BINARY))>0)";
            parameters.insert(parameters.end(), {query, query, query});
        }

        // SocialTransaction alone inherits the server's isolation level. Set
        // only this next transaction so count and page share an InnoDB snapshot
        // even when the server default is READ COMMITTED.
        constexpr char isolation[] = "SET TRANSACTION ISOLATION LEVEL REPEATABLE READ";
        if (mysql_real_query(connection, isolation, static_cast<unsigned long>(sizeof(isolation) - 1)) != 0) {
            connection_failure(diagnostic, "设置记事本分页事务", connection, preferences_schema);
            return wire::GameNoteResult::unavailable;
        }
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active()) return wire::GameNoteResult::unavailable;
        std::vector<SocialRow> rows;
        uint64_t matched = 0;
        if (!query_rows(preferences_schema, connection, "SELECT COUNT(*)" + scope, parameters,
            {20}, 1, rows, diagnostic, "统计记事本"))
            return wire::GameNoteResult::unavailable;
        if (rows.size() != 1 || rows[0].nulls[0] || !social_uint64(rows[0].values[0], matched) ||
            matched > wire::game_note_limit) {
            audit_failure(diagnostic, "统计记事本", "记事本数量无效或超出范围");
            return wire::GameNoteResult::unavailable;
        }
        const auto window = page_window(static_cast<size_t>(matched), requested_page, wire::game_note_page_size);
        rows.clear();
        if (matched != 0 && !query_rows(preferences_schema, connection,
            "SELECT recent.AccountID,recent.account_name,recent.UID,recent.GameName,recent.Notes,"
            "DATE_FORMAT(recent.NotesTime,'%Y-%m-%d %H:%i:%s')" + scope +
            " ORDER BY recent.NotesTime DESC,recent.AccountID,recent.UID,recent.GameName LIMIT " +
            std::to_string(wire::game_note_page_size) + " OFFSET " + std::to_string(window.first),
            parameters, {64,wire::game_note_account_name_limit,wire::game_note_uid_limit,
            wire::game_note_name_limit,wire::game_note_text_limit,wire::game_note_time_limit},
            wire::game_note_page_size, rows, diagnostic, "读取记事本"))
            return wire::GameNoteResult::unavailable;
        if (rows.size() != window.end - window.first) {
            audit_failure(diagnostic, "读取记事本", "记事本分页数量不一致");
            return wire::GameNoteResult::unavailable;
        }
        NoticePage candidate;
        candidate.records.reserve(rows.size());
        for (auto& row : rows) {
            if (row.nulls[0] || row.nulls[2] || row.nulls[3] || row.nulls[4] || row.nulls[5] ||
                !valid_notice_field(row.values[0], 64) ||
                !valid_notice_field(row.values[1], wire::game_note_account_name_limit, true) ||
                !valid_notice_field(row.values[2], wire::game_note_uid_limit) ||
                !valid_notice_field(row.values[3], wire::game_note_name_limit) ||
                !valid_notice_field(row.values[4], wire::game_note_text_limit, true) ||
                !valid_notice_field(row.values[5], wire::game_note_time_limit)) {
                audit_failure(diagnostic, "读取记事本", "记事本记录内容无效");
                return wire::GameNoteResult::unavailable;
            }
            candidate.records.push_back({std::move(row.values[0]), std::move(row.values[1]),
                std::move(row.values[2]), std::move(row.values[3]), std::move(row.values[4]),
                std::move(row.values[5])});
        }
        if (!transaction.commit()) return wire::GameNoteResult::unavailable;
        candidate.query = std::move(query); candidate.uid = std::move(uid); candidate.account = std::move(account);
        candidate.matched = static_cast<uint32_t>(matched); candidate.page = window.page;
        out = std::move(candidate);
        return wire::GameNoteResult::ok;
    });
}

wire::GameNoteResult create_notice(const std::string& actor, const std::string& uid,
    const std::string& game_name, const std::string& notes, std::string* diagnostic) noexcept {
    return notice_database(diagnostic, [&](MYSQL* connection) {
        if (!valid_notice_field(actor, 64) || !valid_notice_field(uid, wire::game_note_uid_limit) ||
            !valid_notice_field(game_name, wire::game_note_name_limit) ||
            !valid_notice_field(notes, wire::game_note_text_limit, true))
            return wire::GameNoteResult::invalid;
        auto statement = preferences_statement(connection,
            "INSERT INTO notice(AccountID,UID,GameName,Notes,NotesTime) VALUES(?,?,?,?,NOW())",
            {actor, uid, game_name, notes}, diagnostic, "新增记事本");
        if (!statement || mysql_warning_count(connection) != 0) return wire::GameNoteResult::unavailable;
        return mysql_stmt_affected_rows(statement.get()) == 1 ? wire::GameNoteResult::ok : wire::GameNoteResult::unavailable;
    });
}

wire::GameNoteResult update_notice(const std::string& actor, const NoticeRecord& original,
    const std::string& replacement, std::string* diagnostic) noexcept {
    return notice_database(diagnostic, [&](MYSQL* connection) {
        if (!valid_notice_key(actor, original) || !valid_notice_field(replacement, wire::game_note_text_limit, true))
            return wire::GameNoteResult::invalid;
        if (original.account_id != actor) return wire::GameNoteResult::forbidden;
        auto statement = preferences_statement(connection,
            "UPDATE notice SET Notes=? WHERE AccountID=? AND UID=? AND GameName=? AND NotesTime=? LIMIT 1",
            {replacement, actor, original.uid, original.game_name, original.notes_time}, diagnostic, "修改记事本");
        if (!statement || mysql_warning_count(connection) != 0) return wire::GameNoteResult::unavailable;
        if (mysql_stmt_affected_rows(statement.get()) == 1) return wire::GameNoteResult::ok;
        // Affected rows count only changed rows, so unchanged text also reports 0.
        std::vector<SocialRow> rows;
        if (!query_rows(preferences_schema, connection,
            "SELECT 1 FROM notice WHERE AccountID=? AND UID=? AND GameName=? AND NotesTime=? LIMIT 1",
            {actor, original.uid, original.game_name, original.notes_time}, {1}, 1, rows, diagnostic, "修改记事本"))
            return wire::GameNoteResult::unavailable;
        return rows.empty() ? wire::GameNoteResult::not_found : wire::GameNoteResult::ok;
    });
}

wire::GameNoteResult delete_notice(const std::string& actor, const NoticeRecord& original,
    std::string* diagnostic) noexcept {
    return notice_database(diagnostic, [&](MYSQL* connection) {
        if (!valid_notice_key(actor, original)) return wire::GameNoteResult::invalid;
        if (original.account_id != actor) return wire::GameNoteResult::forbidden;
        auto statement = preferences_statement(connection,
            "DELETE FROM notice WHERE AccountID=? AND UID=? AND GameName=? AND NotesTime=? LIMIT 1",
            {actor, original.uid, original.game_name, original.notes_time}, diagnostic, "删除记事本");
        if (!statement || mysql_warning_count(connection) != 0) return wire::GameNoteResult::unavailable;
        return mysql_stmt_affected_rows(statement.get()) == 1 ? wire::GameNoteResult::ok : wire::GameNoteResult::not_found;
    });
}
