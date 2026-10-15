#pragma once
// Included inside chh::server by MySQL.cpp. Images use binary bindings only.
namespace {
using MediaCode = wire::MediaResult;
constexpr uint64_t media_upload_count_limit = 1000, media_upload_bytes_limit = 256ull * 1024 * 1024;
constexpr StoreSchema media_schema{"图片或好友数据表不存在，请执行media_schema.sql及好友数据库部署步骤",
    "图片数据表字段不匹配，请核对media_schema.sql", true, MYSQL_TYPE_BLOB};
Statement media_statement(MYSQL* connection, std::string_view sql,
    const std::vector<std::string_view>& values, std::string* diagnostic, const char* stage) {
    return store_statement(media_schema, connection, sql, values, diagnostic, stage);
}
// Writing or reading a whole image (up to 15 MiB) can take MySQL longer than the
// 1 s other requests allow per network step; only those two operations wait longer.
constexpr unsigned media_data_timeout = 5;
template<class Operation> MediaCode media_database(std::string* diagnostic, Operation operation, unsigned timeout = 1) noexcept {
    return store_session<MediaCode>(media_schema, {"初始化图片数据库", "初始化图片数据库线程",
        "设置图片数据库安全会话", "处理图片数据库请求"}, timeout, diagnostic, std::move(operation));
}
bool media_lock_actor(MYSQL* connection, const std::string& actor, std::string* diagnostic) {
    return lock_account(connection, actor, media_schema, diagnostic, "锁定图片账号");
}
// query_rows that also reports the MySQL error code of a failed statement, so
// the collector can tell a lock wait or a missing table from an outage.
bool media_rows(MYSQL* connection, std::string_view sql, const std::vector<std::string_view>& values,
    const std::vector<unsigned long>& limits, size_t row_limit, std::vector<SocialRow>& rows,
    unsigned& error, std::string* diagnostic, const char* stage) {
    error = 0;
    Statement statement(mysql_stmt_init(connection));
    if (!statement) { error = mysql_errno(connection); return connection_failure(diagnostic, stage, connection, media_schema); }
    std::vector<unsigned long> lengths(values.size());
    std::vector<MYSQL_BIND> parameters(values.size());
    for (size_t index = 0; index < values.size(); ++index) {
        lengths[index] = static_cast<unsigned long>(values[index].size());
        parameters[index].buffer_type = media_schema.binding;
        parameters[index].buffer = const_cast<char*>(values[index].data());
        parameters[index].buffer_length = lengths[index];
        parameters[index].length = &lengths[index];
    }
    if (mysql_stmt_prepare(statement.get(), sql.data(), static_cast<unsigned long>(sql.size())) != 0 ||
        (!values.empty() && mysql_stmt_bind_param(statement.get(), parameters.data()) != 0) ||
        mysql_stmt_execute(statement.get()) != 0) {
        error = mysql_stmt_errno(statement.get());
        return statement_failure(diagnostic, stage, statement.get(), media_schema);
    }
    // No columns: a statement without a result set (DELETE).
    return limits.empty() || social_rows(statement.get(), limits, row_limit, rows, diagnostic, stage);
}
// Whether the asset row exists. A plain read first, so that creating a missing
// asset never starts from a gap lock (two first stores of the same image
// would deadlock); an existing row is then held under a shared lock until
// the transaction ends, which orders this reference with the collector.
bool media_asset_locked(MYSQL* connection, const std::string& hash, bool& exists,
    std::string* diagnostic) {
    exists = false;
    std::vector<SocialRow> rows;
    if (!query_rows(media_schema, connection, "SELECT 1 FROM media_assets8 WHERE hash=UNHEX(?)",
        {hash}, {1}, 1, rows, diagnostic, "检查图片记录")) return false;
    if (rows.empty()) return true;
    rows.clear();
    if (!query_rows(media_schema, connection, "SELECT 1 FROM media_assets8 WHERE hash=UNHEX(?) FOR SHARE",
        {hash}, {1}, 1, rows, diagnostic, "锁定图片记录")) return false;
    exists = !rows.empty(); return true;
}
bool media_schema_ready(MYSQL* connection, std::string* diagnostic) {
    std::vector<SocialRow> rows;
    return query_rows(media_schema, connection,
        "SELECT 1 FROM media_assets8 a LEFT JOIN media_uploads8 u ON u.hash=a.hash "
        "LEFT JOIN media_favorites8 f ON f.hash=a.hash LEFT JOIN private_message_media8 p ON p.hash=a.hash WHERE 1=0",
        {}, {1}, 0, rows, diagnostic, "检查图片数据库结构");
}
// Caller holds the actor's account lock (or the ordered pair locks), so upload,
// favorite and conversation grants are checked in the same transaction as use.
// lock_asset: a transaction that creates a new reference holds the asset row
// shared until COMMIT, so the collector cannot delete it underneath.
MediaCode media_access(MYSQL* connection, const std::string& actor, const std::string& hash,
    bool lobby_visible, std::string* diagnostic, bool lock_asset = false) {
    // Existence only: reading OCTET_LENGTH(data) would fetch the whole BLOB,
    // and every load validates the stored size itself.
    std::vector<SocialRow> exists;
    if (!query_rows(media_schema, connection, lock_asset ? "SELECT 1 FROM media_assets8 WHERE hash=UNHEX(?) FOR SHARE" :
        "SELECT 1 FROM media_assets8 WHERE hash=UNHEX(?)",
        {hash}, {1}, 1, exists, diagnostic, "检查图片记录")) return MediaCode::unavailable;
    if (exists.empty()) return MediaCode::not_found;
    if (lobby_visible) return MediaCode::ok;
    std::vector<SocialRow> grants;
    // media_grants8 keeps private-image access after the message rows are
    // delivered and deleted; the join below covers legacy history.
    if (!query_rows(media_schema, connection,
        "SELECT 1 FROM media_uploads8 WHERE accountID=? AND hash=UNHEX(?) UNION ALL "
        "SELECT 1 FROM media_favorites8 WHERE accountID=? AND hash=UNHEX(?) UNION ALL "
        "SELECT 1 FROM media_grants8 WHERE owner_id=? AND hash=UNHEX(?) LIMIT 1",
        {actor,hash,actor,hash,actor,hash}, {1}, 1, grants, diagnostic, "检查本人图片授权")) return MediaCode::unavailable;
    if (!grants.empty()) return MediaCode::ok;
    bool clear_table = false;
    if (!private_clear_schema(connection, clear_table, diagnostic)) return MediaCode::unavailable;
    std::string query = "SELECT 1 FROM private_message_media8 a JOIN private_messages8 m ON m.id=a.message_id";
    std::vector<std::string_view> parameters;
    if (clear_table) {
        query += " LEFT JOIN private_conversation_clears8 c ON c.owner_id=? AND "
            "c.peer_id=CASE WHEN m.sender_id=? THEN m.recipient_id ELSE m.sender_id END";
        parameters = {actor,actor};
    }
    query += " WHERE a.hash=UNHEX(?) AND (m.sender_id=? OR m.recipient_id=?)";
    parameters.push_back(hash); parameters.push_back(actor); parameters.push_back(actor);
    if (clear_table) query += " AND m.id>COALESCE(c.clear_through_id,0)";
    query += " LIMIT 1";
    if (!query_rows(media_schema, connection, query, parameters, {1}, 1, grants, diagnostic, "检查私聊图片授权")) return MediaCode::unavailable;
    return grants.empty() ? MediaCode::forbidden : MediaCode::ok;
}
// The asset insert tells an admin task apart from an outage: a database still on
// the pre-original-size CHECK media_asset_size rejects rows over 2 MiB (3819), and
// a statement over max_allowed_packet makes MySQL drop the connection (the client
// only sees 2013), so that limit is compared before sending a large image.
MediaCode media_insert_asset(MYSQL* connection, const std::string& hash,
    const std::vector<uint8_t>& bytes, std::string* diagnostic) {
    constexpr std::string_view sql="INSERT INTO media_assets8(hash,data) VALUES(UNHEX(?),?) ON DUPLICATE KEY UPDATE hash=hash";
    constexpr const char* stage="保存图片内容";
    constexpr const char* packet_reason="MySQL的max_allowed_packet小于图片大小，请设为至少16M（建议64M，见MEDIA_MIGRATION.md）";
    constexpr size_t former_limit=2*1024*1024, statement_overhead=1024;
    if (bytes.size()>former_limit) {
        std::vector<SocialRow> packet; uint64_t allowed=0;
        if (!query_rows(media_schema,connection,"SELECT @@SESSION.max_allowed_packet",{},{20},1,packet,diagnostic,"检查max_allowed_packet") ||
            packet.size()!=1 || !social_uint64(packet[0].values[0],allowed)) return MediaCode::unavailable;
        if (allowed<bytes.size()+statement_overhead) { audit_failure(diagnostic,stage,packet_reason); return MediaCode::upgrade_pending; }
    }
    Statement statement(mysql_stmt_init(connection));
    if (!statement) { connection_failure(diagnostic,stage,connection,media_schema); return MediaCode::unavailable; }
    unsigned long lengths[2]{static_cast<unsigned long>(hash.size()),static_cast<unsigned long>(bytes.size())};
    MYSQL_BIND parameters[2]{};
    parameters[0].buffer_type=media_schema.binding; parameters[0].buffer=const_cast<char*>(hash.data());
    parameters[1].buffer_type=media_schema.binding; parameters[1].buffer=const_cast<uint8_t*>(bytes.data());
    for (size_t index=0; index<2; ++index) { parameters[index].buffer_length=lengths[index]; parameters[index].length=&lengths[index]; }
    if (mysql_stmt_prepare(statement.get(),sql.data(),static_cast<unsigned long>(sql.size()))==0 &&
        mysql_stmt_bind_param(statement.get(),parameters)==0 && mysql_stmt_execute(statement.get())==0) return MediaCode::ok;
    const auto error=mysql_stmt_errno(statement.get());
    if (bytes.size()>former_limit && error==3819) {
        audit_failure(diagnostic,stage,"数据库仍是旧的2MiB图片大小限制，请执行media_original_migration.sql（见MEDIA_MIGRATION.md）",error,nullptr,true);
        return MediaCode::upgrade_pending;
    }
    if (bytes.size()>former_limit && (error==1153 || error==1301)) {
        audit_failure(diagnostic,stage,packet_reason,error,nullptr,true); return MediaCode::upgrade_pending;
    }
    statement_failure(diagnostic,stage,statement.get(),media_schema);
    return MediaCode::unavailable;
}
SocialCode private_text_nonce_kind(MYSQL* connection, const std::string& message_id, std::string* diagnostic) {
    std::vector<SocialRow> rows;
    if (!query_rows(media_schema, connection, "SELECT 1 FROM private_message_media8 WHERE message_id=?", {message_id}, {1}, 1,
        rows, diagnostic, "检查私聊消息类型")) return SocialCode::unavailable;
    return rows.empty() ? SocialCode::ok : SocialCode::invalid;
}
}

