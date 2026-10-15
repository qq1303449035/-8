#include "MySQL.h"
#include "hero_permissions_parser.h"
#include "hero_ranking.h"

#include <Winsock2.h>
#include <Windows.h>
#include <mysql.h>
#include <array>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#pragma comment(lib, "libmysql.lib")

namespace chh {
namespace server {
namespace {

// Deployment overrides are read from CHH_DB_HOST/USER/PASSWORD/NAME/PORT.
// Keep the existing defaults private and never include them in diagnostics.
// The private defaults live in the untracked db_defaults.local.h.
#if __has_include("db_defaults.local.h")
#include "db_defaults.local.h"
#else
#include "db_defaults.example.h"
#endif

constexpr std::size_t kAccountLimit = 64;
constexpr std::size_t kPasswordLimit = 256;
constexpr std::size_t kMachineLimit = 64;
constexpr std::size_t kNameLimit = 512;
constexpr std::size_t kExpirationLength = 19;
constexpr std::size_t kSpecialLimit = 4096;
constexpr std::size_t kNotesLimit = 16384;

struct LibraryState {
    std::mutex mutex;
    std::condition_variable idle;
    bool attempted = false;
    bool initialized = false;
    bool stopping = false;
    std::size_t active_calls = 0;
};

LibraryState& library_state()
{
    static LibraryState state;
    return state;
}

class LibraryCall {
public:
    LibraryCall()
    {
        auto& state = library_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (state.initialized && !state.stopping) {
            ++state.active_calls;
            active_ = true;
        }
    }

    ~LibraryCall()
    {
        if (!active_)
            return;
        auto& state = library_state();
        std::lock_guard<std::mutex> lock(state.mutex);
        if (--state.active_calls == 0)
            state.idle.notify_all();
    }

    bool active() const { return active_; }
    LibraryCall(const LibraryCall&) = delete;
    LibraryCall& operator=(const LibraryCall&) = delete;

private:
    bool active_ = false;
};

class ThreadContext {
public:
    ThreadContext() : active_(mysql_thread_init() == 0) {}
    ~ThreadContext() { if (active_) mysql_thread_end(); }
    bool active() const { return active_; }
    ThreadContext(const ThreadContext&) = delete;
    ThreadContext& operator=(const ThreadContext&) = delete;

private:
    bool active_;
};

struct ConnectionDeleter {
    void operator()(MYSQL* connection) const { mysql_close(connection); }
};
struct StatementDeleter {
    void operator()(MYSQL_STMT* statement) const { mysql_stmt_close(statement); }
};
using Connection = std::unique_ptr<MYSQL, ConnectionDeleter>;
using Statement = std::unique_ptr<MYSQL_STMT, StatementDeleter>;

class WipeBuffer {
public:
    WipeBuffer(void* data, std::size_t size) : data_(data), size_(size) {}
    ~WipeBuffer() { if (data_ && size_) SecureZeroMemory(data_, size_); }
    WipeBuffer(const WipeBuffer&) = delete;
    WipeBuffer& operator=(const WipeBuffer&) = delete;

private:
    void* data_;
    std::size_t size_;
};

bool environment_or_default(const char* variable, const std::string& fallback,
    std::string& value)
{
    char* raw = nullptr;
    std::size_t length = 0;
    const auto result = _dupenv_s(&raw, &length, variable);
    std::unique_ptr<char, decltype(&std::free)> allocated(raw, &std::free);
    WipeBuffer wipe(raw, length);
    if (result != 0)
        return false;
    if (raw == nullptr)
        value = fallback;
    else
        value = raw;
    return true;
}

struct DatabaseConfig {
    std::string host;
    std::string user;
    std::string password;
    std::string name;
    unsigned int port = DB_PORT;

