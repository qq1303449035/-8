#pragma once
// Included inside chh::server by MySQL.cpp; reuse its MySQL RAII and diagnostics.
namespace {
using SocialCode = wire::SocialResult;
SocialCode private_text_nonce_kind(MYSQL* connection, const std::string& message_id, std::string* diagnostic);
struct SocialRow { std::vector<std::string> values; std::vector<bool> nulls; };

constexpr StoreSchema social_schema{"好友功能数据表不存在，请先核对并执行friends_schema.sql迁移",
    "好友功能表字段不匹配，请核对friends_schema.sql", true, MYSQL_TYPE_STRING};
constexpr StoreSchema offline_schema{"离线时间所需数据表不存在",
    "user8缺少offlineTime字段，请核对离线时间部署说明", false, MYSQL_TYPE_STRING};
constexpr StoreSchema friend_online_time_schema{"好友在线时长所需数据表不存在",
    "好友在线时长字段不匹配，请核对friends_schema.sql及user8.onlineSeconds部署说明", false, MYSQL_TYPE_STRING};
constexpr StoreSchema migration_schema{"本地聊天记录迁移表缺失或结构不匹配，请执行local_history_migration.sql",
    "本地聊天记录迁移表缺失或结构不匹配，请执行local_history_migration.sql", true, MYSQL_TYPE_STRING};
constexpr StoreSchema private_media_schema{"私聊图片迁移未完成，请执行private_media_migration.sql",
    "私聊图片迁移未完成，请执行private_media_migration.sql", true, MYSQL_TYPE_STRING};

Statement social_statement(MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, std::string* diagnostic, const char* stage) {
    return store_statement(social_schema, connection, sql, values, diagnostic, stage);
}
bool social_rows(MYSQL_STMT* statement, const std::vector<unsigned long>& limits,
    size_t row_limit, std::vector<SocialRow>& rows, std::string* diagnostic, const char* stage) {
    std::vector<std::vector<char>> buffers(limits.size());
    std::vector<unsigned long> lengths(limits.size());
    std::unique_ptr<bool[]> nulls(new bool[limits.size()]{}), errors(new bool[limits.size()]{});
    std::vector<MYSQL_BIND> columns(limits.size());
    for (size_t index = 0; index < limits.size(); ++index) {
        buffers[index].resize(static_cast<size_t>(limits[index]) + 1);
        columns[index].buffer_type = MYSQL_TYPE_STRING;
        columns[index].buffer = buffers[index].data();
        columns[index].buffer_length = limits[index] + 1;
        columns[index].length = &lengths[index];
        columns[index].is_null = &nulls[index]; columns[index].error = &errors[index];
    }
    if (mysql_stmt_bind_result(statement, columns.data()) != 0)
        return statement_failure(diagnostic, stage, statement, social_schema);
    for (;;) {
        const auto fetched = mysql_stmt_fetch(statement);
        if (fetched == MYSQL_NO_DATA) return true;
        if (fetched == 1) return statement_failure(diagnostic, stage, statement, social_schema);
        if (fetched != 0 || rows.size() >= row_limit)
            return audit_failure(diagnostic, stage, "数据库结果超出允许范围或无法完整读取");
        SocialRow row;
        for (size_t index = 0; index < limits.size(); ++index) {
            if (errors[index] || lengths[index] > limits[index] ||
                (!nulls[index] && std::memchr(buffers[index].data(), '\0', lengths[index])))
                return audit_failure(diagnostic, stage, "数据库字段被截断或包含无效空字符");
            row.nulls.push_back(nulls[index]);
            row.values.emplace_back(nulls[index] ? "" : buffers[index].data(), nulls[index] ? 0 : lengths[index]);
        }
        rows.push_back(std::move(row));
    }
}
// Executes one statement and reads all of its rows; stage labels both steps.
bool query_rows(const StoreSchema& schema, MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, const std::vector<unsigned long>& limits,
    size_t row_limit, std::vector<SocialRow>& rows, std::string* diagnostic, const char* stage) {
    auto statement = store_statement(schema, connection, sql, values, diagnostic, stage);
    return statement && social_rows(statement.get(), limits, row_limit, rows, diagnostic, stage);
}
bool social_uint64(const std::string& value, uint64_t& out) {
    if (value.empty()) return false;
    uint64_t parsed = 0;
    for (char digit : value) {
        if (digit < '0' || digit > '9' || parsed > (UINT64_MAX - uint64_t(digit - '0')) / 10) return false;
        parsed = parsed * 10 + uint64_t(digit - '0');
    }
    out = parsed; return true;
}
bool social_id(const std::string& id) { return valid_input(id, kAccountLimit, true) && wire::valid_text(id); }
template<class Operation> SocialCode social_database(std::string* diagnostic, Operation operation) noexcept {
    return store_session<SocialCode>(social_schema, {"初始化好友数据库", "初始化好友数据库线程",
        "设置好友数据库安全会话", "处理好友数据库请求"}, 1, diagnostic, std::move(operation));
}
class SocialTransaction {
public:
    SocialTransaction(MYSQL* connection, std::string* diagnostic) : connection_(connection), diagnostic_(diagnostic) {
        active_ = mysql_autocommit(connection_, false) == 0;
        if (!active_) connection_failure(diagnostic_, "开始好友事务", connection_, social_schema);
    }
    ~SocialTransaction() { if (active_) mysql_rollback(connection_); }
    bool active() const { return active_; }
    bool commit() {
        if (mysql_commit(connection_) != 0) return connection_failure(diagnostic_, "提交好友事务", connection_, social_schema);
        active_ = false; return true;
    }
private:
    MYSQL* connection_; std::string* diagnostic_; bool active_ = false;
};
// Locks the account's friend_accounts8 row until the transaction ends, which
// orders its relationship, private-message and media changes.
bool lock_account(MYSQL* connection, const std::string& account, const StoreSchema& schema,
    std::string* diagnostic, const char* stage) {
    return bool(store_statement(schema, connection,
        "INSERT INTO friend_accounts8(accountID) VALUES(?) ON DUPLICATE KEY UPDATE accountID=accountID",
        {account}, diagnostic, stage));
}
bool lock_social_pair(MYSQL* connection, const std::string& low, const std::string& high, std::string* diagnostic) {
    // Both accounts are locked in byte order, including when no relation exists.
    // This also serializes the per-account relationship limit across distinct pairs.
    return lock_account(connection, low, social_schema, diagnostic, "锁定好友账号") &&
        lock_account(connection, high, social_schema, diagnostic, "锁定好友账号");
}
SocialCode social_lookup(MYSQL* connection, const std::string& peer, std::string* diagnostic) {
    auto statement = social_statement(connection, "SELECT accountID,accountName FROM user8 WHERE accountID=? LIMIT 2",
        {peer}, diagnostic, "查找好友账号");
    if (!statement) return SocialCode::unavailable;
    std::vector<SocialRow> rows;
    if (!social_rows(statement.get(), {64,512}, 2, rows, diagnostic, "读取好友账号")) return SocialCode::unavailable;
    if (rows.empty()) return SocialCode::not_found;
    if (rows.size() != 1) {
        audit_failure(diagnostic, "读取好友账号", "账号匹配不唯一"); return SocialCode::unavailable;
    }
    if (rows[0].values[0] != peer) return SocialCode::not_found;
    if (rows[0].nulls[1] || rows[0].values[1].empty() || !wire::valid_text(rows[0].values[1])) {
        audit_failure(diagnostic, "读取好友昵称", "账号昵称无效"); return SocialCode::unavailable;
    }
    return SocialCode::ok;
}
bool social_relation(MYSQL* connection, const std::string& low, const std::string& high,
    unsigned& state, std::string& requester, std::string* diagnostic) {
    std::vector<SocialRow> rows;
    if (!query_rows(social_schema, connection,
        "SELECT state,requested_by FROM friendships8 WHERE account_low=? AND account_high=?",
        {low,high}, {1,64}, 1, rows, diagnostic, "读取好友关系")) return false;
    state = 0; requester.clear();
    if (rows.empty()) return true;
    if ((rows[0].values[0] != "1" && rows[0].values[0] != "2") ||
        (rows[0].values[1] != low && rows[0].values[1] != high))
        return audit_failure(diagnostic, "读取好友关系", "好友关系数据无效");
    state = rows[0].values[0] == "1" ? 1u : 2u; requester = std::move(rows[0].values[1]); return true;
}
bool social_count(MYSQL* connection, const std::string& id, uint64_t& count, std::string* diagnostic) {
    std::vector<SocialRow> rows;
    return query_rows(social_schema, connection, "SELECT COUNT(*) FROM friendships8 WHERE account_low=? OR account_high=?",
        {id,id}, {20}, 1, rows, diagnostic, "检查好友数量") && rows.size() == 1 && social_uint64(rows[0].values[0], count);
}
SocialCode private_account_exists(MYSQL* connection, const std::string& id, std::string* diagnostic) {
    auto statement = social_statement(connection,
        "SELECT accountID FROM user8 WHERE accountID=? AND CAST(accountID AS BINARY)=CAST(? AS BINARY) LIMIT 2",
        {id,id}, diagnostic, "查找私聊账号");
    std::vector<SocialRow> rows;
    if (!statement || !social_rows(statement.get(), {64}, 2, rows, diagnostic, "读取私聊账号")) return SocialCode::unavailable;
    if (rows.empty()) return SocialCode::not_found;
    if (rows.size() != 1 || rows[0].nulls[0] || rows[0].values[0] != id) {
        audit_failure(diagnostic, "读取私聊账号", "账号匹配不唯一或不精确"); return SocialCode::unavailable;
    }
    return SocialCode::ok;
}
SocialCode require_private_pair(MYSQL* connection, const std::string& low, const std::string& high, std::string* diagnostic) {
    if (!lock_social_pair(connection, low, high, diagnostic)) return SocialCode::unavailable;
    const auto first = private_account_exists(connection, low, diagnostic);
    return first == SocialCode::ok ? private_account_exists(connection, high, diagnostic) : first;
}
// Only an absent new table permits legacy reads. Permission, schema and network
// errors remain failures, so they cannot reopen an owner's cleared history.
bool private_clear_schema(MYSQL* connection, bool& present, std::string* diagnostic) {
    present = false;
    constexpr char query[] = "SELECT owner_id,peer_id,clear_through_id FROM private_conversation_clears8 WHERE 1=0";
    Statement statement(mysql_stmt_init(connection));
    if (!statement) return connection_failure(diagnostic, "检查私聊清除表", connection, social_schema);
    if (mysql_stmt_prepare(statement.get(), query, static_cast<unsigned long>(sizeof(query) - 1)) != 0 ||
        mysql_stmt_execute(statement.get()) != 0) {
        if (mysql_stmt_errno(statement.get()) == 1146) return true;
        return statement_failure(diagnostic, "检查私聊清除表", statement.get(), social_schema);
    }
    std::vector<SocialRow> rows;
    if (!social_rows(statement.get(), {64,64,20}, 0, rows, diagnostic, "检查私聊清除表")) return false;
    present = true; return true;
}
// Caller has confirmed that private_conversation_clears8 exists.
bool private_clear_position(MYSQL* connection, const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic) {
    through = 0;
    std::vector<SocialRow> rows;
    if (!query_rows(social_schema, connection,
        "SELECT clear_through_id FROM private_conversation_clears8 WHERE owner_id=? AND peer_id=?",
        {actor,peer}, {20}, 1, rows, diagnostic, "读取本人私聊清除位置")) return false;
    if (rows.empty()) return true;
    if (rows[0].nulls[0] || !social_uint64(rows[0].values[0], through))
        return audit_failure(diagnostic, "读取本人私聊清除位置", "清除位置无效");
    return true;
}
bool private_clear_watermark(MYSQL* connection, const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic) {
    through = 0;
    bool present = false;
    if (!private_clear_schema(connection, present, diagnostic)) return false;
    return !present || private_clear_position(connection, actor, peer, through, diagnostic);
}
bool private_rows(MYSQL_STMT* statement, size_t limit, const std::string& actor, const std::string& peer,
    std::vector<PrivateMessage>& messages, std::string* diagnostic) {
    std::vector<SocialRow> rows;
    if (!social_rows(statement, {20,20,64,64,480,1}, limit, rows, diagnostic, "读取私聊消息")) return false;
    for (auto& row : rows) {
        PrivateMessage message; uint64_t timestamp = 0;
        if (!social_uint64(row.values[0], message.id) || !message.id ||
            !social_uint64(row.values[1], timestamp) || timestamp > UINT32_MAX ||
            !((row.values[2] == actor && row.values[3] == peer) || (row.values[2] == peer && row.values[3] == actor)) ||
            !wire::valid_chat_text(row.values[4]) || (row.values[5] != "0" && row.values[5] != "1"))
            return audit_failure(diagnostic, "读取私聊消息", "私聊记录内容或参与账号无效");
        message.timestamp = static_cast<uint32_t>(timestamp);
        message.sender_id = std::move(row.values[2]); message.recipient_id = std::move(row.values[3]);
        message.text = std::move(row.values[4]); message.recalled = row.values[5] == "1";
        messages.push_back(std::move(message));
    }
    return true;
}
// The legacy boundary is frozen by local_history_migration.sql, so one
// successful read per process is enough. The probe names every table of the
// local-history paths, so a skipped migration gets its own diagnostic.
bool private_legacy_boundary(MYSQL* connection, uint64_t& legacy, std::string* diagnostic) {
    struct Cache { std::mutex mutex; bool loaded = false; uint64_t value = 0; };
    static Cache cache;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.loaded) { legacy = cache.value; return true; }
    }
    std::vector<SocialRow> rows;
    if (!query_rows(migration_schema, connection,
        "SELECT s.next_id,n.message_id,r.message_id,c.last_at,e.completed_at,g.hash,w.clear_through_id "
        "FROM private_sequence8 s,private_nonces8 n,private_recalls8 r,private_contacts8 c,private_export8 e,"
        "media_grants8 g,private_conversation_clears8 w WHERE 1=0",
        {}, {20,20,20,32,32,32,20}, 0, rows, diagnostic, "检查本地聊天记录迁移") ||
        !query_rows(migration_schema, connection, "SELECT legacy_max_id FROM private_legacy8 WHERE id=1",
            {}, {20}, 1, rows, diagnostic, "读取旧私聊记录边界")) return false;
    uint64_t value = 0;
    if (rows.size() != 1 || rows[0].nulls[0] || !social_uint64(rows[0].values[0], value))
        return audit_failure(diagnostic, "读取旧私聊记录边界", "旧私聊记录边界未初始化，请执行local_history_migration.sql");
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.loaded = true; cache.value = value; legacy = value; return true;
}
// private_media_migration.sql adds the sender (peer_id), a push cursor (id)
// and a message count (refs) to media_grants8 and creates media_collection8.
// Everything that writes, pushes or removes a pending image right needs them;
// text messages do not.
// One successful probe per process is enough (the migration is never undone
// while the server runs).
bool private_media_ready(MYSQL* connection, std::string* diagnostic) {
    struct Cache { std::mutex mutex; bool ready = false; };
    static Cache cache;
    {
        std::lock_guard<std::mutex> lock(cache.mutex);
        if (cache.ready) return true;
    }
    std::vector<SocialRow> rows;
    if (!query_rows(private_media_schema, connection,
        "SELECT g.id,g.peer_id,g.refs,c.enabled_after FROM media_grants8 g,media_collection8 c WHERE 1=0",
        {}, {20,64,20,32}, 0, rows, diagnostic, "检查私聊图片迁移")) return false;
    std::lock_guard<std::mutex> lock(cache.mutex);
    cache.ready = true; return true;
}
// Reads LOWER(HEX(hash)) rows (at most limit) into released; used to report
// which images lost a reference, so the collector can look at them.
bool released_hashes(MYSQL* connection, std::string_view sql, const std::vector<std::string_view>& values,
    size_t limit, std::vector<std::string>* released, std::string* diagnostic, const char* stage) {
    std::vector<SocialRow> rows;
    if (!query_rows(social_schema, connection, sql, values, {64}, limit, rows, diagnostic, stage)) return false;
    for (auto& row : rows) {
        if (row.nulls[0] || !wire::valid_sha256_hex(row.values[0]))
            return audit_failure(diagnostic, stage, "图片标识无效");
        if (released) released->push_back(std::move(row.values[0]));
    }
    return true;
}
// A delivered message row is gone; its content-free tombstone still answers a
// retried send with the original id for seven days.
SocialCode private_nonce_tombstone(MYSQL* connection, const std::string& actor, const std::string& nonce,
    bool& found, uint64_t& message_id, uint32_t& timestamp, std::string* diagnostic) {
    found = false;
    std::vector<SocialRow> rows;
    if (!query_rows(social_schema, connection,
        "SELECT message_id,UNIX_TIMESTAMP(created_at) FROM private_nonces8 WHERE sender_id=? AND nonce=UNHEX(?)",
        {actor,nonce}, {20,20}, 1, rows, diagnostic, "检查已送达私聊防重复凭据"))
        return SocialCode::unavailable;
    if (rows.empty()) return SocialCode::ok;
    uint64_t id = 0, created = 0;
    if (rows[0].nulls[0] || rows[0].nulls[1] || !social_uint64(rows[0].values[0], id) || !id ||
        !social_uint64(rows[0].values[1], created) || created > UINT32_MAX) {
        audit_failure(diagnostic, "检查已送达私聊防重复凭据", "防重复凭据记录无效"); return SocialCode::unavailable;
    }
    found = true; message_id = id; timestamp = static_cast<uint32_t>(created); return SocialCode::ok;
}
// Caller holds both account locks in an open transaction. The sequence row
// stays locked until COMMIT, so ids become visible in allocation order and a
// recipient's delivery cursor never passes a row that commits later.
SocialCode private_insert(MYSQL* connection, const std::string& low, const std::string& high,
    const std::string& actor, const std::string& peer, const std::string& nonce, const std::string& body,
    const std::string& image, uint64_t& message_id, uint32_t& timestamp, std::string* diagnostic) {
    uint64_t legacy = 0;
    if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
    if (!image.empty() && !private_media_ready(connection, diagnostic)) return SocialCode::unavailable;
    auto sequence = social_statement(connection,
        "UPDATE private_sequence8 SET next_id=LAST_INSERT_ID(next_id+1) WHERE id=1", {}, diagnostic, "分配私聊消息编号");
    if (!sequence) return SocialCode::unavailable;
    if (mysql_stmt_affected_rows(sequence.get()) != 1) {
        audit_failure(diagnostic, "分配私聊消息编号", "私聊编号序列缺失，请执行local_history_migration.sql");
        return SocialCode::unavailable;
    }
    std::vector<SocialRow> rows;
    uint64_t id = 0, now = 0;
    if (!query_rows(social_schema, connection, "SELECT LAST_INSERT_ID(),UNIX_TIMESTAMP()",
        {}, {20,20}, 1, rows, diagnostic, "分配私聊消息编号")) return SocialCode::unavailable;
    if (rows.size() != 1 || !social_uint64(rows[0].values[0], id) || id <= legacy ||
        !social_uint64(rows[0].values[1], now) || !now || now > UINT32_MAX) {
        audit_failure(diagnostic, "分配私聊消息编号", "私聊编号或数据库时间无效"); return SocialCode::unavailable;
    }
    const auto encoded = std::to_string(id), time = std::to_string(now);
    auto message = social_statement(connection,
        "INSERT INTO private_messages8(id,account_low,account_high,sender_id,recipient_id,nonce,body,created_at) "
        "VALUES(?,?,?,?,?,UNHEX(?),?,FROM_UNIXTIME(?))",
        {encoded,low,high,actor,peer,nonce,body,time}, diagnostic, "保存私聊消息");
    if (!message) return SocialCode::unavailable;
    if (mysql_warning_count(connection) != 0) {
        audit_failure(diagnostic, "保存私聊消息", "数据库返回警告"); return SocialCode::unavailable;
    }
    if (!image.empty()) {
        auto attachment = social_statement(connection, "INSERT INTO private_message_media8(message_id,hash) VALUES(?,UNHEX(?))",
            {encoded,image}, diagnostic, "保存私聊图片关联");
        if (!attachment) return SocialCode::unavailable;
        // The recipient's pending download right outlives the message row and
        // ends with its image acknowledgement (118). The sender keeps its own
        // bytes and gets no right. One right serves every message of this
        // sender with this image; refs counts them, so recalling one of them
        // before delivery (recall_private) keeps the right for the others.
        auto grant = social_statement(connection,
            "INSERT INTO media_grants8(owner_id,hash,peer_id) VALUES(?,UNHEX(?),?) ON DUPLICATE KEY UPDATE refs=refs+1",
            {peer,image,actor}, diagnostic, "保存私聊图片授权");
        if (!grant) return SocialCode::unavailable;
    }
    auto tombstone = social_statement(connection,
        "INSERT INTO private_nonces8(sender_id,nonce,message_id,created_at) VALUES(?,UNHEX(?),?,FROM_UNIXTIME(?))",
        {actor,nonce,encoded,time}, diagnostic, "保存私聊防重复凭据");
    if (!tombstone) return SocialCode::unavailable;
    auto contacts = social_statement(connection,
        "INSERT INTO private_contacts8(owner_id,peer_id,last_id,last_at) VALUES(?,?,?,FROM_UNIXTIME(?)),(?,?,?,FROM_UNIXTIME(?)) "
        "ON DUPLICATE KEY UPDATE last_at=IF(last_id<CAST(? AS UNSIGNED),FROM_UNIXTIME(?),last_at),"
        "last_id=GREATEST(last_id,CAST(? AS UNSIGNED))",
        {actor,peer,encoded,time,peer,actor,encoded,time,encoded,time,encoded}, diagnostic, "保存私聊会话索引");
    if (!contacts) return SocialCode::unavailable;
    if (mysql_warning_count(connection) != 0) {
        audit_failure(diagnostic, "保存私聊会话索引", "数据库返回警告"); return SocialCode::unavailable;
    }
    message_id = id; timestamp = static_cast<uint32_t>(now); return SocialCode::ok;
}
// Recalled rows carry no body; an image carries its hash instead of "[图片]".
bool private_payload(const SocialRow& row, size_t recalled, size_t body, size_t hash, uint32_t& flags, std::string& out) {
    if (row.nulls[recalled] || (row.values[recalled] != "0" && row.values[recalled] != "1")) return false;
    if (row.values[recalled] == "1") { flags = wire::private_flag_recalled; out.clear(); return true; }
    if (!row.nulls[hash]) {
        if (!wire::valid_sha256_hex(row.values[hash])) return false;
        flags = wire::private_flag_image; out = row.values[hash]; return true;
    }
    if (row.nulls[body] || row.values[body].empty() || row.values[body].size() > wire::chat_text_limit ||
        !wire::valid_text(row.values[body])) return false;
    flags = 0; out = row.values[body]; return true;
}
// Fills each entry's name and the peer's own ID-visibility preference. An entry
// whose account vanished, is duplicated or has an unusable name keeps a
// placeholder with a hidden ID, so the owner can still see and remove it;
// without keep_missing, vanished accounts are dropped instead.
bool load_peer_names(MYSQL* connection, std::vector<FriendRecord>& entries, bool keep_missing,
    std::string* diagnostic, const char* stage, const char* fallback_stage) {
    if (entries.empty()) return true;
    // The plain IN list can use the accountID index; the binary one keeps
    // only exact IDs. Names longer than a reply allows read as NULL.
    std::string list = "?";
    for (size_t index = 1; index < entries.size(); ++index) list += ",?";
    std::vector<std::string_view> ids;
    for (int pass = 0; pass < 2; ++pass) for (const auto& entry : entries) ids.push_back(entry.id);
    const size_t row_limit = 2 * entries.size();
    const std::string columns = "SELECT u.accountID,CASE WHEN OCTET_LENGTH(u.accountName)<=512 THEN u.accountName END";
    const std::string filter = " WHERE u.accountID IN (" + list + ") AND CAST(u.accountID AS BINARY) IN (" + list +
        ") LIMIT " + std::to_string(row_limit + 1);
    // Account privacy is optional for older deployments. Prefer the joined
    // preference; if it is unavailable, read names alone and hide every ID.
    std::vector<SocialRow> found;
    const bool privacy_loaded = query_rows(social_schema, connection, columns +
        ",COALESCE(p.hide_id,0) FROM user8 u LEFT JOIN account_preferences8 p ON p.owner_id=u.accountID" + filter,
        ids, {64,512,1}, row_limit, found, diagnostic, stage);
    if (!privacy_loaded) {
        found.clear();
        if (!query_rows(social_schema, connection, columns + " FROM user8 u" + filter,
            ids, {64,512}, row_limit, found, diagnostic, fallback_stage)) return false;
    }
    std::vector<FriendRecord> kept;
    for (auto& entry : entries) {
        const SocialRow* match = nullptr; size_t matches = 0;
        for (const auto& row : found) if (row.values[0] == entry.id) { match = &row; ++matches; }
        if (!matches && !keep_missing) continue;
        if (matches == 1 && !match->values[1].empty() && wire::valid_text(match->values[1])) {
            entry.name = match->values[1];
            // An unavailable or invalid preference fails closed.
            entry.hide_account_id = !privacy_loaded || match->values[2] != "0";
        } else {
            entry.name = "[账号不可用]"; entry.hide_account_id = true;
        }
        kept.push_back(std::move(entry));
    }
    entries = std::move(kept); return true;
}
} // namespace