wire::MediaResult list_media_favorites(const std::string& actor, std::vector<std::string>& hashes, std::string* diagnostic) noexcept {
    hashes.clear();
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor)) return MediaCode::invalid;
        if (!media_schema_ready(connection, diagnostic)) return MediaCode::unavailable;
        std::vector<SocialRow> rows;
        if (!query_rows(media_schema, connection, "SELECT LOWER(HEX(f.hash)),a.hash IS NOT NULL FROM media_favorites8 f "
            "LEFT JOIN media_assets8 a ON a.hash=f.hash WHERE f.accountID=? ORDER BY f.created_at DESC,f.hash LIMIT 101",
            {actor}, {64,1}, wire::favorite_limit, rows, diagnostic, "读取收藏列表")) return MediaCode::unavailable;
        std::vector<std::string> candidate;
        for (auto& row : rows) {
            if (row.nulls[0] || row.nulls[1] || !wire::valid_sha256_hex(row.values[0]) || row.values[1] != "1") {
                audit_failure(diagnostic, "读取收藏列表", "收藏图片记录无效"); return MediaCode::unavailable;
            }
            candidate.push_back(std::move(row.values[0]));
        }
        hashes = std::move(candidate); return MediaCode::ok;
    });
}

wire::MediaResult store_media(const std::string& actor, const std::string& hash,
    const std::vector<uint8_t>& bytes, std::string* diagnostic) noexcept {
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !wire::valid_sha256_hex(hash) || bytes.empty()) return MediaCode::invalid;
        if (bytes.size() > wire::media_size_limit) return MediaCode::too_large;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !media_lock_actor(connection, actor, diagnostic)) return MediaCode::unavailable;
        bool stored = false;
        // Shared: an existing asset row is never written here, and the lock is
        // held through the quota query below (which can read every one of the
        // actor's uploads), so other accounts' stores, sends and favourites of
        // the same image must not wait for it. It still keeps the collector out.
        if (!media_asset_locked(connection, hash, stored, diagnostic)) return MediaCode::unavailable;
        std::vector<SocialRow> existing;
        if (!query_rows(media_schema, connection, "SELECT 1 FROM media_uploads8 WHERE accountID=? AND hash=UNHEX(?)",
            {actor,hash}, {1}, 1, existing, diagnostic, "检查已上传图片")) return MediaCode::unavailable;
        if (existing.empty()) {
            std::vector<SocialRow> quota;
            if (!query_rows(media_schema, connection, "SELECT COUNT(*),COALESCE(SUM(OCTET_LENGTH(a.data)),0),COUNT(a.hash) "
                "FROM media_uploads8 u LEFT JOIN media_assets8 a ON a.hash=u.hash WHERE u.accountID=?",
                {actor}, {20,20,20}, 1, quota, diagnostic, "检查上传图片额度")) return MediaCode::unavailable;
            uint64_t count=0, used=0, present=0;
            if (quota.size()!=1 || !social_uint64(quota[0].values[0],count) || !social_uint64(quota[0].values[1],used) ||
                !social_uint64(quota[0].values[2],present) || present != count) {
                audit_failure(diagnostic, "检查上传图片额度", "上传图片记录不完整"); return MediaCode::unavailable;
            }
            if (count >= media_upload_count_limit || used > media_upload_bytes_limit || bytes.size() > media_upload_bytes_limit-used)
                return MediaCode::limit;
        }
        if (!stored) {
            const auto asset = media_insert_asset(connection, hash, bytes, diagnostic);
            if (asset != MediaCode::ok) return asset;
        }
        // The upload row keeps the asset for the grace period while the private
        // message that follows is written; a repeated send restarts it.
        auto grant = media_statement(connection, "INSERT INTO media_uploads8(accountID,hash) VALUES(?,UNHEX(?)) "
            "ON DUPLICATE KEY UPDATE created_at=CURRENT_TIMESTAMP",
            {actor,hash}, diagnostic, "保存上传图片授权");
        if (!grant || !transaction.commit()) return MediaCode::unavailable;
        return MediaCode::ok;
    }, media_data_timeout);
}