    ~DatabaseConfig()
    {
        if (!password.empty())
            SecureZeroMemory(&password[0], password.size());
    }
};

bool read_config(DatabaseConfig& config)
{
    std::string port;
    if (!environment_or_default("CHH_DB_HOST", DB_HOST, config.host) ||
        !environment_or_default("CHH_DB_USER", DB_USER, config.user) ||
        !environment_or_default("CHH_DB_PASSWORD", DB_PASS, config.password) ||
        !environment_or_default("CHH_DB_NAME", DB_NAME, config.name) ||
        !environment_or_default("CHH_DB_PORT", std::to_string(DB_PORT), port) ||
        config.host.empty() || config.user.empty() || config.name.empty() ||
        port.empty() || port.size() > 5)
        return false;

    unsigned int parsed = 0;
    for (char digit : port) {
        if (digit < '0' || digit > '9')
            return false;
        parsed = parsed * 10 + static_cast<unsigned int>(digit - '0');
    }
    if (parsed == 0 || parsed > 65535)
        return false;
    config.port = parsed;
    return true;
}

const char* database_error_reason(unsigned int error)
{
    switch (error) {
    case 1044: case 1045: case 1142: case 1143:
        return "数据库账号权限不足或身份验证失败";
    case 1049: return "指定的数据库不存在";
    case 1146: return "日志表不存在，请检查当前数据库中的userlogin8表";
    case 1054: return "日志表缺少所需字段";
    case 1364: return "日志表存在未提供值且没有默认值的必填字段";
    case 1406: return "日志表字段长度不足";
    case 1366: return "字段字符集或字段类型与写入内容不兼容";
    case 1062: return "主键或唯一索引冲突";
    case 1205: return "数据库锁等待超时";
    case 1213: return "数据库事务发生死锁";
    case 2006: case 2013: case 2055:
        return "数据库连接已断开或读取响应超时";
    case 2002: case 2003: return "无法连接数据库服务";
    case 2005: return "数据库服务器地址解析失败";
    case 1193: return "数据库版本不支持所需的会话变量";
    case 1231: case 1238: return "数据库不接受所需的会话变量设置";
    case 1036: case 1290: return "数据库或日志表当前不允许写入";
    case 1040: return "数据库连接数已达到限制";
    case 1129: case 1130: return "数据库服务拒绝当前主机连接";
    case 1452: return "日志表的外键约束不允许写入";
    case 3819: return "日志表的检查约束不允许写入";
    case 0: return "数据库客户端未提供具体错误码";
    default: return "数据库操作失败，请按错误码检查日志表结构和权限";
    }
}

std::string_view safe_error_field(unsigned int error, const char* raw) noexcept
{
    // Never return raw error text or duplicate-key values. Only these fixed
    // server templates may expose a validated schema identifier.
    std::string_view prefix;
    if (error == 1364)
        prefix = "Field '";
    else if (error == 1406)
        prefix = "Data too long for column '";
    else if (error == 1054)
        prefix = "Unknown column '";
    else
        return {};
    if (!raw)
        return {};
    const std::string_view message(raw);
    if (message.substr(0, prefix.size()) != prefix)
        return {};
    const auto remaining = message.substr(prefix.size());
    const auto quote = remaining.find('\'');
    if (quote == std::string_view::npos || quote == 0 || quote > 64)
        return {};
    const auto field = remaining.substr(0, quote);
    for (std::size_t index = 0; index < field.size(); ++index) {
        const char character = field[index];
        if (!((character >= 'a' && character <= 'z') ||
            (character >= 'A' && character <= 'Z') || character == '_' ||
            (index != 0 && character >= '0' && character <= '9')))
            return {};
    }
    const auto suffix = remaining.substr(quote);
    if (error == 1364 && suffix != "' doesn't have a default value")
        return {};
    if (error == 1054 && suffix != "' in 'field list'")
        return {};
    if (error == 1406) {
        constexpr std::string_view row_prefix = "' at row ";
        if (suffix.substr(0, row_prefix.size()) != row_prefix)
            return {};
        const auto row = suffix.substr(row_prefix.size());
        if (row.empty() || row.size() > 20)
            return {};
        for (char digit : row) {
            if (digit < '0' || digit > '9')
                return {};
        }
    }
    return field;
}

bool audit_failure(std::string* diagnostic, const char* stage,
    const char* reason, unsigned int error = 0,
    const char* raw = nullptr, bool database_error = false) noexcept
{
    if (diagnostic) {
        try {
            std::string message = std::string("阶段：") + stage + "；原因：" + reason;
            if (database_error)
                message += "；MySQL错误码：" + std::to_string(error);
            const auto field = safe_error_field(error, raw);
            if (!field.empty()) {
                message += "；字段：";
                message.append(field.data(), field.size());
            }
            *diagnostic = std::move(message);
        }
        catch (...) {
            // Reporting an allocation failure must not throw from the logger.
            diagnostic->clear();
        }
    }
    return false;
}

std::string_view safe_connection_system_error(unsigned int error,
    const char* raw) noexcept
{
    if ((error != 2002 && error != 2003) || !raw)
        return {};
    const std::string_view message(raw);
    if (message.size() < 4 || message.back() != ')')
        return {};
    const auto open = message.rfind(" (");
    if (open == std::string_view::npos)
        return {};
    const auto digits = message.substr(open + 2, message.size() - open - 3);
    if (digits.empty() || digits.size() > 10)
        return {};
    unsigned int code = 0;
    for (char digit : digits) {
        if (digit < '0' || digit > '9')
            return {};
        const unsigned int value = static_cast<unsigned int>(digit - '0');
        if (code > (2147483647u - value) / 10u)
            return {};
        code = code * 10u + value;
    }
    return code == 0 ? std::string_view{} : digits;
}

// How one store explains schema errors and binds its statement parameters.
// A null reason for a missing table (1146) or field (1054) uses the shared
// reason; field_names allows the validated identifier of safe_error_field.
struct StoreSchema {
    const char* missing_table;
    const char* missing_field;
    bool field_names;
    enum_field_types binding;
};
constexpr StoreSchema login_schema{nullptr, nullptr, true, MYSQL_TYPE_STRING};

bool schema_failure(std::string* diagnostic, const char* stage,
    unsigned int error, const char* raw, const StoreSchema& schema) noexcept
{
    const char* reason = error == 1146 && schema.missing_table ? schema.missing_table :
        error == 1054 && schema.missing_field ? schema.missing_field :
        database_error_reason(error);
    return audit_failure(diagnostic, stage, reason, error,
        schema.field_names ? raw : nullptr, true);
}

bool connection_failure(std::string* diagnostic, const char* stage,
    MYSQL* connection, const StoreSchema& schema = login_schema) noexcept
{
    const auto error = mysql_errno(connection);
    const char* raw = (error == 1364 || error == 1406 || error == 1054) ?
        mysql_error(connection) : nullptr;
    return schema_failure(diagnostic, stage, error, raw, schema);
}

bool statement_failure(std::string* diagnostic, const char* stage,
    MYSQL_STMT* statement, const StoreSchema& schema = login_schema) noexcept
{
    const auto error = mysql_stmt_errno(statement);
    const char* raw = (error == 1364 || error == 1406 || error == 1054) ?
        mysql_stmt_error(statement) : nullptr;
    return schema_failure(diagnostic, stage, error, raw, schema);
}

Connection open_database(const DatabaseConfig& config, unsigned int timeout,
    std::string* diagnostic)
{
    Connection connection(mysql_init(nullptr));
    if (!connection) {
        audit_failure(diagnostic, "创建数据库连接", "数据库连接对象初始化失败");
        return Connection();
    }
    if (mysql_options(connection.get(), MYSQL_OPT_CONNECT_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection.get(), MYSQL_OPT_READ_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection.get(), MYSQL_OPT_WRITE_TIMEOUT, &timeout) != 0 ||
        mysql_options(connection.get(), MYSQL_SET_CHARSET_NAME, "utf8mb4") != 0) {
        connection_failure(diagnostic, "设置连接超时和字符集", connection.get());
        return Connection();
    }
    const auto connect_started = std::chrono::steady_clock::now();
    if (mysql_real_connect(connection.get(), config.host.c_str(),
            config.user.c_str(), config.password.c_str(), config.name.c_str(),
            config.port, nullptr, 0) == nullptr) {
        const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - connect_started).count();
        connection_failure(diagnostic, "连接数据库", connection.get());
        if (diagnostic && !diagnostic->empty()) {
            try {
                std::string message = *diagnostic;
                message += "；连接耗时：" + std::to_string(elapsed) + "毫秒";
                message += "；连接超时配置：" + std::to_string(timeout) + "秒";
                const auto system_error = safe_connection_system_error(
                    mysql_errno(connection.get()), mysql_error(connection.get()));
                if (!system_error.empty()) {
                    message += "；系统错误码：";
                    message.append(system_error.data(), system_error.size());
                }
                *diagnostic = std::move(message);
            }
            catch (...) {
                // Preserve the base diagnostic if adding timing details fails.
            }
        }
        return Connection();
    }
    if (mysql_set_character_set(connection.get(), "utf8mb4") != 0) {
        connection_failure(diagnostic, "设置数据库连接字符集", connection.get());
        return Connection();
    }
    return connection;
}

// A lease belongs to one operation/thread. Statements and transactions must be
// destroyed before the lease, then ThreadContext, then LibraryCall.
struct PoolReturn {
    std::shared_ptr<const DatabaseConfig> config;
    unsigned int timeout = 5;
    void operator()(MYSQL* connection) const noexcept;
};
using PooledConnection = std::unique_ptr<MYSQL, PoolReturn>;

class DatabasePool {
public:
    static constexpr size_t capacity = 32;
    PooledConnection acquire(std::shared_ptr<const DatabaseConfig> config,
        unsigned int timeout, std::string* diagnostic) {
        Connection connection;
        std::unique_lock<std::mutex> lock(mutex_);
        const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(timeout);
        for (;;) {
            if (stopping_) {
                audit_failure(diagnostic, "借用数据库连接", "数据库连接池正在停止");
                return {};
            }
            size_t match = idle_count_;
            for (size_t index = 0; index < idle_count_; ++index) {
                const auto& idle = idle_[index];
                if (idle.timeout == timeout && same_config(*idle.config, *config)) {
                    match = index;
                    break;
                }
            }
            if (match != idle_count_) {
                connection = std::move(idle_[match].connection);
                idle_[match] = std::move(idle_[--idle_count_]);
                idle_[idle_count_] = {};
                break;
            }
            if (total_ < capacity) {
                ++total_; // Reserve before connecting, including concurrent creators.
                break;
            }
            if (idle_count_) {
                // Socket timeouts are connect-time options. Replace an idle
                // connection of another timeout/config rather than changing it.
                connection = std::move(idle_[--idle_count_].connection);
                idle_[idle_count_] = {};
                lock.unlock();
                connection.reset();
                lock.lock();
                break; // Keep its reservation while creating the replacement.
            }
            if (!available_.wait_until(lock, deadline,
                    [&] { return stopping_ || idle_count_ || total_ < capacity; })) {
                audit_failure(diagnostic, "等待数据库连接", "数据库连接池繁忙，等待超时");
                return {};
            }
        }
        lock.unlock();
        struct Reservation {
            DatabasePool& pool;
            bool active = true;
            ~Reservation() { if (active) pool.discard_slot(); }
        } reservation{*this};
        // Check only before business SQL. A lost connection is replaced here;
        // a failed write/commit is never replayed automatically.
        if (connection && mysql_ping(connection.get()) != 0) connection.reset();
        if (!connection) connection = open_database(*config, timeout, diagnostic);
        if (!connection) return {};
        reservation.active = false;
        return PooledConnection(connection.release(), PoolReturn{std::move(config), timeout});
    }