bool record_offline_time(const std::string& account, uint64_t unix_seconds, std::string* diagnostic) noexcept {
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(account) || !unix_seconds) {
            audit_failure(diagnostic, "保存最后离线时间", "账号或离线时间格式无效"); return SocialCode::invalid;
        }
        const auto owner = private_account_exists(connection, account, diagnostic);
        if (owner != SocialCode::ok) return owner;
        const auto encoded = std::to_string(unix_seconds);
        // The statement stays open until its warning count has been checked.
        auto checked = store_statement(offline_schema, connection, "SELECT FROM_UNIXTIME(?) IS NOT NULL", {encoded},
            diagnostic, "检查离线时间范围");
        std::vector<SocialRow> rows;
        if (!checked || !social_rows(checked.get(), {1}, 1, rows, diagnostic, "检查离线时间范围")) return SocialCode::unavailable;
        if (rows.size() != 1 || rows[0].values[0] != "1" || mysql_warning_count(connection) != 0) {
            audit_failure(diagnostic, "检查离线时间范围", "离线时间超出数据库日期范围"); return SocialCode::invalid;
        }
        // The conditional single-row UPDATE is atomic on both InnoDB and legacy
        // MyISAM user8 tables. Independent processes cannot move time backwards.
        auto update = store_statement(offline_schema, connection,
            "UPDATE user8 SET offlineTime=FROM_UNIXTIME(?) WHERE accountID=? "
            "AND CAST(accountID AS BINARY)=CAST(? AS BINARY) "
            "AND (offlineTime IS NULL OR offlineTime<FROM_UNIXTIME(?)) LIMIT 1",
            {encoded,account,account,encoded}, diagnostic, "保存最后离线时间");
        if (!update) return SocialCode::unavailable;
        if (mysql_warning_count(connection) != 0 || mysql_stmt_affected_rows(update.get()) > 1) {
            audit_failure(diagnostic, "保存最后离线时间", "数据库返回警告或更新行数异常"); return SocialCode::unavailable;
        }
        return SocialCode::ok;
    }) == SocialCode::ok;
}