wire::MediaResult authorize_media(const std::string& actor, const std::string& hash, bool lobby_visible, std::string* diagnostic) noexcept {
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !wire::valid_sha256_hex(hash)) return MediaCode::invalid;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !media_lock_actor(connection, actor, diagnostic)) return MediaCode::unavailable;
        const auto access = media_access(connection, actor, hash, lobby_visible, diagnostic);
        if (access != MediaCode::ok) return access;
        return transaction.commit() ? MediaCode::ok : MediaCode::unavailable;
    });
}

wire::MediaResult load_media(const std::string& actor, const std::string& hash, bool lobby_visible,
    std::vector<uint8_t>& bytes, std::string* diagnostic) noexcept {
    bytes.clear();
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !wire::valid_sha256_hex(hash)) return MediaCode::invalid;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !media_lock_actor(connection, actor, diagnostic)) return MediaCode::unavailable;
        const auto access = media_access(connection, actor, hash, lobby_visible, diagnostic);
        if (access != MediaCode::ok) return access;
        auto statement = media_statement(connection, "SELECT data FROM media_assets8 WHERE hash=UNHEX(?)", {hash}, diagnostic, "读取图片内容");
        if (!statement) return MediaCode::unavailable;
        // Learn the length with an empty buffer first, then copy exactly that many
        // bytes: a small image never costs a 15 MiB allocation.
        MYSQL_BIND column{}; unsigned long length=0; bool null=false,truncated=false;
        column.buffer_type=MYSQL_TYPE_BLOB; column.buffer=nullptr; column.buffer_length=0;
        column.length=&length; column.is_null=&null; column.error=&truncated;
        if (mysql_stmt_bind_result(statement.get(), &column) != 0) {
            statement_failure(diagnostic,"绑定图片读取",statement.get(),media_schema); return MediaCode::unavailable;
        }
        const int fetched=mysql_stmt_fetch(statement.get());
        if ((fetched != 0 && fetched != MYSQL_DATA_TRUNCATED) || null || !length || length > wire::media_size_limit) {
            audit_failure(diagnostic,"读取图片内容","图片为空、超出限制或读取不完整"); return MediaCode::unavailable;
        }
        std::vector<uint8_t> candidate(length);
        column.buffer=candidate.data(); column.buffer_length=length; truncated=false;
        const unsigned long expected=length;
        if (mysql_stmt_fetch_column(statement.get(), &column, 0, 0) != 0 || truncated || length != expected) {
            audit_failure(diagnostic,"读取图片内容","图片为空、超出限制或读取不完整"); return MediaCode::unavailable;
        }
        // Drain the result before COMMIT on this connection.
        if (mysql_stmt_fetch(statement.get()) != MYSQL_NO_DATA || !transaction.commit()) return MediaCode::unavailable;
        bytes=std::move(candidate); return MediaCode::ok;
    }, media_data_timeout);
}