    void release(MYSQL* raw, std::shared_ptr<const DatabaseConfig> config,
        unsigned int timeout) noexcept {
        Connection connection(raw);
        bool reusable = false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            reusable = !stopping_;
        }
        // Reset rolls back unfinished work and releases GET_LOCK, temporary
        // tables, prepared statements, LAST_INSERT_ID and session variables.
        // In particular collect_media's READ-COMMITTED must not escape its call.
        // No network I/O is performed while holding the pool mutex.
        reusable = reusable && mysql_reset_connection(connection.get()) == 0 &&
            mysql_set_character_set(connection.get(), "utf8mb4") == 0;
        if (reusable) {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!stopping_) {
                idle_[idle_count_++] = {std::move(connection), std::move(config), timeout};
                available_.notify_one();
                return;
            }
        }
        connection.reset();
        discard_slot();
    }

    void request_stop() {
        std::lock_guard<std::mutex> lock(mutex_);
        stopping_ = true;
        available_.notify_all();
    }
    // shutdown_database has blocked new LibraryCalls and waited for all leases.
    void close_idle() {
        std::array<Idle, capacity> retired;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            retired.swap(idle_);
            total_ -= idle_count_;
            idle_count_ = 0;
        }
        // Destruction closes the connections before mysql_library_end().
    }