bool list_friend_offline_times(const std::string& actor, std::vector<FriendOffline>& out,
    std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) {
            audit_failure(diagnostic, "读取好友离线时间", "账号格式无效"); return SocialCode::invalid;
        }
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        std::vector<SocialRow> rows;
        if (!query_rows(offline_schema, connection,
            "SELECT f.account_low,f.account_high,u.accountID,COALESCE(UNIX_TIMESTAMP(u.offlineTime),0) "
            "FROM friendships8 f JOIN user8 u ON u.accountID=CASE WHEN f.account_low=? THEN f.account_high ELSE f.account_low END "
            "AND CAST(u.accountID AS BINARY)=CASE WHEN f.account_low=? THEN f.account_high ELSE f.account_low END "
            "WHERE f.state=2 AND (f.account_low=? OR f.account_high=?) ORDER BY f.account_low,f.account_high LIMIT 201",
            {actor,actor,actor,actor}, {64,64,64,20}, wire::friend_limit, rows,
            diagnostic, "读取好友离线时间")) return SocialCode::unavailable;
        std::vector<FriendOffline> candidate;
        for (const auto& row : rows) {
            uint64_t offline = 0;
            const auto& low = row.values[0]; const auto& high = row.values[1]; const auto& peer = row.values[2];
            bool duplicate = false;
            for (const auto& item : candidate) if (item.peer_id == peer) duplicate = true;
            if (!(low < high) || (low != actor && high != actor) || !social_id(peer) ||
                peer != (low == actor ? high : low) || row.nulls[3] || !social_uint64(row.values[3], offline) ||
                duplicate) {
                audit_failure(diagnostic, "读取好友离线时间", "好友账号不精确、不唯一或离线时间无效"); return SocialCode::unavailable;
            }
            candidate.push_back({peer,offline});
        }
        out = std::move(candidate); return SocialCode::ok;
    }) == SocialCode::ok;
}