wire::MediaResult change_media_favorite(const std::string& actor, const std::string& hash,
    bool add, bool lobby_visible, std::string* diagnostic, const std::vector<uint8_t>* bytes) noexcept {
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !wire::valid_sha256_hex(hash) || (bytes && (bytes->empty() || bytes->size()>wire::media_size_limit)))
            return MediaCode::invalid;
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active() || !media_lock_actor(connection, actor, diagnostic)) return MediaCode::unavailable;
        if (!add) {
            auto statement=media_statement(connection,"DELETE FROM media_favorites8 WHERE accountID=? AND hash=UNHEX(?)",
                {actor,hash},diagnostic,"删除本人收藏");
            return statement && transaction.commit() ? MediaCode::ok : MediaCode::unavailable;
        }
        // A lobby image lives only in server memory and a private image may
        // already be collected: the caller's copy of the canonical bytes both
        // proves access and restores the asset, which the favourite then keeps.
        bool stored=false;
        if (!media_asset_locked(connection,hash,stored,diagnostic)) return MediaCode::unavailable;
        if (!stored && !bytes) return MediaCode::not_found;
        if (stored && !bytes) {
            const auto access=media_access(connection,actor,hash,lobby_visible,diagnostic);
            if (access!=MediaCode::ok) return access;
        }
        std::vector<SocialRow> count;
        if (!query_rows(media_schema,connection,"SELECT COUNT(*),COALESCE(SUM(hash=UNHEX(?)),0) FROM media_favorites8 WHERE accountID=?",
            {hash,actor},{20,20},1,count,diagnostic,"检查收藏额度")) return MediaCode::unavailable;
        uint64_t total=0, existing=0;
        if (count.size()!=1 || !social_uint64(count[0].values[0],total) || !social_uint64(count[0].values[1],existing)) return MediaCode::unavailable;
        if (!existing && total>=wire::favorite_limit) return MediaCode::limit;
        if (!stored) {
            const auto asset=media_insert_asset(connection,hash,*bytes,diagnostic);
            if (asset!=MediaCode::ok) return asset;
        }
        auto statement=media_statement(connection,"INSERT INTO media_favorites8(accountID,hash) VALUES(?,UNHEX(?)) ON DUPLICATE KEY UPDATE hash=hash",
            {actor,hash},diagnostic,"保存本人收藏");
        return statement && transaction.commit() ? MediaCode::ok : MediaCode::unavailable;
    }, bytes ? media_data_timeout : 1);
}