private:
    struct Idle {
        Connection connection;
        std::shared_ptr<const DatabaseConfig> config;
        unsigned int timeout = 0;
    };
    static bool same_config(const DatabaseConfig& a, const DatabaseConfig& b) {
        return a.port == b.port && a.host == b.host && a.user == b.user &&
            a.name == b.name && a.password == b.password;
    }
    void discard_slot() noexcept {
        std::lock_guard<std::mutex> lock(mutex_);
        --total_;
        available_.notify_one();
    }
    std::mutex mutex_;
    std::condition_variable available_;
    std::array<Idle, capacity> idle_{};
    size_t idle_count_ = 0, total_ = 0;
    bool stopping_ = false;
};

DatabasePool& database_pool() {
    static DatabasePool pool;
    return pool;
}
void PoolReturn::operator()(MYSQL* connection) const noexcept {
    database_pool().release(connection, config, timeout);
}
PooledConnection connect_database(unsigned int timeout = 5,
    std::string* diagnostic = nullptr) {
    auto config = std::make_shared<DatabaseConfig>();
    if (!read_config(*config)) {
        audit_failure(diagnostic, "读取数据库配置", "数据库配置缺失或端口配置无效");
        return {};
    }
    return database_pool().acquire(std::move(config), timeout, diagnostic);
}

// Strict mode rejects too-small columns instead of silently storing truncated
// values, including with a nontransactional table. Preserve every mode already
// configured on this fresh connection; lock waits end after one second.
bool set_strict_session(MYSQL* connection) noexcept
{
    constexpr char query[] =
        "SET SESSION sql_mode=CONCAT_WS(',',NULLIF(@@SESSION.sql_mode,''),'STRICT_ALL_TABLES'),"
        "innodb_lock_wait_timeout=1,lock_wait_timeout=1";
    return mysql_real_query(connection, query,
        static_cast<unsigned long>(sizeof(query) - 1)) == 0;
}

// Prepares, binds every value with the store's buffer type and executes.
// A failure is reported under the caller's stage and returns no statement.
Statement store_statement(const StoreSchema& schema, MYSQL* connection,
    std::string_view sql, const std::vector<std::string_view>& values,
    std::string* diagnostic, const char* stage)
{
    Statement statement(mysql_stmt_init(connection));
    if (!statement) {
        connection_failure(diagnostic, stage, connection, schema);
        return {};
    }
    if (mysql_stmt_prepare(statement.get(), sql.data(),
            static_cast<unsigned long>(sql.size())) != 0) {
        statement_failure(diagnostic, stage, statement.get(), schema);
        return {};
    }
    std::vector<unsigned long> lengths(values.size());
    std::vector<MYSQL_BIND> parameters(values.size());
    for (std::size_t index = 0; index < values.size(); ++index) {
        lengths[index] = static_cast<unsigned long>(values[index].size());
        parameters[index].buffer_type = schema.binding;
        parameters[index].buffer = const_cast<char*>(values[index].data());
        parameters[index].buffer_length = lengths[index];
        parameters[index].length = &lengths[index];
    }
    if ((!values.empty() && mysql_stmt_bind_param(statement.get(), parameters.data()) != 0) ||
        mysql_stmt_execute(statement.get()) != 0) {
        statement_failure(diagnostic, stage, statement.get(), schema);
        return {};
    }
    return statement;
}

// Stage names of one store's session setup; the reasons are shared.
struct SessionStages {
    const char* library;
    const char* thread;
    const char* session;
    const char* failure;
};

// Runs operation on an exclusive pooled lease in strict autocommit mode.
// Every setup failure or exception yields Result::unavailable.
template<class Result, class Operation>
Result store_session(const StoreSchema& schema, const SessionStages& stages,
    unsigned int timeout, std::string* diagnostic, Operation operation) noexcept
{
    if (diagnostic)
        diagnostic->clear();
    try {
        LibraryCall library;
        if (!library.active()) {
            audit_failure(diagnostic, stages.library, "数据库客户端库未初始化或正在停止");
            return Result::unavailable;
        }
        ThreadContext thread;
        if (!thread.active()) {
            audit_failure(diagnostic, stages.thread, "数据库线程环境初始化失败");
            return Result::unavailable;
        }
        auto connection = connect_database(timeout, diagnostic);
        if (!connection)
            return Result::unavailable;
        if (mysql_autocommit(connection.get(), true) != 0 ||
            !set_strict_session(connection.get())) {
            connection_failure(diagnostic, stages.session, connection.get(), schema);
            return Result::unavailable;
        }
        return operation(connection.get());
    }
    catch (...) {
        audit_failure(diagnostic, stages.failure, "发生本地异常或内存不足");
        return Result::unavailable;
    }
}

bool valid_input(const std::string& value, std::size_t limit, bool required)
{
    return (!required || !value.empty()) && value.size() <= limit &&
        value.find('\0') == std::string::npos;
}

bool valid_audit_input(const std::string& value, std::size_t limit,
    const char* stage, std::string* diagnostic)
{
    if (value.size() > limit)
        return audit_failure(diagnostic, stage, "字段长度超过程序允许的字节范围");
    if (value.find('\0') != std::string::npos)
        return audit_failure(diagnostic, stage, "字段包含不允许的空字符");
    return true;
}