bool list_friend_online_times(const std::string& actor, std::vector<FriendOnlineTime>& out,
    std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) {
            audit_failure(diagnostic, "读取好友累计在线时长", "账号格式无效"); return SocialCode::invalid;
        }
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        std::vector<SocialRow> rows;
        if (!query_rows(friend_online_time_schema, connection,
            "SELECT f.account_low,f.account_high,u.accountID,COALESCE(u.onlineSeconds,0) "
            "FROM friendships8 f JOIN user8 u ON u.accountID=CASE WHEN f.account_low=? THEN f.account_high ELSE f.account_low END "
            "AND CAST(u.accountID AS BINARY)=CASE WHEN f.account_low=? THEN f.account_high ELSE f.account_low END "
            "WHERE f.state=2 AND (f.account_low=? OR f.account_high=?) ORDER BY f.account_low,f.account_high LIMIT 201",
            {actor,actor,actor,actor}, {64,64,64,20}, wire::friend_limit, rows,
            diagnostic, "读取好友累计在线时长")) return SocialCode::unavailable;
        std::vector<FriendOnlineTime> candidate;
        for (const auto& row : rows) {
            uint64_t online = 0;
            const auto& low = row.values[0]; const auto& high = row.values[1]; const auto& peer = row.values[2];
            bool duplicate = false;
            for (const auto& item : candidate) if (item.peer_id == peer) duplicate = true;
            if (!(low < high) || (low != actor && high != actor) || !social_id(peer) ||
                peer != (low == actor ? high : low) || row.nulls[3] || !social_uint64(row.values[3], online) ||
                duplicate) {
                audit_failure(diagnostic, "读取好友累计在线时长", "好友账号不精确、不唯一或在线时长无效"); return SocialCode::unavailable;
            }
            candidate.push_back({peer,online});
        }
        out = std::move(candidate); return SocialCode::ok;
    }) == SocialCode::ok;
}

wire::SocialResult list_friends(const std::string& actor, std::vector<FriendRecord>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT account_low,account_high,requested_by,state FROM friendships8 WHERE account_low=? OR account_high=? ORDER BY account_low,account_high LIMIT 201",
            {actor,actor}, {64,64,64,1}, wire::friend_limit, rows, diagnostic, "读取好友列表")) return SocialCode::unavailable;
        std::vector<FriendRecord> candidate;
        for (const auto& row : rows) {
            const auto& values = row.values;
            if (!(values[0] < values[1]) || (values[0] != actor && values[1] != actor) ||
                (values[2] != values[0] && values[2] != values[1]) || (values[3] != "1" && values[3] != "2")) {
                audit_failure(diagnostic, "读取好友列表", "好友关系数据无效"); return SocialCode::unavailable;
            }
            FriendRecord entry;
            entry.id = values[0] == actor ? values[1] : values[0];
            entry.relation = values[3] == "2" ? wire::FriendRelation::accepted :
                values[2] == actor ? wire::FriendRelation::outgoing : wire::FriendRelation::incoming;
            candidate.push_back(std::move(entry));
        }
        // A removed account stays listed, so the relationship can be cleaned up.
        if (!load_peer_names(connection, candidate, true, diagnostic, "读取好友昵称和隐私设置", "读取好友昵称"))
            return SocialCode::unavailable;
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult list_private_contacts(const std::string& actor,
    std::vector<FriendRecord>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        const auto account = private_account_exists(connection, actor, diagnostic);
        if (account != SocialCode::ok) return account;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        // Delivered messages are deleted, so conversations come from the
        // content-free index that every send updates for both participants.
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT c.peer_id,c.last_id FROM private_contacts8 c WHERE c.owner_id=? AND c.peer_id<>? "
            "AND NOT EXISTS (SELECT 1 FROM friendships8 f WHERE f.state=2 AND "
            "((f.account_low=? AND f.account_high=c.peer_id) OR (f.account_low=c.peer_id AND f.account_high=?))) "
            "AND EXISTS (SELECT 1 FROM user8 u WHERE u.accountID=c.peer_id AND CAST(u.accountID AS BINARY)=c.peer_id) "
            "ORDER BY c.last_id DESC LIMIT 50",
            {actor,actor,actor,actor}, {64,20}, wire::conversation_limit, rows, diagnostic, "读取临时会话列表"))
            return SocialCode::unavailable;
        std::vector<FriendRecord> candidate;
        for (const auto& row : rows) {
            uint64_t latest = 0;
            if (!social_id(row.values[0]) || row.values[0] == actor || !social_uint64(row.values[1], latest) || !latest) {
                audit_failure(diagnostic, "读取临时会话列表", "会话参与账号或消息位置无效"); return SocialCode::unavailable;
            }
            candidate.push_back({row.values[0], {}, wire::FriendRelation::temporary});
        }
        // An account can disappear between the participant and name reads.
        if (!load_peer_names(connection, candidate, false, diagnostic, "读取临时会话昵称和隐私设置", "读取临时会话昵称"))
            return SocialCode::unavailable;
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult change_friend(const std::string& actor, const std::string& peer,
    wire::FriendAction action, std::string* diagnostic) noexcept {
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || uint32_t(action) < 1 || uint32_t(action) > 5) return SocialCode::invalid;
        if (actor == peer) return SocialCode::self;
        // New relationships require a real, unambiguous account. Cleanup remains
        // possible when an existing user's account has subsequently been removed.
        if (action == wire::FriendAction::request || action == wire::FriendAction::accept) {
            const auto lookup = social_lookup(connection, peer, diagnostic);
            if (lookup != SocialCode::ok) return lookup;
        }
        const auto& low = actor < peer ? actor : peer;
        const auto& high = actor < peer ? peer : actor;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !lock_social_pair(connection, low, high, diagnostic)) return SocialCode::unavailable;
        unsigned state = 0; std::string requester;
        if (!social_relation(connection, low, high, state, requester, diagnostic)) return SocialCode::unavailable;
        std::string query;
        std::vector<std::string_view> parameters;
        if (action == wire::FriendAction::request) {
            if (state) return SocialCode::already;
            uint64_t actor_count = 0, peer_count = 0;
            if (!social_count(connection, actor, actor_count, diagnostic) || !social_count(connection, peer, peer_count, diagnostic)) return SocialCode::unavailable;
            if (actor_count >= wire::friend_limit || peer_count >= wire::friend_limit) return SocialCode::limit;
            query = "INSERT INTO friendships8(account_low,account_high,requested_by,state) VALUES(?,?,?,1)";
            parameters = {low,high,actor};
        } else if (action == wire::FriendAction::accept) {
            if (state == 2) return SocialCode::already;
            if (state != 1 || requester != peer) return SocialCode::no_request;
            query = "UPDATE friendships8 SET state=2 WHERE account_low=? AND account_high=?";
            parameters = {low,high};
        } else {
            if (action == wire::FriendAction::remove) { if (state != 2) return SocialCode::not_friend; }
            else if (state != 1 || requester != (action == wire::FriendAction::cancel ? actor : peer)) return SocialCode::no_request;
            query = "DELETE FROM friendships8 WHERE account_low=? AND account_high=?"; parameters = {low,high};
        }
        auto changed = social_statement(connection, query, parameters, diagnostic, "保存好友关系");
        if (!changed || mysql_stmt_affected_rows(changed.get()) != 1 || !transaction.commit()) return SocialCode::unavailable;
        return SocialCode::ok;
    });
}

wire::SocialResult get_private_clear_watermark(const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic) noexcept {
    through = 0;
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor == peer) return SocialCode::invalid;
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        uint64_t candidate = 0;
        if (!private_clear_watermark(connection, actor, peer, candidate, diagnostic)) return SocialCode::unavailable;
        through = candidate; return SocialCode::ok;
    });
}