wire::MediaResult private_images(const std::string& actor, const std::string& peer, const std::vector<uint64_t>& ids,
    std::vector<PrivateImage>& images, std::string* diagnostic) noexcept {
    images.clear();
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor==peer || ids.size()>100) return MediaCode::invalid;
        const auto& low=actor<peer?actor:peer; const auto& high=actor<peer?peer:actor;
        SocialTransaction transaction(connection,diagnostic); if (!transaction.active()) return MediaCode::unavailable;
        const auto access=require_private_pair(connection,low,high,diagnostic);
        if (access!=SocialCode::ok) return access==SocialCode::not_found?MediaCode::not_found:MediaCode::unavailable;
        if (ids.empty()) return transaction.commit()?MediaCode::ok:MediaCode::unavailable;
        uint64_t through=0;
        if (!private_clear_watermark(connection,actor,peer,through,diagnostic)) return MediaCode::unavailable;
        std::vector<std::string> encoded; encoded.reserve(ids.size());
        for (auto id:ids) { if (!id) return MediaCode::invalid; encoded.push_back(std::to_string(id)); }
        const auto floor=std::to_string(through);
        std::vector<std::string_view> parameters{low,high,actor,peer,peer,actor,floor};
        std::string sql="SELECT m.id,LOWER(HEX(a.hash)) FROM private_messages8 m JOIN private_message_media8 a ON a.message_id=m.id "
            "WHERE m.account_low=? AND m.account_high=? AND ((m.sender_id=? AND m.recipient_id=?) OR "
            "(m.sender_id=? AND m.recipient_id=?)) AND m.recalled_at IS NULL AND m.id>? AND m.id IN (";
        for (const auto& id:encoded) { if (parameters.size()>7) sql+=','; sql+='?'; parameters.push_back(id); }
        sql+=") LIMIT 101";
        std::vector<SocialRow> rows;
        if (!query_rows(media_schema,connection,sql,parameters,{20,64},100,rows,diagnostic,"读取私聊图片关联")) return MediaCode::unavailable;
        std::vector<PrivateImage> candidate;
        for (auto& row:rows) {
            uint64_t id=0;
            if (!social_uint64(row.values[0],id) || !id || !wire::valid_sha256_hex(row.values[1])) return MediaCode::unavailable;
            bool requested=false; for (auto original:ids) if (original==id) requested=true;
            if (!requested) return MediaCode::unavailable;
            candidate.push_back({id,std::move(row.values[1])});
        }
        if (!transaction.commit()) return MediaCode::unavailable;
        images=std::move(candidate); return MediaCode::ok;
    });
}