bool has_ban_permission(std::string_view permissions)
{
    std::size_t begin = 0;
    while ((begin = permissions.find_first_not_of(permission_whitespace, begin)) !=
        std::string_view::npos) {
        const auto end = permissions.find_first_of(permission_whitespace, begin);
        const auto length = (end == std::string_view::npos ?
            permissions.size() : end) - begin;
        if (length == 1 && permissions[begin] == '0')
            return true;
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return false;
}

} // namespace

bool initialize_database()
{
    auto& state = library_state();
    std::lock_guard<std::mutex> lock(state.mutex);
    if (state.attempted || state.stopping)
        return state.initialized && !state.stopping;
    state.attempted = true;
    state.initialized = mysql_library_init(0, nullptr, nullptr) == 0;
    // mysql_library_init also initializes its calling thread. Authentication
    // manages its own thread state, so release this startup-only state now.
    if (state.initialized)
        mysql_thread_end();
    return state.initialized;
}

void shutdown_database()
{
    auto& state = library_state();
    std::unique_lock<std::mutex> lock(state.mutex);
    state.stopping = true;
    database_pool().request_stop();
    state.idle.wait(lock, [&state] { return state.active_calls == 0; });
    if (state.initialized) {
        {
            ThreadContext thread;
            database_pool().close_idle();
        }
        mysql_library_end();
        state.initialized = false;
    }
}

bool lobby_image_memory_limit(std::size_t& bytes, std::string* error)
{
    // Whole MiB only, so a typo such as "256M" or "0.5" is refused instead of
    // silently becoming another limit.
    std::string value;
    unsigned long megabytes = 0;
    bool valid = environment_or_default("CHH_LOBBY_IMAGE_MEMORY_MB", "256", value) &&
        !value.empty() && value.size() <= 4;
    for (std::size_t index = 0; valid && index < value.size(); ++index) {
        if (value[index] < '0' || value[index] > '9')
            valid = false;
        else
            megabytes = megabytes * 10 + static_cast<unsigned long>(value[index] - '0');
    }
    if (!valid || megabytes < 1 || megabytes > 4096) {
        if (error)
            *error = "CHH_LOBBY_IMAGE_MEMORY_MB无效：请填写1至4096之间的整数（MiB），默认256";
        return false;
    }
    bytes = static_cast<std::size_t>(megabytes) << 20;
    return true;
}

bool latest_app_version(std::string& out, std::string* diagnostic) noexcept
{
    out.clear();
    if (diagnostic)
        diagnostic->clear();
    try {
        LibraryCall library_call;
        if (!library_call.active())
            return audit_failure(diagnostic, "读取软件版本", "数据库客户端尚未就绪");
        ThreadContext thread_context;
        if (!thread_context.active())
            return audit_failure(diagnostic, "读取软件版本", "数据库线程初始化失败");
        auto connection = connect_database(5, diagnostic);
        if (!connection)
            return false;
        Statement statement(mysql_stmt_init(connection.get()));
        if (!statement)
            return connection_failure(diagnostic, "创建版本查询", connection.get());
        constexpr char query[] = "SELECT `version` FROM `appdata` LIMIT 2";
        if (mysql_stmt_prepare(statement.get(), query,
                static_cast<unsigned long>(sizeof(query) - 1)) != 0 ||
            mysql_stmt_execute(statement.get()) != 0)
            return statement_failure(diagnostic, "查询软件版本", statement.get());

        std::array<char, wire::app_version_limit + 1> version{};
        unsigned long length = 0;
        bool is_null = false, truncated = false;
        MYSQL_BIND column{};
        column.buffer_type = MYSQL_TYPE_STRING;
        column.buffer = version.data();
        column.buffer_length = static_cast<unsigned long>(version.size());
        column.length = &length;
        column.is_null = &is_null;
        column.error = &truncated;
        if (mysql_stmt_bind_result(statement.get(), &column) != 0)
            return statement_failure(diagnostic, "读取软件版本字段", statement.get());
        const int fetched = mysql_stmt_fetch(statement.get());
        if (fetched == MYSQL_NO_DATA)
            return audit_failure(diagnostic, "读取软件版本", "版本配置为空，必须恰好包含一条记录");
        if (fetched == MYSQL_DATA_TRUNCATED || truncated || length > wire::app_version_limit)
            return audit_failure(diagnostic, "读取软件版本", "版本字段超过允许长度");
        if (fetched != 0)
            return statement_failure(diagnostic, "读取软件版本", statement.get());
        if (is_null || !wire::valid_app_version(std::string_view(version.data(), length)))
            return audit_failure(diagnostic, "读取软件版本", "版本必须是四段数字且不能为空");
        const int next = mysql_stmt_fetch(statement.get());
        if (next == 0 || next == MYSQL_DATA_TRUNCATED)
            return audit_failure(diagnostic, "读取软件版本", "版本配置包含多条记录");
        if (next != MYSQL_NO_DATA)
            return statement_failure(diagnostic, "检查版本记录数量", statement.get());
        out.assign(version.data(), length);
        return true;
    }
    catch (...) {
        out.clear();
        return audit_failure(diagnostic, "读取软件版本", "版本查询发生异常");
    }
}