wire::SocialResult list_private_clears(const std::string& actor, const std::string& after_peer,
    std::vector<PrivateClear>& out, bool& has_more, std::string* diagnostic) noexcept {
    out.clear(); has_more = false;
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || (!after_peer.empty() && !social_id(after_peer))) return SocialCode::invalid;
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        bool present = false;
        if (!private_clear_schema(connection, present, diagnostic)) return SocialCode::unavailable;
        if (!present) return SocialCode::ok;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT peer_id,clear_through_id FROM private_conversation_clears8 WHERE owner_id=? AND peer_id>? ORDER BY peer_id LIMIT 51",
            {actor,after_peer}, {64,20}, 51, rows, diagnostic, "同步本人私聊清除位置"))
            return SocialCode::unavailable;
        std::vector<PrivateClear> candidate;
        for (const auto& row : rows) {
            uint64_t value = 0;
            if (!social_id(row.values[0]) || row.values[0] == actor || row.values[0] <= after_peer ||
                row.nulls[1] || !social_uint64(row.values[1], value)) {
                audit_failure(diagnostic, "同步本人私聊清除位置", "清除记录账号或位置无效"); return SocialCode::unavailable;
            }
            candidate.push_back({row.values[0],value});
        }
        has_more = candidate.size() > wire::conversation_limit;
        if (has_more) candidate.pop_back();
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult clear_private_conversation(const std::string& actor, const std::string& peer,
    uint64_t& through, std::string* diagnostic, std::vector<std::string>* released) noexcept {
    through = 0;
    if (released) released->clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor == peer) return SocialCode::invalid;
        const auto& low = actor < peer ? actor : peer; const auto& high = actor < peer ? peer : actor;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic) ||
            !private_media_ready(connection, diagnostic)) return SocialCode::unavailable;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active()) return SocialCode::unavailable;
        const auto access = require_private_pair(connection, low, high, diagnostic);
        if (access != SocialCode::ok) return access;
        unsigned state = 0; std::string requester;
        if (!social_relation(connection, low, high, state, requester, diagnostic)) return SocialCode::unavailable;
        if (state == 2) return SocialCode::invalid;
        bool present = false;
        if (!private_clear_schema(connection, present, diagnostic)) return SocialCode::unavailable;
        if (!present) {
            audit_failure(diagnostic, "清除本人临时会话", "清除表不存在，请手动执行temporary_chat_clear.sql");
            return SocialCode::unavailable;
        }
        std::vector<SocialRow> rows;
        uint64_t candidate = 0, previous = 0;
        if (!query_rows(social_schema, connection,
                "SELECT COALESCE(MAX(id),0) FROM private_messages8 WHERE account_low=? AND account_high=?",
                {low,high}, {20}, 1, rows, diagnostic, "确定本人私聊清除位置") ||
            rows.size() != 1 || !social_uint64(rows[0].values[0], candidate) ||
            !private_clear_position(connection, actor, peer, previous, diagnostic)) return SocialCode::unavailable;
        if (candidate < previous) candidate = previous;
        if (candidate) {
            const auto encoded = std::to_string(candidate);
            // A string parameter would make GREATEST compare as text.
            auto stored = social_statement(connection,
                "INSERT INTO private_conversation_clears8(owner_id,peer_id,clear_through_id) VALUES(?,?,?) "
                "ON DUPLICATE KEY UPDATE clear_through_id=GREATEST(clear_through_id,CAST(? AS UNSIGNED))",
                {actor,peer,encoded,encoded}, diagnostic, "保存本人私聊清除位置");
            if (!stored || mysql_warning_count(connection) != 0) return SocialCode::unavailable;
        }
        // The watermark still filters the legacy export. Newer messages live on
        // the clients, so the server only drops this owner's contact entry,
        // whatever the peer sent that has not been delivered yet, and the
        // owner's pending image rights from that peer. Hashes beyond the report
        // limits are found by the collector's sweep.
        const auto floor = std::to_string(legacy);
        std::vector<std::string> candidates;
        if (!released_hashes(connection,
                "SELECT DISTINCT LOWER(HEX(a.hash)) FROM private_message_media8 a JOIN private_messages8 m ON m.id=a.message_id "
                "WHERE m.recipient_id=? AND m.sender_id=? AND m.id>? LIMIT 1000",
                {actor,peer,floor}, 1000, &candidates, diagnostic, "读取未送达私聊图片") ||
            !released_hashes(connection, "SELECT LOWER(HEX(hash)) FROM media_grants8 WHERE owner_id=? AND peer_id=? LIMIT 1000",
                {actor,peer}, 1000, &candidates, diagnostic, "读取本人私聊图片授权") ||
            !social_statement(connection, "DELETE FROM media_grants8 WHERE owner_id=? AND peer_id=?",
                {actor,peer}, diagnostic, "清除本人私聊图片授权")) return SocialCode::unavailable;
        if (!social_statement(connection, "DELETE FROM private_contacts8 WHERE owner_id=? AND peer_id=?",
                {actor,peer}, diagnostic, "清除本人私聊会话索引") ||
            !social_statement(connection,
                "DELETE a FROM private_message_media8 a JOIN private_messages8 m ON m.id=a.message_id "
                "WHERE m.recipient_id=? AND m.sender_id=? AND m.id>?",
                {actor,peer,floor}, diagnostic, "清除未送达私聊图片") ||
            !social_statement(connection,
                "DELETE FROM private_messages8 WHERE recipient_id=? AND sender_id=? AND id>?",
                {actor,peer,floor}, diagnostic, "清除未送达私聊消息")) return SocialCode::unavailable;
        if (!transaction.commit()) return SocialCode::unavailable;
        through = candidate;
        if (released) *released = std::move(candidates);
        return SocialCode::ok;
    });
}

wire::SocialResult private_notice_baseline(const std::string& actor,
    uint64_t& cursor, std::string* diagnostic) noexcept {
    cursor = 0;
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        std::vector<SocialRow> rows;
        uint64_t candidate = 0;
        if (!query_rows(social_schema, connection,
            "SELECT COALESCE(MAX(id),0) FROM private_messages8 WHERE recipient_id=? "
            "AND CAST(recipient_id AS BINARY)=CAST(? AS BINARY)",
            {actor,actor}, {20}, 1, rows, diagnostic, "读取收到私聊通知起点"))
            return SocialCode::unavailable;
        if (rows.size() != 1 || rows[0].nulls[0] || !social_uint64(rows[0].values[0], candidate)) {
            audit_failure(diagnostic, "读取收到私聊通知起点", "通知位置无效"); return SocialCode::unavailable;
        }
        cursor = candidate; return SocialCode::ok;
    });
}