wire::MediaResult send_private_image(const std::string& actor, const std::string& peer,
    const std::string& hash, const std::string& nonce, bool lobby_visible, bool allow_new,
    uint64_t& message_id, uint32_t& timestamp, bool& inserted, std::string* diagnostic) noexcept {
    message_id=0; timestamp=0; inserted=false;
    return media_database(diagnostic, [&](MYSQL* connection) {
        if (!social_id(actor) || !social_id(peer) || actor==peer || !wire::valid_sha256_hex(hash) || !wire::valid_nonce_hex(nonce)) return MediaCode::invalid;
        const auto& low=actor<peer?actor:peer; const auto& high=actor<peer?peer:actor;
        uint64_t legacy=0;
        if (!private_legacy_boundary(connection,legacy,diagnostic)) return MediaCode::unavailable;
        SocialTransaction transaction(connection,diagnostic); if (!transaction.active()) return MediaCode::unavailable;
        const auto participants=require_private_pair(connection,low,high,diagnostic);
        if (participants!=SocialCode::ok) return participants==SocialCode::not_found?MediaCode::not_found:MediaCode::unavailable;
        std::vector<SocialRow> prior;
        if (!query_rows(media_schema,connection,"SELECT m.id,m.recipient_id,m.body,LOWER(HEX(a.hash)),UNIX_TIMESTAMP(m.created_at) FROM private_messages8 m "
            "LEFT JOIN private_message_media8 a ON a.message_id=m.id WHERE m.sender_id=? AND m.nonce=UNHEX(?)",
            {actor,nonce},{20,64,480,64,20},1,prior,diagnostic,"检查图片消息防重复凭据")) return MediaCode::unavailable;
        if (!prior.empty()) {
            if (prior[0].nulls[3] || prior[0].values[1]!=peer || prior[0].values[2]!="[图片]" || prior[0].values[3]!=hash) return MediaCode::invalid;
            uint64_t saved=0,created=0;
            if (!social_uint64(prior[0].values[0],saved) || !saved || !social_uint64(prior[0].values[4],created) ||
                created>UINT32_MAX || !transaction.commit()) return MediaCode::unavailable;
            message_id=saved; timestamp=static_cast<uint32_t>(created); return MediaCode::ok;
        }
        bool delivered=false; uint64_t saved=0; uint32_t created=0;
        if (private_nonce_tombstone(connection,actor,nonce,delivered,saved,created,diagnostic)!=SocialCode::ok) return MediaCode::unavailable;
        if (delivered) {
            if (!transaction.commit()) return MediaCode::unavailable;
            message_id=saved; timestamp=created; return MediaCode::ok;
        }
        // A matching old nonce is only an acknowledgement of the existing row;
        // it must not create a new visible message or restore its media grant.
        const auto access=media_access(connection,actor,hash,lobby_visible,diagnostic,true); if (access!=MediaCode::ok) return access;
        if (!allow_new) return MediaCode::busy;
        if (private_insert(connection,low,high,actor,peer,nonce,"[图片]",hash,saved,created,diagnostic)!=SocialCode::ok ||
            !transaction.commit()) return MediaCode::unavailable;
        message_id=saved; timestamp=created; inserted=true; return MediaCode::ok;
    });
}