bool latest_update_content(const std::string& requested_version, std::string& out) noexcept
{
    out.clear();
    if (!wire::valid_app_version(requested_version))
        return false;
    try {
        LibraryCall library_call;
        if (!library_call.active()) return false;
        ThreadContext thread_context;
        if (!thread_context.active()) return false;
        auto connection = connect_database(1);
        if (!connection) return false;
        Statement statement(mysql_stmt_init(connection.get()));
        if (!statement) return false;
        // Read version and notes atomically from the sole configuration row.
        // Bound the database response itself, retaining one extra byte to
        // detect oversize fields without ever accepting partial UTF-8 text.
        constexpr char query[] =
            "SELECT LEFT(CAST(`version` AS BINARY),33),"
            "LEFT(CAST(CONVERT(`updateContent` USING utf8mb4) AS BINARY),16385) "
            "FROM `appdata` LIMIT 2";
        static_assert(wire::app_version_limit == 32 && wire::update_notes_limit == 16384);
        if (mysql_stmt_prepare(statement.get(), query,
                static_cast<unsigned long>(sizeof(query) - 1)) != 0 ||
            mysql_stmt_execute(statement.get()) != 0) return false;
        std::array<char, wire::app_version_limit + 1> version{};
        std::array<char, wire::update_notes_limit + 1> content{};
        std::array<unsigned long, 2> length{};
        std::array<bool, 2> is_null{}, truncated{};
        std::array<MYSQL_BIND, 2> columns{};
        for (size_t index = 0; index < columns.size(); ++index) {
            columns[index].buffer_type = MYSQL_TYPE_STRING;
            columns[index].length = &length[index];
            columns[index].is_null = &is_null[index];
            columns[index].error = &truncated[index];
        }
        columns[0].buffer = version.data();
        columns[0].buffer_length = static_cast<unsigned long>(version.size());
        columns[1].buffer = content.data();
        columns[1].buffer_length = static_cast<unsigned long>(content.size());
        if (mysql_stmt_bind_result(statement.get(), columns.data()) != 0 ||
            mysql_stmt_fetch(statement.get()) != 0 || is_null[0] || is_null[1] ||
            truncated[0] || truncated[1] || length[0] > wire::app_version_limit ||
            length[1] > wire::update_notes_limit || !length[1]) return false;
        const std::string_view stored_version(version.data(), length[0]);
        const std::string_view stored_content(content.data(), length[1]);
        if (stored_version != requested_version ||
            !wire::valid_update_notes_content(stored_content)) return false;
        // Multiple appdata rows have the same fail-closed semantics as login's
        // version query. A second fetch must not replace the accepted content.
        std::string result(stored_content);
        if (mysql_stmt_fetch(statement.get()) != MYSQL_NO_DATA) return false;
        out = std::move(result);
        return true;
    }
    catch (...) {
        out.clear();
        return false;
    }
}