wire::SocialResult private_notices_since(const std::string& actor, uint64_t after,
    PrivateNoticeBatch& out, std::string* diagnostic) noexcept {
    out = {};
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        bool clear_table = false;
        if (!private_clear_schema(connection, clear_table, diagnostic)) return SocialCode::unavailable;
        std::string query = "SELECT m.id,m.sender_id,m.recipient_id,";
        query += clear_table ? "COALESCE(c.clear_through_id,0)" : "0";
        query += " FROM private_messages8 m";
        if (clear_table) query += " LEFT JOIN private_conversation_clears8 c ON c.owner_id=m.recipient_id AND c.peer_id=m.sender_id";
        query += " WHERE m.recipient_id=? AND CAST(m.recipient_id AS BINARY)=CAST(? AS BINARY) AND m.id>? ORDER BY m.id LIMIT 32";
        const std::string cursor = std::to_string(after);
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection, query, {actor,actor,cursor}, {20,64,64,20}, 32, rows,
            diagnostic, "读取收到私聊通知")) return SocialCode::unavailable;
        PrivateNoticeBatch candidate; candidate.cursor = after;
        for (auto& row : rows) {
            uint64_t id = 0, through = 0;
            if (row.nulls[0] || row.nulls[1] || row.nulls[2] || row.nulls[3]
                || !social_uint64(row.values[0], id) || id <= candidate.cursor
                || !social_id(row.values[1]) || row.values[1] == actor || row.values[2] != actor
                || !social_uint64(row.values[3], through)) {
                audit_failure(diagnostic, "读取收到私聊通知", "通知记录账号或位置无效"); return SocialCode::unavailable;
            }
            candidate.cursor = id;
            if (id > through) candidate.notices.push_back({id,std::move(row.values[1])});
        }
        candidate.has_more = rows.size() == 32;
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult private_history(const std::string& actor, const std::string& peer, uint64_t before,
    PrivatePage& out, std::string* diagnostic) noexcept {
    out = {};
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer)) return SocialCode::invalid;
        if (actor == peer) return SocialCode::self;
        const auto& low = actor < peer ? actor : peer; const auto& high = actor < peer ? peer : actor;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active()) return SocialCode::unavailable;
        const auto access = require_private_pair(connection, low, high, diagnostic); if (access != SocialCode::ok) return access;
        uint64_t through = 0;
        if (!private_clear_watermark(connection, actor, peer, through, diagnostic)) return SocialCode::unavailable;
        std::string query = "SELECT id,UNIX_TIMESTAMP(created_at),sender_id,recipient_id,"
            "CASE WHEN recalled_at IS NULL THEN body ELSE '[消息已撤回]' END,recalled_at IS NOT NULL "
            "FROM private_messages8 WHERE account_low=? AND account_high=? AND id>?";
        const std::string cursor = std::to_string(before);
        const std::string floor = std::to_string(through);
        std::vector<std::string_view> parameters{low,high,floor};
        if (before) { query += " AND id < ?"; parameters.push_back(cursor); }
        query += " ORDER BY id DESC LIMIT 51";
        auto statement = social_statement(connection, query, parameters, diagnostic, "读取私聊历史");
        std::vector<PrivateMessage> descending;
        if (!statement || !private_rows(statement.get(), 51, actor, peer, descending, diagnostic)) return SocialCode::unavailable;
        PrivatePage candidate; candidate.has_older = descending.size() > wire::private_history_limit;
        if (candidate.has_older) descending.pop_back();
        for (auto it = descending.rbegin(); it != descending.rend(); ++it) candidate.messages.push_back(std::move(*it));
        std::vector<SocialRow> position;
        if (!query_rows(social_schema, connection,
                "SELECT COALESCE(MAX(id),0) FROM private_messages8 WHERE account_low=? AND account_high=?",
                {low,high}, {20}, 1, position, diagnostic, "读取私聊最新位置") ||
            position.size() != 1 || !social_uint64(position[0].values[0], candidate.latest_id)) return SocialCode::unavailable;
        if (candidate.latest_id < through) candidate.latest_id = through;
        if (!transaction.commit()) return SocialCode::unavailable;
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult private_since(const std::string& actor, const std::string& peer, uint64_t after,
    std::vector<PrivateMessage>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer)) return SocialCode::invalid;
        if (actor == peer) return SocialCode::self;
        const auto& low = actor < peer ? actor : peer; const auto& high = actor < peer ? peer : actor;
        SocialTransaction transaction(connection, diagnostic); if (!transaction.active()) return SocialCode::unavailable;
        const auto access = require_private_pair(connection, low, high, diagnostic); if (access != SocialCode::ok) return access;
        uint64_t through = 0;
        if (!private_clear_watermark(connection, actor, peer, through, diagnostic)) return SocialCode::unavailable;
        if (after < through) after = through;
        auto statement = social_statement(connection,
            "SELECT id,UNIX_TIMESTAMP(created_at),sender_id,recipient_id,"
            "CASE WHEN recalled_at IS NULL THEN body ELSE '[消息已撤回]' END,recalled_at IS NOT NULL "
            "FROM private_messages8 WHERE account_low=? AND account_high=? AND id>? ORDER BY id LIMIT 16",
            {low,high,std::to_string(after)}, diagnostic, "读取私聊新消息");
        std::vector<PrivateMessage> candidate;
        if (!statement || !private_rows(statement.get(), 16, actor, peer, candidate, diagnostic) || !transaction.commit()) return SocialCode::unavailable;
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult send_private(const std::string& actor, const std::string& peer,
    const std::string& nonce, const std::string& text, uint64_t& message_id, uint32_t& timestamp, bool& inserted,
    std::string* diagnostic, bool allow_new) noexcept {
    message_id = 0; timestamp = 0; inserted = false;
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || !wire::valid_nonce_hex(nonce) || !wire::valid_chat_text(text)) return SocialCode::invalid;
        if (actor == peer) return SocialCode::self;
        const auto& low = actor < peer ? actor : peer; const auto& high = actor < peer ? peer : actor;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        SocialTransaction transaction(connection, diagnostic); if (!transaction.active()) return SocialCode::unavailable;
        const auto access = require_private_pair(connection, low, high, diagnostic); if (access != SocialCode::ok) return access;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT id,UNIX_TIMESTAMP(created_at),sender_id,recipient_id,body FROM private_messages8 WHERE sender_id=? AND nonce=UNHEX(?)",
            {actor,nonce}, {20,20,64,64,480}, 1, rows, diagnostic, "检查私聊防重复凭据")) return SocialCode::unavailable;
        if (!rows.empty()) {
            uint64_t previous = 0, created = 0;
            if (rows[0].values[2] != actor || rows[0].values[3] != peer || rows[0].values[4] != text) return SocialCode::invalid;
            if (text == "[图片]") {
                const auto kind = private_text_nonce_kind(connection, rows[0].values[0], diagnostic);
                if (kind != SocialCode::ok) return kind;
            }
            if (!social_uint64(rows[0].values[0], previous) || !previous || !social_uint64(rows[0].values[1], created) ||
                created > UINT32_MAX || !transaction.commit()) return SocialCode::unavailable;
            message_id = previous; timestamp = static_cast<uint32_t>(created); return SocialCode::ok;
        }
        bool delivered = false; uint64_t previous = 0; uint32_t created = 0;
        const auto tombstone = private_nonce_tombstone(connection, actor, nonce, delivered, previous, created, diagnostic);
        if (tombstone != SocialCode::ok) return tombstone;
        if (delivered) {
            if (!transaction.commit()) return SocialCode::unavailable;
            message_id = previous; timestamp = created; return SocialCode::ok;
        }
        if (!allow_new) return SocialCode::rate_limited;
        const auto stored = private_insert(connection, low, high, actor, peer, nonce, text, {}, previous, created, diagnostic);
        if (stored != SocialCode::ok) return stored;
        if (!transaction.commit()) return SocialCode::unavailable;
        message_id = previous; timestamp = created; inserted = true; return SocialCode::ok;
    });
}

wire::SocialResult recall_private(const std::string& actor, const std::string& peer,
    uint64_t message_id, uint32_t& timestamp, std::string* diagnostic, std::vector<std::string>* released) noexcept {
    timestamp = 0;
    if (released) released->clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor == peer || !message_id)
            return SocialCode::invalid;
        const auto& low = actor < peer ? actor : peer;
        const auto& high = actor < peer ? peer : actor;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active()) return SocialCode::unavailable;
        const auto access = require_private_pair(connection, low, high, diagnostic);
        if (access != SocialCode::ok)
            return access == SocialCode::unavailable ? SocialCode::unavailable : SocialCode::recall_not_found;
        const auto encoded = std::to_string(message_id);
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT sender_id,recipient_id,UNIX_TIMESTAMP(created_at),recalled_at IS NOT NULL "
            "FROM private_messages8 WHERE id=? AND account_low=? AND account_high=? FOR UPDATE",
            {encoded, low, high}, {64,64,20,1}, 1, rows, diagnostic, "读取待撤回私聊消息"))
            return SocialCode::unavailable;
        uint32_t original = 0;
        std::vector<std::string> image;
        if (!rows.empty()) {
            if (rows[0].values[0] != actor || rows[0].values[1] != peer)
                return SocialCode::recall_forbidden;
            if (rows[0].values[3] != "0" && rows[0].values[3] != "1")
                return SocialCode::unavailable;
            uint64_t created = 0;
            if (!social_uint64(rows[0].values[2], created)) return SocialCode::unavailable;
            if (created > UINT32_MAX) return SocialCode::unavailable;
            original = static_cast<uint32_t>(created);
            // The issuer's retry, e.g. after the result was lost: its own copy is
            // updated only on ok, and the first recall already queued the event.
            if (rows[0].values[3] == "1") {
                if (!transaction.commit()) return SocialCode::unavailable;
                timestamp = original;
                return SocialCode::ok;
            }
            if (!released_hashes(connection, "SELECT LOWER(HEX(hash)) FROM private_message_media8 WHERE message_id=?",
                    {encoded}, 1, &image, diagnostic, "读取待撤回私聊图片") ||
                (!image.empty() && !private_media_ready(connection, diagnostic))) return SocialCode::unavailable;
            auto updated = social_statement(connection,
                "UPDATE private_messages8 SET body='[消息已撤回]',recalled_at=CURRENT_TIMESTAMP "
                "WHERE id=? AND sender_id=? AND recalled_at IS NULL",
                {encoded, actor}, diagnostic, "保存私聊撤回状态");
            if (!updated || mysql_stmt_affected_rows(updated.get()) != 1) return SocialCode::unavailable;
            if (!image.empty()) {
                // The recalled row no longer keeps its image. While it is still
                // undelivered its share of the recipient's right goes too; the
                // right itself goes only with its last message (refs), so an
                // earlier message with the same image from this sender, pending
                // or delivered but not yet downloaded, keeps it. A delivered copy
                // (row already gone) cannot be revoked, and a legacy row's rights
                // predate the sender column and stay.
                if (!social_statement(connection, "DELETE FROM private_message_media8 WHERE message_id=?",
                        {encoded}, diagnostic, "删除撤回的私聊图片关联") ||
                    (message_id > legacy && (!social_statement(connection,
                        "UPDATE media_grants8 SET refs=refs-1 WHERE owner_id=? AND peer_id=? AND hash=UNHEX(?) AND refs>0",
                        {peer, actor, image[0]}, diagnostic, "减少撤回的私聊图片授权") ||
                    !social_statement(connection,
                        "DELETE FROM media_grants8 WHERE owner_id=? AND peer_id=? AND hash=UNHEX(?) AND refs=0",
                        {peer, actor, image[0]}, diagnostic, "删除撤回的私聊图片授权"))))
                    return SocialCode::unavailable;
            }
        } else {
            // A delivered row has been deleted. Ownership can no longer be
            // checked here: the recipient applies the event only when its own
            // copy of this id was sent by the issuer.
            uint64_t next = 0;
            if (!query_rows(social_schema, connection, "SELECT next_id FROM private_sequence8 WHERE id=1",
                    {}, {20}, 1, rows, diagnostic, "读取私聊消息编号") ||
                rows.size() != 1 || !social_uint64(rows[0].values[0], next)) return SocialCode::unavailable;
            if (message_id > next) return SocialCode::recall_not_found;
            rows.clear();
            if (!query_rows(social_schema, connection,
                "SELECT 1 FROM private_recalls8 WHERE recipient_id=? AND sender_id=? AND message_id=? LIMIT 1",
                {peer, actor, encoded}, {1}, 1, rows, diagnostic, "检查待送达撤回事件"))
                return SocialCode::unavailable;
            // Answered like a fresh recall: the result must not reveal whether
            // the peer has fetched its events since, and it repairs a lost ok.
            if (!rows.empty()) return transaction.commit() ? SocialCode::ok : SocialCode::unavailable;
        }
        // Queued even while the row is still pending: it may already be on the
        // recipient's disk with its acknowledgement in flight.
        auto event = social_statement(connection,
            "INSERT INTO private_recalls8(recipient_id,sender_id,message_id) VALUES(?,?,?)",
            {peer, actor, encoded}, diagnostic, "保存私聊撤回事件");
        if (!event || mysql_warning_count(connection) != 0 || !transaction.commit())
            return SocialCode::unavailable;
        timestamp = original;
        if (released) *released = std::move(image);
        return SocialCode::ok;
    });
}