MediaCollect collect_media(const std::string& hash, uint32_t& wait_seconds, std::string* diagnostic) noexcept {
    wait_seconds = 0;
    return store_session<MediaCollect>(media_schema, {"初始化图片清理", "初始化图片清理线程",
        "设置图片清理安全会话", "清理图片"}, media_data_timeout, diagnostic, [&](MYSQL* connection) {
        if (!wire::valid_sha256_hex(hash)) return MediaCollect::kept;
        // No gap locks: the collector never blocks inserts of other images.
        constexpr char isolation[]="SET SESSION transaction_isolation='READ-COMMITTED'";
        if (mysql_real_query(connection,isolation,static_cast<unsigned long>(sizeof(isolation)-1))!=0) {
            connection_failure(diagnostic,"设置图片清理隔离级别",connection,media_schema); return MediaCollect::unavailable;
        }
        const auto busy=[](unsigned error){ return error==1205 || error==3572; };
        SocialTransaction transaction(connection, diagnostic);
        if (!transaction.active()) return MediaCollect::unavailable;
        // Every transaction that creates a reference holds this row (shared or
        // exclusive) until it commits, so the reference check below cannot miss
        // one that is being written, and a later one finds no asset.
        std::vector<SocialRow> rows; unsigned error=0;
        if (!media_rows(connection,"SELECT 1 FROM media_assets8 WHERE hash=UNHEX(?) FOR UPDATE",{hash},{1},1,rows,error,diagnostic,"锁定待清理图片"))
            return busy(error)?MediaCollect::busy:MediaCollect::unavailable;
        if (rows.empty()) return MediaCollect::gone;
        rows.clear();
        const auto grace=std::to_string(media_upload_grace_seconds);
        if (!media_rows(connection,"SELECT (EXISTS(SELECT 1 FROM media_grants8 WHERE hash=UNHEX(?)) "
            "OR EXISTS(SELECT 1 FROM private_message_media8 WHERE hash=UNHEX(?)) "
            "OR EXISTS(SELECT 1 FROM media_favorites8 WHERE hash=UNHEX(?))),"
            "COALESCE(TIMESTAMPDIFF(SECOND,NOW(),MAX(u.created_at)+INTERVAL CAST(? AS UNSIGNED) SECOND),-1) "
            "FROM media_uploads8 u WHERE u.hash=UNHEX(?)",{hash,hash,hash,grace,hash},{1,21},1,rows,error,diagnostic,"检查待清理图片引用"))
            return busy(error)?MediaCollect::busy:MediaCollect::unavailable;
        if (rows.size()!=1 || rows[0].nulls[0] || rows[0].nulls[1] || (rows[0].values[0]!="0" && rows[0].values[0]!="1")) {
            audit_failure(diagnostic,"检查待清理图片引用","引用检查结果无效"); return MediaCollect::unavailable;
        }
        if (rows[0].values[0]=="1") return transaction.commit()?MediaCollect::kept:MediaCollect::unavailable;
        const auto& remaining=rows[0].values[1];
        if (!remaining.empty() && remaining[0]!='-' && remaining!="0") {
            uint64_t seconds=0;
            if (!social_uint64(remaining,seconds)) { audit_failure(diagnostic,"检查待清理图片引用","上传时间无效"); return MediaCollect::unavailable; }
            wait_seconds=static_cast<uint32_t>((std::min<uint64_t>)(seconds,media_upload_grace_seconds));
            return transaction.commit()?MediaCollect::young:MediaCollect::unavailable;
        }
        rows.clear();
        if (!media_rows(connection,"DELETE FROM media_uploads8 WHERE hash=UNHEX(?)",{hash},{},0,rows,error,diagnostic,"删除已清理图片的上传记录") ||
            !media_rows(connection,"DELETE FROM media_assets8 WHERE hash=UNHEX(?)",{hash},{},0,rows,error,diagnostic,"删除已清理图片"))
            return busy(error)?MediaCollect::busy:MediaCollect::unavailable;
        return transaction.commit()?MediaCollect::deleted:MediaCollect::unavailable;
    });
}