AuthResult authenticate_account(const std::string& id,
    const std::string& password, const std::string& machine,
    AccountProfile& out, AuthAuditInfo* audit)
{
    out = AccountProfile{};
    if (audit)
        *audit = AuthAuditInfo{};
    if (!valid_input(id, kAccountLimit, true))
        return AuthResult::account_not_found;
    if (!valid_input(password, kPasswordLimit, true))
        return AuthResult::password_mismatch;
    if (!valid_input(machine, kMachineLimit, false))
        return AuthResult::machine_mismatch;

    try {
        LibraryCall library_call;
        if (!library_call.active())
            return AuthResult::unavailable;
        ThreadContext thread_context;
        if (!thread_context.active())
            return AuthResult::unavailable;
        auto connection = connect_database();
        if (!connection)
            return AuthResult::unavailable;
        Statement statement(mysql_stmt_init(connection.get()));
        if (!statement)
            return AuthResult::unavailable;

        // Database time decides expiry; password and machine comparison stay
        // case-sensitive in C++, independent of the table's collation.
        constexpr char query[] =
            "SELECT `accountID`, `password`, `machineCode`, `accountName`, "
            "DATE_FORMAT(`ExpirationTime`, '%Y-%m-%d %H:%i:%s'), `special`, "
            "COALESCE(`ExpirationTime` > CURRENT_TIMESTAMP, 0), `notes` "
            "FROM `user8` WHERE `accountID` = ? LIMIT 2";
        if (mysql_stmt_prepare(statement.get(), query,
                static_cast<unsigned long>(sizeof(query) - 1)) != 0)
            return AuthResult::unavailable;

        MYSQL_BIND parameter{};
        unsigned long account_length = static_cast<unsigned long>(id.size());
        parameter.buffer_type = MYSQL_TYPE_STRING;
        parameter.buffer = const_cast<char*>(id.data());
        parameter.buffer_length = account_length;
        parameter.length = &account_length;
        if (mysql_stmt_bind_param(statement.get(), &parameter) != 0 ||
            mysql_stmt_execute(statement.get()) != 0)
            return AuthResult::unavailable;

        std::array<char, kAccountLimit + 1> account{};
        std::array<char, kPasswordLimit + 1> stored_password{};
        WipeBuffer wipe_password(stored_password.data(), stored_password.size());
        std::array<char, kMachineLimit + 1> stored_machine{};
        std::array<char, kNameLimit + 1> name{};
        std::array<char, kExpirationLength + 1> expiration{};
        std::array<char, kSpecialLimit + 1> special{};
        std::array<char, 2> valid_expiration{};
        std::array<char, kNotesLimit + 1> notes{};
        std::array<void*, 8> buffers{{account.data(), stored_password.data(),
            stored_machine.data(), name.data(), expiration.data(),
            special.data(), valid_expiration.data(), notes.data()}};
        const std::array<unsigned long, 8> limits{{
            static_cast<unsigned long>(kAccountLimit),
            static_cast<unsigned long>(kPasswordLimit),
            static_cast<unsigned long>(kMachineLimit),
            static_cast<unsigned long>(kNameLimit),
            static_cast<unsigned long>(kExpirationLength),
            static_cast<unsigned long>(kSpecialLimit), 1,
            static_cast<unsigned long>(kNotesLimit)}};
        std::array<unsigned long, 8> lengths{};
        std::array<bool, 8> nulls{};
        std::array<bool, 8> errors{};
        std::array<MYSQL_BIND, 8> columns{};
        for (std::size_t index = 0; index < columns.size(); ++index) {
            auto& column = columns[index];
            column.buffer_type = MYSQL_TYPE_STRING;
            column.buffer = buffers[index];
            column.buffer_length = limits[index] + 1;
            column.length = &lengths[index];
            column.is_null = &nulls[index];
            column.error = &errors[index];
        }
        if (mysql_stmt_bind_result(statement.get(), columns.data()) != 0)
            return AuthResult::unavailable;
        const int fetched = mysql_stmt_fetch(statement.get());
        if (fetched == MYSQL_NO_DATA)
            return AuthResult::account_not_found;
        if (fetched != 0 && fetched != MYSQL_DATA_TRUNCATED)
            return AuthResult::unavailable;
        for (std::size_t index = 0; index < 7; ++index) {
            if (errors[index] || lengths[index] > limits[index])
                return AuthResult::unavailable;
        }
        const bool notes_truncated = errors[7] || lengths[7] > limits[7];
        if (fetched == MYSQL_DATA_TRUNCATED && !notes_truncated)
            return AuthResult::unavailable;
        // Notes belong only to the audit record; malformed notes must not
        // change the credential or account-state result.
        const bool notes_valid = !notes_truncated &&
            (nulls[7] || std::memchr(notes.data(), '\0', lengths[7]) == nullptr);

        // Check uniqueness before choosing a credential or account-state error.
        // MYSQL_NO_DATA leaves the first row's bound buffers intact.
        if (mysql_stmt_fetch(statement.get()) != MYSQL_NO_DATA)
            return AuthResult::unavailable;
        if (nulls[0] || nulls[1] || lengths[0] == 0 || lengths[1] == 0 ||
            nulls[6] || lengths[6] != 1 ||
            (valid_expiration[0] != '0' && valid_expiration[0] != '1'))
            return AuthResult::unavailable;
        if (std::memchr(account.data(), '\0', lengths[0]) != nullptr ||
            std::memchr(stored_password.data(), '\0', lengths[1]) != nullptr ||
            (!nulls[2] && std::memchr(stored_machine.data(), '\0', lengths[2]) != nullptr) ||
            (!nulls[3] && std::memchr(name.data(), '\0', lengths[3]) != nullptr) ||
            (!nulls[5] && std::memchr(special.data(), '\0', lengths[5]) != nullptr))
            return AuthResult::unavailable;
        const std::string canonical_id(account.data(), lengths[0]);
        if (canonical_id != id)
            return AuthResult::account_not_found;
        if (lengths[1] != password.size() ||
            std::memcmp(stored_password.data(), password.data(), lengths[1]) != 0)
            return AuthResult::password_mismatch;
        if (audit) {
            AuthAuditInfo verified;
            if (!nulls[3])
                verified.account_name.assign(name.data(), lengths[3]);
            if (notes_valid && !nulls[7])
                verified.notes.assign(notes.data(), lengths[7]);
            verified.credentials_verified = true;
            verified.metadata_valid = notes_valid;
            *audit = std::move(verified);
        }
        if (!nulls[5] && has_ban_permission(
                std::string_view(special.data(), lengths[5])))
            return AuthResult::banned;
        if (!nulls[2] && lengths[2] != 0 &&
            (lengths[2] != machine.size() ||
                std::memcmp(stored_machine.data(), machine.data(), lengths[2]) != 0))
            return AuthResult::machine_mismatch;
        if (nulls[4] || valid_expiration[0] == '0')
            return AuthResult::expired;
        if (nulls[3] || lengths[3] == 0 || lengths[4] != kExpirationLength)
            return AuthResult::unavailable;
        AccountProfile candidate{canonical_id,
            std::string(name.data(), lengths[3]),
            std::string(expiration.data(), lengths[4])};
        if (!valid_input(candidate.name, kNameLimit, true))
            return AuthResult::unavailable;
        out = std::move(candidate);
        return AuthResult::ok;
    }
    catch (...) {
        out = AccountProfile{};
        if (audit)
            *audit = AuthAuditInfo{};
        return AuthResult::unavailable;
    }
}