wire::SocialResult private_inbox(const std::string& actor, uint64_t message_after, size_t message_limit,
    uint64_t recall_after, size_t recall_limit, PrivateInbox& out, std::string* diagnostic) noexcept {
    out = {};
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || message_limit > 20 || recall_limit > 20) return SocialCode::invalid;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        PrivateInbox candidate;
        std::vector<SocialRow> rows;
        if (message_limit) {
            // Legacy rows reach the client through the export instead.
            const auto after = std::to_string((std::max)(message_after, legacy));
            if (!query_rows(social_schema, connection,
                "SELECT m.id,UNIX_TIMESTAMP(m.created_at),m.sender_id,LOWER(HEX(m.nonce)),m.recalled_at IS NOT NULL,m.body,"
                "LOWER(HEX(a.hash)) FROM private_messages8 m LEFT JOIN private_message_media8 a ON a.message_id=m.id "
                "WHERE m.recipient_id=? AND m.id>? ORDER BY m.id LIMIT " + std::to_string(message_limit),
                {actor, after}, {20,20,64,32,1,480,64}, message_limit, rows, diagnostic, "读取待送达私聊"))
                return SocialCode::unavailable;
            uint64_t cursor = (std::max)(message_after, legacy);
            for (auto& row : rows) {
                PrivateDelivery message; uint64_t created = 0;
                if (row.nulls[0] || row.nulls[1] || row.nulls[2] || row.nulls[3] ||
                    !social_uint64(row.values[0], message.id) || message.id <= cursor ||
                    !social_uint64(row.values[1], created) || created > UINT32_MAX ||
                    !social_id(row.values[2]) || row.values[2] == actor || !wire::valid_nonce_hex(row.values[3]) ||
                    !private_payload(row, 4, 5, 6, message.flags, message.body)) {
                    audit_failure(diagnostic, "读取待送达私聊", "待送达私聊记录内容或参与账号无效"); return SocialCode::unavailable;
                }
                cursor = message.id; message.timestamp = static_cast<uint32_t>(created);
                message.sender_id = std::move(row.values[2]); message.nonce = std::move(row.values[3]);
                candidate.messages.push_back(std::move(message));
            }
        }
        if (recall_limit) {
            rows.clear();
            if (!query_rows(social_schema, connection,
                "SELECT id,sender_id,message_id FROM private_recalls8 WHERE recipient_id=? AND id>? ORDER BY id LIMIT " +
                std::to_string(recall_limit), {actor, std::to_string(recall_after)}, {20,64,20}, recall_limit, rows,
                diagnostic, "读取待送达撤回事件")) return SocialCode::unavailable;
            uint64_t cursor = recall_after;
            for (auto& row : rows) {
                PrivateRecallEvent event;
                if (row.nulls[0] || row.nulls[1] || row.nulls[2] || !social_uint64(row.values[0], event.id) ||
                    event.id <= cursor || !social_id(row.values[1]) || row.values[1] == actor ||
                    !social_uint64(row.values[2], event.message_id) || !event.message_id) {
                    audit_failure(diagnostic, "读取待送达撤回事件", "撤回事件账号或编号无效"); return SocialCode::unavailable;
                }
                cursor = event.id; event.sender_id = std::move(row.values[1]);
                candidate.recalls.push_back(std::move(event));
            }
        }
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult ack_private_delivery(const std::string& actor, uint64_t through, std::string* diagnostic,
    std::vector<std::string>* released) noexcept {
    if (released) released->clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !through) return SocialCode::invalid;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        if (through <= legacy) return SocialCode::ok;
        // The recipient lock orders this with sends that are still inserting.
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !lock_account(connection, actor, social_schema, diagnostic, "锁定私聊账号"))
            return SocialCode::unavailable;
        const auto floor = std::to_string(legacy), ceiling = std::to_string(through);
        // The recipient's image rights stay until its image acknowledgement;
        // the collector only needs to look at these images again.
        std::vector<std::string> images;
        if (!released_hashes(connection,
                "SELECT DISTINCT LOWER(HEX(a.hash)) FROM private_message_media8 a JOIN private_messages8 m ON m.id=a.message_id "
                "WHERE m.recipient_id=? AND m.id>? AND m.id<=? LIMIT 200",
                {actor,floor,ceiling}, 200, &images, diagnostic, "读取已送达私聊图片关联") ||
            !social_statement(connection,
                "DELETE a FROM private_message_media8 a JOIN private_messages8 m ON m.id=a.message_id "
                "WHERE m.recipient_id=? AND m.id>? AND m.id<=?",
                {actor,floor,ceiling}, diagnostic, "删除已送达私聊图片关联") ||
            !social_statement(connection,
                "DELETE FROM private_messages8 WHERE recipient_id=? AND id>? AND id<=?",
                {actor,floor,ceiling}, diagnostic, "删除已送达私聊") ||
            !transaction.commit()) return SocialCode::unavailable;
        if (released) *released = std::move(images);
        return SocialCode::ok;
    });
}

wire::SocialResult ack_private_recalls(const std::string& actor, uint64_t through, std::string* diagnostic) noexcept {
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !through) return SocialCode::invalid;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !lock_account(connection, actor, social_schema, diagnostic, "锁定私聊账号") ||
            !social_statement(connection, "DELETE FROM private_recalls8 WHERE recipient_id=? AND id<=?",
                {actor,std::to_string(through)}, diagnostic, "删除已送达撤回事件") ||
            !transaction.commit()) return SocialCode::unavailable;
        return SocialCode::ok;
    });
}

wire::SocialResult purge_private_nonces(bool& more, std::string* diagnostic) noexcept {
    more = false;
    return social_database(diagnostic, [&](MYSQL* connection) {
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        constexpr uint64_t batch = 10000;
        auto removed = social_statement(connection,
            "DELETE FROM private_nonces8 WHERE created_at<NOW()-INTERVAL 7 DAY LIMIT " + std::to_string(batch),
            {}, diagnostic, "清理过期私聊防重复凭据");
        if (!removed) return SocialCode::unavailable;
        more = mysql_stmt_affected_rows(removed.get()) == batch;
        return SocialCode::ok;
    });
}