bool media_collection_page(const std::string& after, size_t limit, MediaCollectionPage& out, std::string* diagnostic) noexcept {
    out = {};
    return media_database(diagnostic, [&](MYSQL* connection) {
        if ((!after.empty() && !wire::valid_sha256_hex(after)) || !limit || limit>1000) return MediaCode::invalid;
        // Plain reads only (no locks, never the image data); collect_media
        // re-checks every candidate under the asset lock.
        const auto grace=std::to_string(media_upload_grace_seconds);
        std::string sql="SELECT LOWER(HEX(a.hash)),a.created_at<NOW()-INTERVAL CAST(? AS UNSIGNED) SECOND "
            "AND NOT EXISTS(SELECT 1 FROM media_grants8 g WHERE g.hash=a.hash) "
            "AND NOT EXISTS(SELECT 1 FROM private_message_media8 p WHERE p.hash=a.hash) "
            "AND NOT EXISTS(SELECT 1 FROM media_favorites8 f WHERE f.hash=a.hash) "
            "AND NOT EXISTS(SELECT 1 FROM media_uploads8 u WHERE u.hash=a.hash AND u.created_at>NOW()-INTERVAL CAST(? AS UNSIGNED) SECOND) "
            "FROM media_assets8 a";
        std::vector<std::string_view> parameters{grace,grace};
        if (!after.empty()) { sql+=" WHERE a.hash>UNHEX(?)"; parameters.push_back(after); }
        sql+=" ORDER BY a.hash LIMIT "+std::to_string(limit);
        std::vector<SocialRow> rows;
        if (!query_rows(media_schema,connection,sql,parameters,{64,1},limit,rows,diagnostic,"查找可清理的图片")) return MediaCode::unavailable;
        MediaCollectionPage candidate;
        for (auto& row:rows) {
            if (row.nulls[0] || row.nulls[1] || !wire::valid_sha256_hex(row.values[0]) || row.values[0]<=candidate.last ||
                (row.values[1]!="0" && row.values[1]!="1")) {
                audit_failure(diagnostic,"查找可清理的图片","图片标识或清理条件无效"); return MediaCode::unavailable;
            }
            candidate.last=row.values[0];
            if (row.values[1]=="1") candidate.collectable.push_back(std::move(row.values[0]));
        }
        candidate.scanned=rows.size();
        out=std::move(candidate); return MediaCode::ok;
    }) == MediaCode::ok;
}

MediaCollection media_collection_enabled(std::string* diagnostic) noexcept {
    return store_session<MediaCollection>(media_schema, {"初始化图片清理", "初始化图片清理线程",
        "设置图片清理安全会话", "读取图片清理开关"}, 1, diagnostic, [&](MYSQL* connection) {
        std::vector<SocialRow> rows; unsigned error=0;
        if (!media_rows(connection,"SELECT enabled_after<=NOW() FROM media_collection8 WHERE id=1",{},{1},1,rows,error,diagnostic,"读取图片清理开关"))
            return error==1146 || error==1054 ? MediaCollection::missing : MediaCollection::unavailable;
        if (rows.empty() || rows[0].nulls[0]) return MediaCollection::paused;
        return rows[0].values[0]=="1" ? MediaCollection::enabled : MediaCollection::paused;
    });
}