bool record_login(const std::string& id, const std::string& password_value,
    const std::string& machine, const AuthAuditInfo& audit,
    AuthResult result, bool primary_client, std::string* diagnostic) noexcept
{
    if (diagnostic)
        diagnostic->clear();
    try {
        const char* outcome = nullptr;
        switch (result) {
        case AuthResult::account_not_found: outcome = "账号不存在"; break;
        case AuthResult::password_mismatch: outcome = "密码错误"; break;
        case AuthResult::machine_mismatch: outcome = "机器码错误"; break;
        case AuthResult::expired: outcome = "茶会已过期"; break;
        case AuthResult::banned: outcome = "茶会已封禁"; break;
        case AuthResult::ok: outcome = "登入成功"; break;
        case AuthResult::update_required: outcome = "版本不一致，请更新"; break;
        default:
            return audit_failure(diagnostic, "检查登录结果", "该登录结果不生成审计记录");
        }
        if (audit.credentials_verified && !audit.metadata_valid)
            return audit_failure(diagnostic, "检查账号备注",
                "账号备注超长、被截断或含空字符，已拒绝不完整的日志记录");
        if (!valid_audit_input(id, kAccountLimit, "检查账号字段", diagnostic) ||
            !valid_audit_input(password_value, kPasswordLimit, "检查密码字段", diagnostic) ||
            !valid_audit_input(machine, kMachineLimit, "检查机器码字段", diagnostic))
            return false;
        const std::string empty;
        const auto& name = audit.credentials_verified ? audit.account_name : empty;
        const auto& notes = audit.credentials_verified ? audit.notes : empty;
        if (!valid_audit_input(name, kNameLimit, "检查账号名称字段", diagnostic) ||
            !valid_audit_input(notes, kNotesLimit, "检查备注字段", diagnostic))
            return false;
        const std::string explanation =
            std::string(primary_client ? "客户端:" : "游戏端:") + outcome;

        LibraryCall library_call;
        if (!library_call.active())
            return audit_failure(diagnostic, "检查数据库客户端库",
                "MySQL客户端库未初始化或正在停止");
        ThreadContext thread_context;
        if (!thread_context.active())
            return audit_failure(diagnostic, "初始化数据库线程",
                "MySQL线程环境初始化失败");
        auto connection = connect_database(1, diagnostic);
        if (!connection)
            return false;

        // Strict mode keeps truncated audit records out of the log table.
        if (mysql_autocommit(connection.get(), true) != 0)
            return connection_failure(diagnostic, "启用日志自动提交", connection.get());
        if (!set_strict_session(connection.get()))
            return connection_failure(diagnostic,
                "设置日志严格模式和锁等待上限", connection.get());

        Statement statement(mysql_stmt_init(connection.get()));
        if (!statement)
            return connection_failure(diagnostic, "创建日志预处理语句", connection.get());
        constexpr char query[] =
            "INSERT INTO `userlogin8` (`accountID`, `password`, `machineCode`, "
            "`LoginTime`, `accountName`, `notes`, `Explanation`) "
            "VALUES (?, ?, ?, CURRENT_TIMESTAMP, ?, ?, ?)";
        if (mysql_stmt_prepare(statement.get(), query,
                static_cast<unsigned long>(sizeof(query) - 1)) != 0)
            return statement_failure(diagnostic, "准备日志插入语句", statement.get());
        const std::array<const std::string*, 6> values{{
            &id, &password_value, &machine, &name, &notes, &explanation}};
        std::array<unsigned long, 6> lengths{};
        std::array<MYSQL_BIND, 6> parameters{};
        for (std::size_t index = 0; index < parameters.size(); ++index) {
            lengths[index] = static_cast<unsigned long>(values[index]->size());
            auto& parameter = parameters[index];
            parameter.buffer_type = MYSQL_TYPE_STRING;
            parameter.buffer = const_cast<char*>(values[index]->data());
            parameter.buffer_length = lengths[index];
            parameter.length = &lengths[index];
        }
        if (mysql_stmt_bind_param(statement.get(), parameters.data()) != 0)
            return statement_failure(diagnostic, "绑定日志字段", statement.get());
        if (mysql_stmt_execute(statement.get()) != 0)
            return statement_failure(diagnostic, "执行日志插入", statement.get());
        if (mysql_stmt_affected_rows(statement.get()) != 1)
            return audit_failure(diagnostic, "检查日志写入行数",
                "数据库报告的写入行数不是一行，请检查日志表及触发器");
        if (mysql_warning_count(connection.get()) != 0)
            return audit_failure(diagnostic, "检查日志写入警告",
                "数据库返回了警告，记录可能已经写入，请勿自动重复写入");
        return true;
    }
    catch (...) {
        return audit_failure(diagnostic, "处理日志写入",
            "日志处理发生本地异常或内存不足");
    }
}

bool record_game_login(const std::string& account_id, uint32_t uid,
    const std::string& player_name, std::string* diagnostic) noexcept
{
    if (diagnostic) diagnostic->clear();
    if (!valid_input(account_id, kAccountLimit, true) || !uid ||
        player_name.empty() || player_name.size() > wire::profile_name_limit ||
        !wire::valid_text(player_name))
        return audit_failure(diagnostic, "检查游戏登录事件", "账号、玩家编号或游戏名无效");
    return store_session<AuthResult>(login_schema, {"初始化游戏登录记录", "初始化游戏登录记录线程",
        "设置游戏登录记录安全会话", "写入游戏登录记录"}, 1, diagnostic, [&](MYSQL* connection) {
        const std::string notes = std::to_string(uid);
        const std::string explanation = "游戏端:登入信息";
        // This is a game-packet event, not an update to the TLS login record.
        const auto statement = store_statement(login_schema, connection,
            "INSERT INTO `userlogin8` (`accountID`,`password`,`machineCode`,"
            "`LoginTime`,`accountName`,`notes`,`Explanation`) "
            "VALUES (?,'','',CURRENT_TIMESTAMP,?,?,?)",
            {account_id, player_name, notes, explanation}, diagnostic, "写入游戏登录记录");
        if (!statement) return AuthResult::unavailable;
        if (mysql_stmt_affected_rows(statement.get()) != 1) {
            audit_failure(diagnostic, "检查游戏登录记录行数",
                "数据库报告的写入行数不是一行，请检查日志表及触发器");
            return AuthResult::unavailable;
        }
        if (mysql_warning_count(connection) != 0) {
            audit_failure(diagnostic, "检查游戏登录记录写入警告",
                "数据库返回了警告，记录可能已经写入，请勿自动重复写入");
            return AuthResult::unavailable;
        }
        return AuthResult::ok;
    }) == AuthResult::ok;
}

#include "friends_store.h"
#include "avatar_store.h"
#include "media_store.h"
#include "profile_store.h"
#include "account_preferences_store.h"
#include "notice_store.h"
#include "hero_ranking_store.h"
#include "hero_permissions_store.h"
#include "online_time_store.h"
#include "online_ranking_store.h"

} // namespace server
} // namespace chh