wire::SocialResult private_export_page(const std::string& actor, uint64_t after,
    PrivateExportPage& out, std::string* diagnostic) noexcept {
    out = {};
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        PrivateExportPage candidate; candidate.last_id = after;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection, "SELECT completed_at IS NOT NULL FROM private_export8 WHERE owner_id=?",
            {actor}, {1}, 1, rows, diagnostic, "读取私聊导出状态")) return SocialCode::unavailable;
        if ((!rows.empty() && rows[0].values[0] == "1") || after >= legacy) { out = std::move(candidate); return SocialCode::ok; }
        rows.clear();
        // One branch per account index; each branch honours this owner's own
        // clear watermark for that conversation, as the old history did.
        const std::string columns = "SELECT m.id AS id,UNIX_TIMESTAMP(m.created_at),m.sender_id,m.recipient_id,"
            "m.recalled_at IS NOT NULL,m.body,LOWER(HEX(a.hash)) FROM private_messages8 m "
            "LEFT JOIN private_message_media8 a ON a.message_id=m.id LEFT JOIN private_conversation_clears8 c ON ";
        const auto from = std::to_string(after), until = std::to_string(legacy);
        if (!query_rows(social_schema, connection,
            "(" + columns + "c.owner_id=m.sender_id AND c.peer_id=m.recipient_id "
            "WHERE m.sender_id=? AND m.id>? AND m.id<=? AND m.id>COALESCE(c.clear_through_id,0) ORDER BY m.id LIMIT 51) "
            "UNION ALL (" + columns + "c.owner_id=m.recipient_id AND c.peer_id=m.sender_id "
            "WHERE m.recipient_id=? AND m.id>? AND m.id<=? AND m.id>COALESCE(c.clear_through_id,0) ORDER BY m.id LIMIT 51) "
            "ORDER BY id LIMIT 51",
            {actor,from,until,actor,from,until}, {20,20,64,64,1,480,64}, 51, rows, diagnostic, "导出旧私聊记录"))
            return SocialCode::unavailable;
        for (auto& row : rows) {
            if (candidate.entries.size() == wire::private_history_limit) { candidate.more = true; break; }
            PrivateExportEntry entry; uint64_t created = 0;
            if (row.nulls[0] || row.nulls[1] || row.nulls[2] || row.nulls[3] ||
                !social_uint64(row.values[0], entry.id) || entry.id <= candidate.last_id || entry.id > legacy ||
                !social_uint64(row.values[1], created) || created > UINT32_MAX ||
                !social_id(row.values[2]) || !social_id(row.values[3]) || row.values[2] == row.values[3] ||
                (row.values[2] != actor && row.values[3] != actor) ||
                !private_payload(row, 4, 5, 6, entry.flags, entry.body)) {
                audit_failure(diagnostic, "导出旧私聊记录", "旧私聊记录内容或参与账号无效"); return SocialCode::unavailable;
            }
            entry.timestamp = static_cast<uint32_t>(created);
            entry.sender_id = std::move(row.values[2]); entry.recipient_id = std::move(row.values[3]);
            candidate.last_id = entry.id;
            candidate.entries.push_back(std::move(entry));
        }
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult complete_private_export(const std::string& actor, uint64_t through, std::string* diagnostic) noexcept {
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        const auto owner = private_account_exists(connection, actor, diagnostic);
        if (owner != SocialCode::ok) return owner;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        const auto encoded = std::to_string((std::min)(through, legacy));
        auto stored = social_statement(connection,
            "INSERT INTO private_export8(owner_id,through_id,completed_at) VALUES(?,?,NOW()) "
            "ON DUPLICATE KEY UPDATE through_id=GREATEST(through_id,CAST(? AS UNSIGNED)),completed_at=COALESCE(completed_at,NOW())",
            {actor,encoded,encoded}, diagnostic, "保存私聊导出完成状态");
        if (!stored || mysql_warning_count(connection) != 0) return SocialCode::unavailable;
        return SocialCode::ok;
    });
}

wire::SocialResult purge_private_legacy(const std::string& actor, PrivatePurge& cursor, std::string* diagnostic,
    std::vector<std::string>* released) noexcept {
    if (released) released->clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return SocialCode::invalid;
        uint64_t legacy = 0;
        if (!private_legacy_boundary(connection, legacy, diagnostic)) return SocialCode::unavailable;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection, "SELECT completed_at IS NOT NULL FROM private_export8 WHERE owner_id=?",
            {actor}, {1}, 1, rows, diagnostic, "读取私聊导出状态")) return SocialCode::unavailable;
        if (rows.empty() || rows[0].values[0] != "1") { cursor.finished = true; return SocialCode::ok; }
        const auto until = std::to_string(legacy);
        // A legacy row may go once both sides no longer need it: the other
        // side finished its own export, cleared past the row, or no longer
        // exists. Scans are bounded so each call stays within the 1 s timeout.
        for (int branch = 0; branch < 2; ++branch) {
            auto& after = branch == 0 ? cursor.sent_after : cursor.received_after;
            if (after >= legacy) continue;
            const std::string column = branch == 0 ? "sender_id" : "recipient_id";
            const std::string self = "m." + column, other = branch == 0 ? "m.recipient_id" : "m.sender_id";
            const std::string query = std::string("SELECT m.id,(e.owner_id IS NOT NULL OR m.id<=COALESCE(c.clear_through_id,0) OR "
                "NOT EXISTS (SELECT 1 FROM user8 u WHERE u.accountID=") + other + " AND CAST(u.accountID AS BINARY)=" + other + ")) "
                "FROM private_messages8 m LEFT JOIN private_export8 e ON e.owner_id=" + other + " AND e.completed_at IS NOT NULL "
                "LEFT JOIN private_conversation_clears8 c ON c.owner_id=" + other + " AND c.peer_id=" + self +
                " WHERE " + self + "=? AND m.id>? AND m.id<=? ORDER BY m.id LIMIT 500";
            rows.clear();
            if (!query_rows(social_schema, connection, query, {actor,std::to_string(after),until}, {20,1}, 500, rows,
                diagnostic, "查找可清理的旧私聊记录")) return SocialCode::unavailable;
            std::vector<std::string> removable;
            uint64_t last = after;
            for (const auto& row : rows) {
                uint64_t id = 0;
                if (row.nulls[0] || row.nulls[1] || !social_uint64(row.values[0], id) || id <= last || id > legacy ||
                    (row.values[1] != "0" && row.values[1] != "1")) {
                    audit_failure(diagnostic, "查找可清理的旧私聊记录", "旧私聊记录编号无效"); return SocialCode::unavailable;
                }
                last = id;
                if (row.values[1] == "1") removable.push_back(std::to_string(id));
            }
            if (!removable.empty()) {
                std::string list;
                std::vector<std::string_view> parameters;
                for (const auto& id : removable) { if (!list.empty()) list += ','; list += '?'; parameters.push_back(id); }
                std::vector<std::string_view> guarded = parameters;
                guarded.push_back(until); guarded.push_back(actor);
                std::vector<std::string> images;
                SocialTransaction transaction(connection, diagnostic);
                if (!transaction.active() ||
                    !released_hashes(connection, "SELECT DISTINCT LOWER(HEX(hash)) FROM private_message_media8 WHERE message_id IN (" +
                        list + ")", parameters, 500, &images, diagnostic, "读取已导出的旧私聊图片关联") ||
                    !social_statement(connection, "DELETE FROM private_message_media8 WHERE message_id IN (" + list + ")",
                        parameters, diagnostic, "清理已导出的旧私聊图片关联") ||
                    !social_statement(connection, "DELETE FROM private_messages8 WHERE id IN (" + list + ") AND id<=? AND " +
                        column + "=?", guarded, diagnostic, "清理已导出的旧私聊记录") ||
                    !transaction.commit()) return SocialCode::unavailable;
                if (mysql_autocommit(connection, true) != 0) {
                    connection_failure(diagnostic, "恢复好友数据库自动提交", connection, social_schema); return SocialCode::unavailable;
                }
                if (released) released->insert(released->end(), images.begin(), images.end());
            }
            after = rows.size() < 500 ? legacy : last;
        }
        cursor.finished = cursor.sent_after >= legacy && cursor.received_after >= legacy;
        return SocialCode::ok;
    });
}

wire::SocialResult private_media_rights(const std::string& actor, uint64_t after, size_t limit,
    std::vector<PrivateMediaRight>& out, std::string* diagnostic) noexcept {
    out.clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !limit || limit > 20) return SocialCode::invalid;
        if (!private_media_ready(connection, diagnostic)) return SocialCode::unavailable;
        std::vector<SocialRow> rows;
        if (!query_rows(social_schema, connection,
            "SELECT id,LOWER(HEX(hash)) FROM media_grants8 WHERE owner_id=? AND id>? ORDER BY id LIMIT " + std::to_string(limit),
            {actor, std::to_string(after)}, {20,64}, limit, rows, diagnostic, "读取待下载私聊图片")) return SocialCode::unavailable;
        std::vector<PrivateMediaRight> candidate;
        uint64_t cursor = after;
        for (auto& row : rows) {
            PrivateMediaRight right;
            if (row.nulls[0] || row.nulls[1] || !social_uint64(row.values[0], right.id) || right.id <= cursor ||
                !wire::valid_sha256_hex(row.values[1])) {
                audit_failure(diagnostic, "读取待下载私聊图片", "图片授权编号或标识无效"); return SocialCode::unavailable;
            }
            cursor = right.id; right.hash = std::move(row.values[1]);
            candidate.push_back(std::move(right));
        }
        out = std::move(candidate); return SocialCode::ok;
    });
}

wire::SocialResult acknowledge_private_media(const std::string& actor, const std::vector<std::string>& hashes,
    std::string* diagnostic, std::vector<std::string>* released) noexcept {
    if (released) released->clear();
    return social_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || hashes.size() > 50) return SocialCode::invalid;
        for (const auto& hash : hashes) if (!wire::valid_sha256_hex(hash)) return SocialCode::invalid;
        if (hashes.empty()) return SocialCode::ok;
        if (!private_media_ready(connection, diagnostic)) return SocialCode::unavailable;
        // Only this owner's own rows; no account lock is needed because a
        // concurrent send only inserts (and ON DUPLICATE keeps) a row. Only
        // hashes that had a right go to the collector: acknowledgements of
        // unknown hashes must not crowd its queue (a row that appears between
        // the two statements is deleted unreported; the sweep finds it).
        std::string list;
        std::vector<std::string_view> parameters{actor};
        for (const auto& hash : hashes) { list += list.empty() ? "UNHEX(?)" : ",UNHEX(?)"; parameters.push_back(hash); }
        std::vector<std::string> held;
        if (!released_hashes(connection, "SELECT DISTINCT LOWER(HEX(hash)) FROM media_grants8 WHERE owner_id=? AND hash IN (" + list + ")",
                parameters, hashes.size(), &held, diagnostic, "读取已下载私聊图片授权") ||
            (!held.empty() && !social_statement(connection, "DELETE FROM media_grants8 WHERE owner_id=? AND hash IN (" + list + ")",
                parameters, diagnostic, "删除已下载私聊图片授权"))) return SocialCode::unavailable;
        if (released) *released = std::move(held);
        return SocialCode::ok;
    });
}
