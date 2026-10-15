#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include "network/tls_transport.h"
#include "network/protocol.h"
#include "network/login_proof.h"
#include "MySQL.h"
#include "hero_ranking.h"
#include "online_time.h"
#include "sessions.h"
#include "roster_display.h"
#include "chat_room.h"
#include "avatar_session.h"
#include "media_session.h"
#include "startup_diagnostics.h"
#include "social_diagnostics.h"
#include <ws2tcpip.h>
#include <conio.h>
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <iostream>
#include <limits>
#include <map>
#include <mutex>
#include <optional>
#include <set>
#include <sstream>
#include <thread>
#include <vector>

namespace {
using Clock = std::chrono::steady_clock;
std::string certificate_error_zh(const std::string& error) {
    static const std::pair<const char*, const char*> messages[] = {
        {"Invalid PFX path or password", "证书路径或证书密码无效"},
        {"PFX must be between 1 byte and 4 MiB", "证书文件大小必须在1字节到4 MiB之间"},
        {"PFX must contain exactly one certificate with a private key", "证书文件必须包含且仅包含一个带私钥的证书"},
        {"PFX has no certificate with an accessible private key", "证书中没有可读取的私钥"},
        {"Server certificate is expired or not yet valid", "服务器证书已过期或尚未生效，请检查证书和系统时间"},
        {"PFX private key is not a CNG key", "证书私钥不支持所需的CNG接口"},
        {"Cannot generate temporary private key name", "无法生成临时私钥容器名称"},
        {"Open PFX", "无法打开证书文件"}, {"Read PFX", "无法读取证书文件"},
        {"Import PFX", "无法导入证书"}, {"Prepare temporary private key", "无法准备临时私钥"},
        {"Size private key", "无法读取私钥大小"}, {"Export temporary private key", "无法准备私钥数据"},
        {"Open private key provider", "无法打开私钥提供程序"},
        {"Import temporary private key", "无法导入临时私钥"},
        {"Attach temporary private key", "无法关联临时私钥"},
        {"Acquire TLS credentials", "无法初始化TLS凭据"}
    };
    for (const auto& [source, translation] : messages) {
        if (error == source) return translation;
        const std::string prefix = std::string(source) + " failed (";
        if (error.compare(0, prefix.size(), prefix) != 0) continue;
        const auto code = error.substr(prefix.size());
        if (code.size() == 11 && code.compare(0, 2, "0x") == 0 && code.back() == ')' &&
            code.find_first_not_of("0123456789abcdefABCDEF", 2) == 10)
            return std::string(translation) + "（系统错误码：" + code.substr(0, 10) + "）";
    }
    return "证书初始化失败，请检查证书文件和私钥读取权限";
}
std::atomic_bool stopping{false};
// Signalled when run_server has joined its workers and released the database.
const HANDLE shutdown_complete = CreateEventW(nullptr, TRUE, FALSE, nullptr);
BOOL WINAPI console_signal(DWORD code) {
    if (code == CTRL_C_EVENT || code == CTRL_BREAK_EVENT || code == CTRL_CLOSE_EVENT) {
        stopping.store(true);
        // Windows ends the process as soon as a close-event handler returns, so
        // hold it (under the ~5 s system limit) until workers have recorded
        // offline times and the normal shutdown has finished.
        if (code == CTRL_CLOSE_EVENT && shutdown_complete) WaitForSingleObject(shutdown_complete, 4500);
        return TRUE;
    }
    return FALSE;
}
int64_t unix_seconds() {
    return std::chrono::duration_cast<std::chrono::seconds>(
        std::chrono::system_clock::now().time_since_epoch()).count();
}
// Wire timestamps are 32-bit Unix seconds.
std::optional<uint32_t> unix_seconds32() {
    const auto seconds = unix_seconds();
    if (seconds < 0 || static_cast<uint64_t>(seconds) > UINT32_MAX) return std::nullopt;
    return static_cast<uint32_t>(seconds);
}
void clear_console() {
    const HANDLE output = GetStdHandle(STD_OUTPUT_HANDLE);
    if (output != nullptr && output != INVALID_HANDLE_VALUE && GetFileType(output) == FILE_TYPE_CHAR) {
        CONSOLE_SCREEN_BUFFER_INFO info{};
        if (GetConsoleScreenBufferInfo(output, &info)) {
            const DWORD cells = static_cast<DWORD>(info.dwSize.X) *
                static_cast<DWORD>(info.dwSize.Y);
            const COORD origin{0, 0};
            DWORD written = 0;
            FillConsoleOutputCharacterW(output, L' ', cells, origin, &written);
            FillConsoleOutputAttribute(output, info.wAttributes, cells, origin, &written);
            SetConsoleCursorPosition(output, origin);
            return;
        }
    }
    // A redirected/non-Windows console has no screen buffer to reset.
    std::system("cls");
}
chh::server::Sessions online;
void print_online_clients() {
    const auto snapshot = online.connections_snapshot();
    std::ostringstream output;
    output << "在线账号：" << snapshot.count.accounts << "；客户端：" << snapshot.count.primary
        << "；游戏端：" << snapshot.count.secondary << '\n';
    for (const auto role : {chh::server::ClientRole::primary, chh::server::ClientRole::secondary}) {
        const bool primary = role == chh::server::ClientRole::primary;
        output << (primary ? "客户端列表：\n" : "游戏端列表：\n");
        bool found = false;
        for (const auto& client : snapshot.clients) {
            if (client.role != role) continue;
            found = true;
            std::string name = client.name.empty() ? (primary ? "未设置名称" : "尚未上报游戏名") : client.name;
            // Keep each connection on one line even with legacy account names.
            for (char& character : name)
                if (static_cast<unsigned char>(character) < 0x20 || character == 0x7f) character = ' ';
            in_addr address{};
            address.s_addr = client.ip;
            char ip[INET_ADDRSTRLEN]{};
            const char* peer_ip = InetNtopA(AF_INET, &address, ip, sizeof(ip));
            output << (primary ? "  名称：" : "  游戏名：") << name << "；sock：" << client.socket
                << "；ip:" << (peer_ip ? peer_ip : "未知") << '\n';
        }
        if (!found) output << "  （无在线连接）\n";
    }
    std::cout << output.str() << std::flush;
}
void request_console_explorer(const std::string& text, const chh::server::Sessions::ConnectionsSnapshot& candidates) {
    uintptr_t socket = 0;
    if (text.empty()) { std::cout << "未输入 sock，已取消。\n" << std::flush; return; }
    if (text.size() > 20) { std::cout << "sock 数字过长，已取消。\n" << std::flush; return; }
    for (const char c : text) {
        if (c < '0' || c > '9' || socket > ((std::numeric_limits<uintptr_t>::max)() - (c - '0')) / 10) {
            std::cout << "sock 格式无效，请输入按1显示的十进制 sock。\n" << std::flush; return;
        }
        socket = socket * 10 + (c - '0');
    }
    const auto target = std::find_if(candidates.clients.begin(), candidates.clients.end(), [&](const auto& client) {
        return client.role == chh::server::ClientRole::primary && client.socket == socket;
    });
    if (target == candidates.clients.end()) {
        std::cout << "该 sock 不是按2时在线的 LA 客户端，请按1重新查看。\n" << std::flush; return;
    }
    const auto result = online.queue_explorer(socket, target->connection_token);
    using Result = chh::server::Sessions::ExplorerQueueResult;
    std::ostringstream output;
    if (result.result == Result::queued)
        output << "explorer.exe 请求 " << result.request_id << " 已排队，目标 sock：" << socket << "。\n";
    else if (result.result == Result::not_ready)
        output << "该客户端尚未声明支持此功能，请更新 LA 客户端并等待登录完成。\n";
    else if (result.result == Result::busy)
        output << "该客户端已有 explorer.exe 请求正在处理，请等待结果。\n";
    else output << "该客户端连接已变化，请按1重新查看 sock 后再操作。\n";
    std::cout << output.str() << std::flush;
}
chh::server::ChatRoom lobby;
chh::server::HeroRankingCache hero_ranking;
chh::server::PermissionRankingCache permission_ranking;
chh::server::OnlineTimeRankingCache online_time_ranking;
chh::server::OnlineTimeTracker online_time;
std::condition_variable online_time_wakeup;
std::atomic<uint64_t> social_revision{0};
std::atomic<uint64_t> offline_revision{0};
// Steady-clock tick at which any local-history session may purge old send tombstones.
std::atomic<std::chrono::steady_clock::rep> private_nonce_purge_due{0};
// Account mutations and authentication-to-admission share the same gate. A
// password authenticated before a change cannot enter after invalidation.
std::array<std::mutex, 64> account_gates;
std::mutex& account_gate(const std::string& id) {
    return account_gates[std::hash<std::string>{}(id) % account_gates.size()];
}
chh::server::ThrottledReport report_offline_error{"离线时间暂时不可用：", "数据库操作未完成"};
chh::server::ThrottledReport report_hero_rank_error{"英雄排行榜暂时不可用：", "数据库读取未完成"};
chh::server::ThrottledReport report_online_time_error{"在线时长暂时无法保存：", "数据库操作未完成"};
// The sole checkpoint writer keeps database latency out of client heartbeat
// handling. It wakes for disconnects and retries failed, already-finished users.
class OnlineTimeWorker {
public:
    OnlineTimeWorker() : thread_([this] { run(); }) {}
    ~OnlineTimeWorker() {
        { std::lock_guard lock(mutex_); stopped_ = true; }
        online_time_wakeup.notify_one();
        if (thread_.joinable()) thread_.join();
    }
private:
    void flush(bool force) noexcept {
        try {
            online_time.flush_due(Clock::now(), chh::server::load_online_seconds,
                chh::server::save_online_seconds,
                [](const std::string& diagnostic) { report_online_time_error(diagnostic); }, force);
        } catch (...) { report_online_time_error("在线时长结算发生本地异常，保留待保存记录"); }
    }
    void run() {
        std::unique_lock lock(mutex_);
        while (!stopped_) {
            online_time_wakeup.wait_for(lock, std::chrono::seconds(1));
            if (stopped_) break;
            lock.unlock(); flush(false); lock.lock();
        }
        lock.unlock();
        flush(true); // Client workers have finished before this destructor runs.
        if (online_time.pending_accounts())
            std::cerr << "服务器停止时仍有在线时长未保存，请检查数据库连接和onlineSeconds更新权限。\n";
    }
    std::mutex mutex_;
    bool stopped_ = false;
    std::thread thread_;
};
void finish_online_time(const chh::server::Sessions::Binding& binding) {
    if (binding.role == chh::server::ClientRole::primary &&
        online_time.finish(binding.key.first, binding.token)) online_time_wakeup.notify_one();
}
void apply_account_preferences(const std::string& account_id, bool ready, bool hidden) {
    online.account_preferences(account_id, ready, hidden);
    lobby.set_author_hidden(account_id, hidden);
}
// Caller owns account_gate, including the already-held password mutation gate.
void record_last_primary_offline(const std::string& actor) noexcept {
    try {
        const auto seconds = unix_seconds();
        std::string diagnostic;
        if (seconds > 0 && chh::server::record_offline_time(actor, static_cast<uint64_t>(seconds), &diagnostic)) ++offline_revision;
        else report_offline_error(diagnostic);
    } catch (...) { /* A best-effort optional field must not terminate a worker destructor. */ }
}
struct Membership {
    chh::server::Sessions::Binding binding;
    bool admitting = false;
    ~Membership() {
        if (!binding) return;
        std::unique_lock lock(account_gate(binding.key.first), std::defer_lock);
        // Admission still owns this same gate if setup unwinds before unlock.
        if (!admitting) lock.lock();
        if (online.leave(binding)) {
            finish_online_time(binding);
            record_last_primary_offline(binding.key.first);
        }
    }
};
struct FailureLimiter {
    std::mutex mutex;
    std::map<unsigned long, std::deque<Clock::time_point>> failures;
    bool blocked(unsigned long ip, bool record = false) {
        std::lock_guard lock(mutex);
        const auto cutoff = Clock::now() - std::chrono::minutes(1);
        for (auto it = failures.begin(); it != failures.end();) {
            while (!it->second.empty() && it->second.front() < cutoff) it->second.pop_front();
            if (it->second.empty()) it = failures.erase(it); else ++it;
        }
        if (record) {
            if (failures.size() >= 4096 && failures.find(ip) == failures.end()) return true;
            failures[ip].push_back(Clock::now());
        }
        const auto it = failures.find(ip);
        return it != failures.end() && it->second.size() >= 10;
    }
} limiter;
bool send_packet(chh::tls::Stream& stream, const chh::wire::Packet& packet) {
    return stream.write_all(packet.bytes.data(), packet.bytes.size(), 5000);
}
// A frame whose payload is only 32-bit numbers (errors, results, counts).
bool send_numbers(chh::tls::Stream& stream, chh::wire::Type type, std::initializer_list<uint32_t> values) {
    chh::wire::Packet packet(type);
    for (const auto value : values) if (!packet.add_number(value)) return false;
    return send_packet(stream, packet);
}
template<class Snapshot>
bool send_command(chh::tls::Stream& stream, chh::wire::Type type, const Snapshot& command,
    uint64_t& sent_revision, std::initializer_list<uint32_t> values) {
    if (!command.initialized || command.revision == sent_revision) return true;
    if (!send_numbers(stream, type, values)) return false;
    sent_revision = command.revision;
    return true;
}
void send_error(chh::tls::Stream& stream, chh::wire::Error error) {
    send_numbers(stream, chh::wire::Type::error, {uint32_t(error)});
}
bool send_chat_result(chh::tls::Stream& stream, uint32_t request_id, chh::wire::ChatResult result) {
    return send_numbers(stream, chh::wire::Type::chat_result, {request_id, uint32_t(result)});
}
bool send_chat_recalled(chh::tls::Stream& stream, const chh::server::ChatRoom::Recall& event) {
    chh::wire::Packet packet(chh::wire::Type::chat_recalled);
    return packet.add_u64(event.id) && packet.add_number(event.timestamp) && send_packet(stream, packet);
}
bool send_chat_message(chh::tls::Stream& stream, const chh::server::ChatRoom::Message& message,
    const std::string& recipient_id, bool enhanced, bool preferences) {
    chh::wire::Packet packet;
    auto current = message;
    current.hide_account_id = lobby.author_hidden(message);
    return chh::server::chat_message_packet(current, recipient_id, packet, enhanced, preferences) && send_packet(stream, packet);
}
struct SocialState {
    bool joined = false, loaded = false, enhanced = false, contacts_loaded = false;
    chh::server::SocialDiagnostics diagnostics;
    bool preferences = false, notes_loaded = false;
    bool clear_enabled = false, clears_loaded = false;
    bool offline_enabled = false, offline_loaded = false;
    bool online_time_enabled = false;
    bool game_presence = false, game_team = false;
    bool notices_enabled = false, notice_cursor_known = false;
    uint64_t notice_cursor = 0;
    uint64_t relation_revision = UINT64_MAX, presence_revision = UINT64_MAX, private_cursor = 0;
    uint64_t game_revision = UINT64_MAX;
    uint64_t conversations_revision = UINT64_MAX;
    uint64_t notes_revision = UINT64_MAX;
    uint64_t clear_revision = 0, recall_revision = 0;
    uint64_t offline_revision = UINT64_MAX, offline_presence_revision = UINT64_MAX;
    std::vector<chh::server::FriendRecord> friends;
    std::vector<chh::server::FriendRecord> contacts;
    std::map<std::string, std::string> notes;
    std::map<std::string, uint64_t> offline_times;
    std::map<std::string, uint64_t> online_times;
    std::string peer;
    std::string clear_after_peer;
    Clock::time_point next_refresh{}, next_private{}, next_open{}, next_notice{};
    Clock::time_point next_online_time_refresh{};
    chh::server::ChatRateLimit action_rate, private_rate, recall_rate, join_rate;
    // Local-history sessions (social feature 128). Cursors start at 0 on every
    // connection, so whatever was not acknowledged is sent again.
    bool local_history = false;
    // One pushed stream (messages or recall events). Rows are deleted only
    // after the client acknowledges a durable write; ids left unacknowledged
    // for 30 s are sent again.
    struct DeliveryChannel {
        uint64_t cursor = 0, high = 0, acked = 0, deleted = 0;
        std::deque<uint64_t> window;
        Clock::time_point progress{}, next_ack{};
        void sent(uint64_t id, Clock::time_point now) {
            if (window.empty()) progress = now;
            window.push_back(id);
            cursor = id; high = (std::max)(high, id);
        }
        // Only ids already sent on this connection may be deleted; a stale
        // acknowledgement from an earlier connection is ignored.
        bool acknowledge(uint64_t through, Clock::time_point now) {
            if (through <= acked || through > high) return false;
            acked = through; progress = now;
            while (!window.empty() && window.front() <= through) window.pop_front();
            next_ack = {};
            return true;
        }
        bool rewind_if_stalled(Clock::time_point now) {
            if (window.empty() || now - progress <= std::chrono::seconds(30)) return false;
            window.clear(); cursor = acked;
            return true;
        }
    } delivery, recall;
    uint64_t delivery_revision = UINT64_MAX;
    Clock::time_point next_delivery{}, delivery_retry{};
    uint32_t export_request = 0; uint64_t export_value = 0; bool export_finish = false, export_recorded = false;
    Clock::time_point next_export{};
    chh::server::PrivatePurge purge; bool purging = false;
    Clock::time_point next_purge{};
    // Social feature 256: pending image rights (media_grants8) are pushed as
    // type 117 and removed when the client acknowledges them (118). The cursor
    // starts at 0 on every connection, so every right still pending is pushed
    // again after a reconnect.
    bool private_media = false;
    struct MediaRights {
        uint64_t cursor = 0;
        // Pushed and not yet acknowledged, with the time of the push. A client
        // acknowledges only images its own history uses, so an entry that
        // stays unacknowledged for private_media_unacked leaves the window.
        std::map<std::string, Clock::time_point> outstanding;
        std::deque<std::string> acks;
        // retry: applying acknowledgements failed; page_retry: reading rights failed
        // (neither holds up message delivery).
        Clock::time_point next_apply{}, retry{}, page_retry{};
        // Rights are only created by sends to this account, which move the
        // conversation revision; otherwise the table is read again only after
        // a full page or every 30 s, not on every one-second delivery poll.
        // next_read paces the reads (at most one a second, unless a full batch
        // came back) independently of the message inbox.
        uint64_t revision = UINT64_MAX; bool more = false;
        Clock::time_point next_page{}, next_read{};
    } media_rights;
};
// Only a successful result for this operation clears its outstanding failure.
// Validation/rate-limit responses did not establish database health.
void report_social_database_result(SocialState& state, const char* operation,
    chh::wire::SocialResult result, const std::string& diagnostic) {
    if (result == chh::wire::SocialResult::unavailable)
        state.diagnostics.failure(operation, diagnostic);
    else if (result == chh::wire::SocialResult::ok)
        state.diagnostics.recovered(operation);
}
bool send_social_result(chh::tls::Stream& stream, uint32_t request, uint32_t operation, chh::wire::SocialResult code) {
    return send_numbers(stream, chh::wire::Type::social_result, {request, operation, uint32_t(code)});
}
struct AccountOnlineTime {
    bool ready = false;
    uint64_t seconds = 0;
    Clock::time_point next_refresh{};
    bool refresh(const std::string& account, Clock::time_point now) {
        if (now < next_refresh) return false;
        next_refresh = now + std::chrono::seconds(60);
        uint64_t loaded = 0;
        ready = chh::server::load_online_seconds(account, loaded, nullptr);
        seconds = ready ? loaded : 0;
        return true;
    }
};
bool send_account_state(chh::tls::Stream& stream, const chh::server::AccountProfile& profile,
    const chh::server::Sessions::Profile& current, bool preferences,
    const AccountOnlineTime* online_time_state) {
    chh::wire::Packet packet(chh::wire::Type::account_state);
    return packet.add_string(profile.account_id, 64) && packet.add_string(current.name, 512) &&
        packet.add_string(profile.expiration, 19) &&
        (!preferences || (packet.add_number(current.preferences_ready ? 1u : 0u) &&
            packet.add_number(current.hide_account_id ? 1u : 0u))) &&
        (!online_time_state || (packet.add_number(online_time_state->ready ? 1u : 0u) &&
            packet.add_u64(online_time_state->seconds))) && send_packet(stream, packet);
}
bool send_account_result(chh::tls::Stream& stream, uint32_t request, uint32_t action, chh::wire::AccountResult result) {
    return send_numbers(stream, chh::wire::Type::account_result, {request, action, uint32_t(result)});
}
bool send_hero_permissions_result(chh::tls::Stream& stream, uint32_t request, chh::wire::HeroPermissionsResult result) {
    return send_numbers(stream, chh::wire::Type::hero_permissions_result, {request, uint32_t(result)});
}
bool send_hero_permissions(chh::tls::Stream& stream, uint32_t request, const std::vector<uint32_t>& ids, bool heroes = false) {
    if (ids.size() > (heroes ? chh::wire::hero_availability_id_limit : chh::wire::hero_permissions_id_limit)) return false;
    chh::wire::Packet begin(chh::wire::Type::hero_permissions_begin);
    if (!begin.add_number(request) || !begin.add_number(static_cast<uint32_t>(ids.size())) ||
        (heroes && !begin.add_number(chh::wire::hero_permissions_scope_heroes)) || !send_packet(stream, begin)) return false;
    for (size_t offset = 0; offset < ids.size();) {
        const uint32_t count = static_cast<uint32_t>((std::min)(ids.size() - offset,
            static_cast<size_t>(chh::wire::hero_permissions_chunk_limit)));
        chh::wire::Packet chunk(chh::wire::Type::hero_permissions_chunk);
        if (!chunk.add_number(request) || !chunk.add_number(static_cast<uint32_t>(offset)) || !chunk.add_number(count)) return false;
        for (uint32_t index = 0; index < count; ++index)
            if (!chunk.add_number(ids[offset + index])) return false;
        if (!send_packet(stream, chunk)) return false;
        offset += count;
    }
    chh::wire::Packet end(chh::wire::Type::hero_permissions_end);
    return end.add_number(request) && send_packet(stream, end);
}
bool send_hero_catalog(chh::tls::Stream& stream, uint32_t request, const std::vector<chh::server::HeroCatalogEntry>& rows) {
    if (rows.size() > chh::wire::hero_availability_id_limit) return false;
    chh::wire::Packet begin(chh::wire::Type::hero_permissions_begin);
    if (!begin.add_number(request) || !begin.add_number(static_cast<uint32_t>(rows.size())) ||
        !begin.add_number(chh::wire::hero_permissions_scope_catalog) || !send_packet(stream, begin)) return false;
    for (size_t index = 0; index < rows.size(); ++index) {
        const auto& row = rows[index];
        chh::wire::Packet entry(chh::wire::Type::hero_permissions_chunk);
        if (!entry.add_number(request) || !entry.add_number(static_cast<uint32_t>(index)) || !entry.add_number(1) ||
            !entry.add_number(row.id) || !entry.add_number(row.available ? 1u : 0u) ||
            !entry.add_string(row.name, chh::wire::hero_catalog_name_limit) || !send_packet(stream, entry)) return false;
    }
    chh::wire::Packet end(chh::wire::Type::hero_permissions_end);
    return end.add_number(request) && send_packet(stream, end);
}
bool send_hero_rank_result(chh::tls::Stream& stream, uint32_t request, chh::wire::HeroRankResult result) {
    return send_numbers(stream, chh::wire::Type::hero_rank_result, {request, uint32_t(result)});
}
bool send_hero_rank_page(chh::tls::Stream& stream, uint32_t request, const chh::server::HeroRankPage& page) {
    chh::wire::Packet begin(chh::wire::Type::hero_rank_begin);
    if (!begin.add_number(request) || !begin.add_number(page.mode) || !begin.add_number(page.sort) ||
        !begin.add_number(page.page) || !begin.add_number(chh::wire::hero_rank_page_size) ||
        !begin.add_number(page.total) || !begin.add_number(page.matched) ||
        !begin.add_number(static_cast<uint32_t>(page.rows.size())) || !begin.add_u64(page.updated_at) ||
        !begin.add_string(page.query, chh::wire::hero_query_limit) || !send_packet(stream, begin)) return false;
    for (const auto& row : page.rows) {
        chh::wire::Packet entry(chh::wire::Type::hero_rank_entry);
        if (!entry.add_number(request) || !entry.add_number(row.rank)) return false;
        if (page.mode == uint32_t(chh::wire::HeroRankMode::online_time)) {
            if (!entry.add_string(row.hide_account_id ? std::string{} : row.account_id, chh::wire::permission_account_limit) ||
                !entry.add_string(row.name, chh::wire::permission_name_limit) ||
                !entry.add_string(row.avatar_hash, 64) || !entry.add_number(row.hide_account_id ? 1u : 0u) ||
                !entry.add_u64(row.online_seconds) || !send_packet(stream, entry)) return false;
        } else if (page.mode == uint32_t(chh::wire::HeroRankMode::permissions)) {
            if (!entry.add_string(row.account_id, chh::wire::permission_account_limit) ||
                !entry.add_string(row.name, chh::wire::permission_name_limit) ||
                !entry.add_string(row.permissions, chh::wire::permission_text_limit) ||
                !entry.add_string(row.avatar_hash, 64) ||
                !entry.add_number(row.hide_account_id ? 1u : 0u) ||
                !send_packet(stream, entry)) return false;
        } else if (!entry.add_number(row.hero_id) ||
            !entry.add_string(row.name, chh::wire::hero_name_limit) || !entry.add_u64(row.uses) ||
            !entry.add_u64(row.wins) || !entry.add_u64(row.losses) || !send_packet(stream, entry)) return false;
    }
    chh::wire::Packet end(chh::wire::Type::hero_rank_end);
    return end.add_number(request) && send_packet(stream, end);
}
bool send_game_notes(chh::tls::Stream& stream, uint32_t request,
    const chh::server::NoticePage& page) {
    chh::wire::Packet begin(chh::wire::Type::game_notes_begin);
    if (!begin.add_number(request) || !begin.add_number(page.page) ||
        !begin.add_number(page.matched) || !begin.add_number(static_cast<uint32_t>(page.records.size())) ||
        !begin.add_string(page.query, chh::wire::game_note_query_limit, true) ||
        !begin.add_string(page.uid, chh::wire::game_note_uid_limit, true) ||
        // The marker-account echo exists only for requests that carried one (older clients never do).
        (!page.account.empty() && !begin.add_string(page.account, chh::wire::game_note_account_id_limit)) ||
        !send_packet(stream, begin)) return false;
    for (const auto& note : page.records) {
        chh::wire::Packet entry(chh::wire::Type::game_note_entry);
        if (!entry.add_number(request) || !entry.add_string(note.account_id, 64) ||
            !entry.add_string(note.uid, chh::wire::game_note_uid_limit) ||
            !entry.add_string(note.game_name, chh::wire::game_note_name_limit) ||
            !entry.add_string(note.notes_time, chh::wire::game_note_time_limit) ||
            !send_packet(stream, entry)) return false;
        chh::wire::Packet detail(chh::wire::Type::game_note_detail);
        if (!detail.add_number(request) ||
            !detail.add_string(note.account_name, chh::wire::game_note_account_name_limit, true) ||
            !detail.add_string(note.notes, chh::wire::game_note_text_limit, true) ||
            !send_packet(stream, detail)) return false;
    }
    chh::wire::Packet end(chh::wire::Type::game_notes_end);
    return end.add_number(request) && send_packet(stream, end);
}
bool send_game_note_result(chh::tls::Stream& stream, uint32_t request,
    chh::wire::GameNoteAction action, chh::wire::GameNoteResult result) {
    return send_numbers(stream, chh::wire::Type::game_note_result, {request, uint32_t(action), uint32_t(result)});
}
bool send_friends(chh::tls::Stream& stream, const std::vector<chh::server::FriendRecord>& friends,
    const chh::server::Sessions::Presence& presence, const SocialState& social) {
    const bool enhanced = social.enhanced;
    const bool game_presence = social.game_presence;
    const bool game_team_presence = social.game_team;
    chh::wire::Packet begin(chh::wire::Type::friends_begin);
    begin.set_number(static_cast<uint32_t>(friends.size()));
    if (!send_packet(stream, begin)) return false;
    for (const auto& entry : friends) {
        chh::wire::Packet packet(chh::wire::Type::friend_entry);
        const bool visible_online = entry.relation == chh::wire::FriendRelation::accepted ||
            (enhanced && entry.relation == chh::wire::FriendRelation::temporary);
        const bool is_online = visible_online &&
            presence.primary_accounts.count(entry.id) != 0;
        uint32_t game_state = 0;
        bool game_connected = false;
        bool game_team = false;
        std::string game_player_name;
        if (enhanced && entry.relation == chh::wire::FriendRelation::accepted) {
            const auto game = presence.game_states.find(entry.id);
            if (game != presence.game_states.end()) game_state = game->second;
            const auto connected = presence.game_connected.find(entry.id);
            if (connected != presence.game_connected.end()) game_connected = connected->second;
            const auto team = presence.game_teams.find(entry.id);
            if (team != presence.game_teams.end()) game_team = team->second;
            const auto player = presence.game_player_names.find(entry.id);
            if (player != presence.game_player_names.end()) game_player_name = player->second;
        }
        const auto note = social.notes.find(entry.id);
        const std::string_view remark = social.preferences && entry.relation == chh::wire::FriendRelation::accepted &&
            note != social.notes.end() ? std::string_view(note->second) : std::string_view{};
        const auto offline = social.offline_times.find(entry.id);
        const uint64_t offline_time = !is_online && entry.relation == chh::wire::FriendRelation::accepted &&
            offline != social.offline_times.end() ? offline->second : 0;
        const auto online_time = social.online_times.find(entry.id);
        const bool online_time_ready = entry.relation == chh::wire::FriendRelation::accepted &&
            online_time != social.online_times.end();
        if (!packet.add_string(entry.id, 64) || !packet.add_string(entry.name, 512) ||
            !packet.add_number(uint32_t(entry.relation)) || !packet.add_number(is_online ? 1u : 0u) ||
            (enhanced && !packet.add_number(game_state)) ||
            (game_presence && !packet.add_number(game_connected ? 1u : 0u)) ||
            (game_team_presence && !packet.add_number(game_team ? 1u : 0u)) ||
            (game_team_presence && !packet.add_string(game_player_name, chh::wire::profile_name_limit)) ||
            (social.preferences && !packet.add_number(entry.hide_account_id ? 1u : 0u)) ||
            (social.preferences && !packet.add_string(remark, chh::wire::profile_name_limit)) ||
            (social.offline_enabled && !packet.add_u64(offline_time)) ||
            (social.online_time_enabled && (!packet.add_number(online_time_ready ? 1u : 0u) ||
                !packet.add_u64(online_time_ready ? online_time->second : 0))) || !send_packet(stream, packet)) return false;
    }
    return send_packet(stream, chh::wire::Packet(chh::wire::Type::friends_end));
}
bool refresh_social(chh::tls::Stream& stream, SocialState& social, const std::string& actor,
    uint64_t conversation_revision, uint64_t notes_revision) {
    const auto now = Clock::now();
    if (!social.joined || now < social.next_refresh) return true;
    social.next_refresh = now + std::chrono::seconds(1);
    const auto revision = social_revision.load();
    bool changed = false;
    if (!social.loaded || revision != social.relation_revision) {
        std::vector<chh::server::FriendRecord> candidate;
        std::string diagnostic;
        const auto result = chh::server::list_friends(actor, candidate, &diagnostic);
        report_social_database_result(social, "读取好友列表", result, diagnostic);
        if (result != chh::wire::SocialResult::ok) {
            social.next_refresh = now + std::chrono::seconds(5);
            if (social.loaded) { social.loaded = false; return send_social_result(stream, 0, 8, result); }
            // A failed initial join must finish with a result, while automatic
            // retries stay quiet until the backend recovers.
            if (social.relation_revision == UINT64_MAX) {
                social.relation_revision = revision;
                return send_social_result(stream, 0, 8, result);
            }
            return true;
        }
        social.friends = std::move(candidate); social.loaded = true;
        social.relation_revision = revision; changed = true;
        bool still_friend = social.peer.empty();
        for (const auto& entry : social.friends)
            if (entry.id == social.peer && entry.relation == chh::wire::FriendRelation::accepted) still_friend = true;
        if (!social.enhanced && !still_friend) { social.peer.clear(); social.private_cursor = 0; }
    }
    const bool relations_changed = changed;
    if (social.enhanced) {
        if (!social.contacts_loaded || changed || conversation_revision != social.conversations_revision) {
            std::vector<chh::server::FriendRecord> candidate;
            std::string diagnostic;
            const auto result = chh::server::list_private_contacts(actor, candidate, &diagnostic);
            report_social_database_result(social, "读取临时联系人", result, diagnostic);
            if (result != chh::wire::SocialResult::ok) {
                social.next_refresh = now + std::chrono::seconds(5);
                const bool notify = social.contacts_loaded || social.conversations_revision == UINT64_MAX;
                social.contacts_loaded = false; social.conversations_revision = conversation_revision;
                return !notify || send_social_result(stream, 0, 8, result);
            }
            social.contacts = std::move(candidate); social.contacts_loaded = true;
            social.conversations_revision = conversation_revision; changed = true;
        }
    }
    if (social.preferences && (!social.notes_loaded || relations_changed || social.notes_revision != notes_revision)) {
        std::vector<chh::server::FriendNote> candidate;
        std::string diagnostic;
        const auto result = chh::server::list_friend_notes(actor, candidate, &diagnostic);
        report_social_database_result(social, "读取好友备注", result, diagnostic);
        if (result == chh::wire::SocialResult::ok) {
            social.notes.clear();
            for (auto& note : candidate) social.notes.emplace(std::move(note.peer_id), std::move(note.note));
            social.notes_loaded = true; social.notes_revision = notes_revision; changed = true;
        } else {
            // Missing optional tables must not take the existing friends UI down.
            // Resend the list only to withdraw remarks it showed; a persistently
            // failing query must not re-stream the whole list every retry.
            if (!social.notes.empty()) changed = true;
            social.notes.clear(); social.notes_loaded = false;
            social.next_refresh = now + std::chrono::seconds(5);
        }
    }
    const auto presence = online.presence();
    const auto offline_version = offline_revision.load();
    if (social.offline_enabled && (!social.offline_loaded || relations_changed ||
        social.offline_presence_revision != presence.revision || social.offline_revision != offline_version)) {
        std::vector<chh::server::FriendOffline> candidate;
        std::string diagnostic;
        social.offline_times.clear();
        if (chh::server::list_friend_offline_times(actor, candidate, &diagnostic)) {
            for (const auto& entry : candidate) social.offline_times.emplace(entry.peer_id, entry.offline_time);
        } else report_offline_error(diagnostic);
        // Record the version captured before the query. A concurrent completed
        // offline write still triggers a follow-up refresh; failures do not poll.
        social.offline_loaded = true; social.offline_presence_revision = presence.revision;
        social.offline_revision = offline_version; changed = true;
    }
    if (social.online_time_enabled && (relations_changed || now >= social.next_online_time_refresh)) {
        social.next_online_time_refresh = now + std::chrono::seconds(60);
        std::vector<chh::server::FriendOnlineTime> candidate;
        std::string diagnostic;
        social.online_times.clear();
        if (chh::server::list_friend_online_times(actor, candidate, &diagnostic)) {
            for (const auto& entry : candidate) social.online_times.emplace(entry.peer_id, entry.online_seconds);
            social.diagnostics.recovered("读取好友在线时长");
        } else social.diagnostics.failure("读取好友在线时长", diagnostic);
        changed = true;
    }
    if (changed || presence.revision != social.presence_revision ||
        (social.enhanced && presence.game_revision != social.game_revision)) {
        auto entries = social.friends;
        if (social.enhanced) {
            for (const auto& contact : social.contacts) {
                const bool accepted = std::any_of(social.friends.begin(), social.friends.end(),
                    [&](const auto& entry) { return entry.id == contact.id && entry.relation == chh::wire::FriendRelation::accepted; });
                if (!accepted) entries.push_back(contact);
            }
        }
        if (!send_friends(stream, entries, presence, social)) return false;
        social.presence_revision = presence.revision;
        social.game_revision = presence.game_revision;
    }
    return true;
}
bool parse_social_join(const chh::wire::Packet& packet, uint32_t& features, bool& has_cursor, uint64_t& cursor) {
    features = 0; has_cursor = false; cursor = 0;
    if (!packet.length()) return true; // Original client: no capability word.
    chh::wire::Reader body{packet};
    if (!body.number(features)) return false;
    if (features == 1 || features == 3 || features == 7 || features == 15) return body.done();
    // Local-history clients never use the notice cursor, so it is optional.
    const bool local = features == (chh::wire::social_features | chh::wire::social_feature_local_history) ||
        features == (chh::wire::social_features | chh::wire::social_feature_local_history | chh::wire::social_feature_private_media) ||
        features == (chh::wire::social_features | chh::wire::social_feature_local_history |
            chh::wire::social_feature_private_media | chh::wire::social_feature_online_time);
    if (local && body.done()) return true;
    uint32_t supplied = 0;
    if ((features != chh::wire::social_features && features != 31 && !local) || !body.number(supplied) || supplied > 1
        || !body.u64(cursor) || !body.done()) return false;
    has_cursor = supplied != 0;
    return true;
}
bool send_private_stored(chh::tls::Stream& stream, uint32_t request, uint32_t kind, uint64_t id, uint32_t timestamp,
    const std::string& peer, const std::string& nonce) {
    chh::wire::Packet packet(chh::wire::Type::private_stored);
    return packet.add_number(request) && packet.add_number(kind) && packet.add_u64(id) && packet.add_number(timestamp) &&
        packet.add_string(peer, 64) && packet.add_string(nonce, 32) && send_packet(stream, packet);
}
constexpr size_t private_delivery_window = 100, private_delivery_batch = 20;
// At most this many pushed image rights wait for their acknowledgement, and
// acknowledgements are applied in batches of at most private_media_ack_batch.
// A pushed right still unacknowledged after private_media_unacked no longer
// counts: the client may hold no message with that image (its right is left
// for the PC that has one), or its fetch queue is long. The cursor has moved
// past it, so it is not pushed again on this connection.
constexpr size_t private_media_window = 100, private_media_ack_batch = 50, private_media_ack_queue = 256;
constexpr auto private_media_unacked = std::chrono::seconds(30);
// Deletes the rows the client acknowledged; a failed delete is retried after 5 s.
template<class Delete>
void delete_acknowledged(SocialState& social, SocialState::DeliveryChannel& channel, const char* operation,
    Clock::time_point now, Delete remove) {
    if (channel.acked <= channel.deleted || now < channel.next_ack) return;
    std::string diagnostic;
    const auto result = remove(channel.acked, &diagnostic);
    report_social_database_result(social, operation, result, diagnostic);
    if (result == chh::wire::SocialResult::ok) channel.deleted = channel.acked;
    else channel.next_ack = now + std::chrono::seconds(5);
}
// Local-history sessions: every pending message and recall event is pushed on
// this connection's own thread. Rows are deleted only after the client reports
// a durable local write; anything left unacknowledged for 30 s is sent again.
bool deliver_private(chh::tls::Stream& stream, SocialState& social, const std::string& actor,
    uint64_t conversation_revision, int& wait_ms) {
    const auto now = Clock::now();
    std::string diagnostic;
    delete_acknowledged(social, social.delivery, "删除已送达私聊", now, [&](uint64_t through, std::string* failure) {
        std::vector<std::string> released;
        const auto result = chh::server::ack_private_delivery(actor, through, failure, &released);
        if (result == chh::wire::SocialResult::ok) chh::server::media_collector.candidates(released);
        return result;
    });
    delete_acknowledged(social, social.recall, "删除已送达撤回事件", now, [&](uint64_t through, std::string* failure) {
        return chh::server::ack_private_recalls(actor, through, failure);
    });
    auto due = private_nonce_purge_due.load();
    if (now.time_since_epoch().count() >= due && private_nonce_purge_due.compare_exchange_strong(due,
        (now + std::chrono::hours(1)).time_since_epoch().count())) {
        bool more = false;
        const auto result = chh::server::purge_private_nonces(more, &diagnostic);
        report_social_database_result(social, "清理私聊防重复凭据", result, diagnostic);
        // A full batch means a backlog, drained a batch a minute rather than an hour.
        if (result != chh::wire::SocialResult::ok)
            private_nonce_purge_due.store((now + std::chrono::minutes(5)).time_since_epoch().count());
        else if (more)
            private_nonce_purge_due.store((now + std::chrono::minutes(1)).time_since_epoch().count());
    }
    // Image acknowledgements (118) are applied in batches, at most every
    // 500 ms unless a full batch is waiting; a failure keeps them for a retry.
    auto& rights = social.media_rights;
    if (!rights.acks.empty() && now >= rights.retry &&
        (now >= rights.next_apply || rights.acks.size() >= private_media_ack_batch)) {
        const size_t count = (std::min)(rights.acks.size(), private_media_ack_batch);
        const std::vector<std::string> batch(rights.acks.begin(), rights.acks.begin() + static_cast<std::ptrdiff_t>(count));
        std::vector<std::string> released;
        const auto result = chh::server::acknowledge_private_media(actor, batch, &diagnostic, &released);
        report_social_database_result(social, "删除已下载私聊图片授权", result, diagnostic);
        if (result == chh::wire::SocialResult::ok) {
            chh::server::media_collector.candidates(released);
            rights.acks.erase(rights.acks.begin(), rights.acks.begin() + static_cast<std::ptrdiff_t>(count));
            rights.next_apply = now + std::chrono::milliseconds(500);
        } else rights.retry = now + std::chrono::seconds(5);
    }
    if (social.delivery.rewind_if_stalled(now)) social.next_delivery = {};
    if (social.recall.rewind_if_stalled(now)) social.next_delivery = {};
    // Messages and recall events: at most once a second, at once after an
    // acknowledgement that made progress (109/111) or a conversation change.
    const bool inbox_due = now >= social.delivery_retry &&
        (now >= social.next_delivery || conversation_revision != social.delivery_revision);
    const size_t messages = inbox_due ?
        (std::min)(private_delivery_batch, private_delivery_window - social.delivery.window.size()) : 0;
    const size_t recalls = inbox_due ?
        (std::min)(private_delivery_batch, private_delivery_window - social.recall.window.size()) : 0;
    // Image rights keep their own pace (next_read), so an image acknowledgement
    // (118) never costs a private_inbox query. A conversation change makes the
    // inbox due as well, so its messages still go first; while the inbox read
    // fails, rights wait too.
    for (auto it = rights.outstanding.begin(); it != rights.outstanding.end();)
        it = now - it->second >= private_media_unacked ? rights.outstanding.erase(it) : std::next(it);
    const bool rights_due = social.private_media && now >= social.delivery_retry && now >= rights.page_retry &&
        now >= rights.next_read && rights.outstanding.size() < private_media_window &&
        (rights.more || rights.revision != conversation_revision || now >= rights.next_page);
    const size_t pending_rights = rights_due ?
        (std::min)(private_delivery_batch, private_media_window - rights.outstanding.size()) : 0;
    if (!messages && !recalls && !pending_rights) return true;
    chh::server::PrivateInbox inbox;
    if (messages || recalls) {
        social.delivery_revision = conversation_revision;
        social.next_delivery = now + std::chrono::seconds(1);
        const auto result = chh::server::private_inbox(actor, social.delivery.cursor, messages,
            social.recall.cursor, recalls, inbox, &diagnostic);
        report_social_database_result(social, "读取待送达私聊", result, diagnostic);
        if (result != chh::wire::SocialResult::ok) { social.delivery_retry = now + std::chrono::seconds(5); return true; }
    }
    for (const auto& message : inbox.messages) {
        chh::wire::Packet packet(chh::wire::Type::private_delivery);
        if (!packet.add_u64(message.id) || !packet.add_number(message.timestamp) || !packet.add_string(message.sender_id, 64) ||
            !packet.add_string(message.nonce, 32) || !packet.add_number(message.flags) ||
            !packet.add_string(message.body, chh::wire::chat_text_limit) || !send_packet(stream, packet)) return false;
        social.delivery.sent(message.id, now);
    }
    // Messages go first, so an event never overtakes the message it recalls.
    for (const auto& event : inbox.recalls) {
        chh::wire::Packet packet(chh::wire::Type::private_recall_event);
        if (!packet.add_u64(event.id) || !packet.add_string(event.sender_id, 64) || !packet.add_u64(event.message_id) ||
            !send_packet(stream, packet)) return false;
        social.recall.sent(event.id, now);
    }
    if ((messages && inbox.messages.size() == messages) || (recalls && inbox.recalls.size() == recalls)) {
        social.next_delivery = {}; wait_ms = 1;
    }
    // Rights last, after the messages that carry the same hashes: "download
    // this image, store it, then acknowledge it". A hash is pushed at most once
    // per connection while it is outstanding.
    if (pending_rights) {
        rights.revision = conversation_revision; rights.next_page = now + std::chrono::seconds(30);
        rights.next_read = now + std::chrono::seconds(1);
        std::vector<chh::server::PrivateMediaRight> page;
        const auto result = chh::server::private_media_rights(actor, rights.cursor, pending_rights, page, &diagnostic);
        report_social_database_result(social, "读取待下载私聊图片", result, diagnostic);
        if (result != chh::wire::SocialResult::ok) {
            rights.page_retry = now + std::chrono::seconds(5); rights.revision = UINT64_MAX; return true;
        }
        for (const auto& right : page) {
            rights.cursor = right.id;
            if (rights.outstanding.count(right.hash)) continue;
            chh::wire::Packet packet(chh::wire::Type::private_media_pending);
            if (!packet.add_string(right.hash, 64) || !send_packet(stream, packet)) return false;
            rights.outstanding.emplace(right.hash, now);
        }
        rights.more = page.size() == pending_rights;
        // A full batch reads on at once while the window has room; a page cut
        // short by the window waits a second, so acknowledgements that free
        // slots meanwhile share one read.
        if (rights.more && pending_rights == private_delivery_batch) { rights.next_read = {}; wait_ms = 1; }
    }
    return true;
}
// One export step per request, paced like private_open; a failed database
// call is retried rather than answered, because the wire has no error frame.
bool export_private(chh::tls::Stream& stream, SocialState& social, const std::string& actor) {
    const auto now = Clock::now();
    if (!social.export_request || now < social.next_export) return true;
    std::string diagnostic;
    if (social.export_finish) {
        const auto result = chh::server::complete_private_export(actor, social.export_value, &diagnostic);
        report_social_database_result(social, "确认旧私聊导出完成", result, diagnostic);
        if (result != chh::wire::SocialResult::ok) { social.next_export = now + std::chrono::seconds(5); return true; }
        social.export_request = 0; social.next_export = now + std::chrono::milliseconds(500);
        social.export_recorded = true;
        // A running pass rechecks this completion on its next batch.
        if (!social.purging) { social.purge = {}; social.purging = true; social.next_purge = {}; }
        return true;
    }
    chh::server::PrivateExportPage page;
    const auto result = chh::server::private_export_page(actor, social.export_value, page, &diagnostic);
    report_social_database_result(social, "导出旧私聊记录", result, diagnostic);
    if (result != chh::wire::SocialResult::ok) { social.next_export = now + std::chrono::seconds(5); return true; }
    chh::wire::Packet begin(chh::wire::Type::private_export_begin);
    if (!begin.add_number(social.export_request) || !begin.add_number(static_cast<uint32_t>(page.entries.size())) ||
        !send_packet(stream, begin)) return false;
    for (const auto& entry : page.entries) {
        chh::wire::Packet packet(chh::wire::Type::private_export_entry);
        if (!packet.add_u64(entry.id) || !packet.add_number(entry.timestamp) || !packet.add_string(entry.sender_id, 64) ||
            !packet.add_string(entry.recipient_id, 64) || !packet.add_number(entry.flags) ||
            !packet.add_string(entry.body, chh::wire::chat_text_limit) || !send_packet(stream, packet)) return false;
    }
    chh::wire::Packet end(chh::wire::Type::private_export_end);
    if (!end.add_number(social.export_request) || !end.add_u64(page.last_id) || !end.add_number(page.more ? 1u : 0u) ||
        !send_packet(stream, end)) return false;
    social.export_request = 0; social.next_export = now + std::chrono::milliseconds(500);
    return true;
}
// Legacy rows that both participants have exported are removed in bounded
// batches, after this account's own completion and once per connection.
void purge_private(SocialState& social, const std::string& actor) {
    const auto now = Clock::now();
    if (!social.purging || now < social.next_purge) return;
    std::string diagnostic;
    std::vector<std::string> released;
    const auto result = chh::server::purge_private_legacy(actor, social.purge, &diagnostic, &released);
    report_social_database_result(social, "清理已导出的旧私聊", result, diagnostic);
    chh::server::media_collector.candidates(released);
    if (result == chh::wire::SocialResult::unavailable) social.next_purge = now + std::chrono::seconds(30);
    else if (result != chh::wire::SocialResult::ok || social.purge.finished) social.purging = false;
    else social.next_purge = now + std::chrono::seconds(1);
}

// Independent of social.peer: the connection thread alone sends its own notices.
template<class Send>
bool refresh_private_notices(SocialState& social, const std::string& actor, int& wait_ms, Send send) {
    if (!social.joined || !social.notices_enabled || Clock::now() < social.next_notice) return true;
    social.next_notice = Clock::now() + std::chrono::seconds(1);
    std::string diagnostic;
    if (!social.notice_cursor_known) {
        uint64_t baseline = 0;
        const auto result = chh::server::private_notice_baseline(actor, baseline, &diagnostic);
        report_social_database_result(social, "初始化私聊通知", result, diagnostic);
        if (result != chh::wire::SocialResult::ok) {
            social.next_notice = Clock::now() + std::chrono::seconds(5);
            return true;
        }
        if (!send(baseline, std::string{})) return false;
        social.notice_cursor = baseline; social.notice_cursor_known = true;
        return true;
    }
    chh::server::PrivateNoticeBatch batch;
    const auto result = chh::server::private_notices_since(actor, social.notice_cursor, batch, &diagnostic);
    report_social_database_result(social, "轮询私聊通知", result, diagnostic);
    if (result != chh::wire::SocialResult::ok) {
        social.next_notice = Clock::now() + std::chrono::seconds(5);
        return true;
    }
    for (const auto& notice : batch.notices)
        if (!send(notice.id, notice.peer)) return false;
    // Progress across cleared rows is carried separately, so reconnect never
    // repeatedly scans the same suppressed messages and never marks them unread.
    if (batch.cursor > social.notice_cursor && !send(batch.cursor, std::string{})) return false;
    social.notice_cursor = batch.cursor;
    if (batch.has_more) { social.next_notice = {}; wait_ms = 1; }
    return true;
}

bool send_private_message(chh::tls::Stream& stream, const chh::server::PrivateMessage& message,
    const std::string& actor, const std::string& peer, chh::server::MediaSession* media = nullptr) {
    chh::wire::Packet packet(chh::wire::Type::private_message);
    const bool sent = packet.add_u64(message.id) && packet.add_number(message.timestamp) &&
        packet.add_number(message.sender_id == actor ? 1u : 0u) && packet.add_string(peer, 64) &&
        packet.add_string(message.text, chh::wire::chat_text_limit) &&
        (!message.recalled || packet.add_number(1u)) && send_packet(stream, packet);
    if (sent && media) media->observe_private(message, peer);
    return sent;
}
bool send_private_recalled(chh::tls::Stream& stream, const std::string& peer, uint64_t id, uint32_t timestamp) {
    chh::wire::Packet packet(chh::wire::Type::private_recalled);
    return packet.add_string(peer, 64) && packet.add_u64(id) && packet.add_number(timestamp) && send_packet(stream, packet);
}
bool send_private_cleared(chh::tls::Stream& stream, const std::string& peer, uint64_t through) {
    chh::wire::Packet packet(chh::wire::Type::private_cleared);
    return packet.add_string(peer, 64) && packet.add_u64(through) && send_packet(stream, packet);
}
bool send_private_page(chh::tls::Stream& stream, uint32_t request, const std::string& actor,
    const std::string& peer, const chh::server::PrivatePage& page, chh::server::MediaSession* media = nullptr) {
    chh::wire::Packet begin(chh::wire::Type::private_begin);
    if (!begin.add_number(request) || !begin.add_string(peer, 64) ||
        !begin.add_number(static_cast<uint32_t>(page.messages.size())) || !begin.add_number(page.has_older ? 1u : 0u) ||
        !send_packet(stream, begin)) return false;
    for (const auto& message : page.messages)
        if (!send_private_message(stream, message, actor, peer, media)) return false;
    chh::wire::Packet end(chh::wire::Type::private_end);
    return end.add_number(request) && end.add_string(peer, 64) && send_packet(stream, end);
}
bool private_request_text(const chh::wire::Packet& packet, size_t offset, std::string& text) {
    if (offset + 2 > packet.length()) return false;
    const auto* bytes = packet.bytes.data() + 8 + offset;
    const size_t size = (size_t(bytes[0]) << 8) | bytes[1];
    if (offset + 2 + size != packet.length()) return false;
    text.assign(reinterpret_cast<const char*>(bytes + 2), size); return true;
}
using GamePlayer = chh::server::Sessions::Game::Player;
// begin(count), one entry per player (fields added by write), end.
template<class Write>
bool send_roster(chh::tls::Stream& stream, chh::wire::Type begin_type, chh::wire::Type entry_type,
    chh::wire::Type end_type, const std::vector<GamePlayer>& players, Write write) {
    if (!send_numbers(stream, begin_type, {static_cast<uint32_t>(players.size())})) return false;
    for (const auto& player : players) {
        chh::wire::Packet entry(entry_type);
        if (!write(entry, player) || !send_packet(stream, entry)) return false;
    }
    return send_packet(stream, chh::wire::Packet(end_type));
}
// A roster uploaded by the game endpoint: begin(count), entries, end.
struct IncomingRoster {
    std::vector<GamePlayer> players;
    uint32_t expected = 0;
    bool loading = false;
    void begin(uint32_t count) { players.clear(); players.reserve(count); expected = count; loading = true; }
    bool full() const { return players.size() >= expected; }
    bool complete() const { return loading && players.size() == expected; }
    std::vector<GamePlayer> take() {
        auto result = std::move(players);
        players.clear(); expected = 0; loading = false;
        return result;
    }
};
bool serve_update_notes(chh::tls::Stream& stream, const chh::wire::Packet& request) {
    std::string version, content;
    chh::wire::Reader reader{request};
    if (!reader.string(version, chh::wire::app_version_limit) || !reader.done() ||
        !chh::wire::valid_app_version(version)) {
        send_error(stream, chh::wire::Error::protocol);
        return false;
    }
    if (!chh::server::latest_update_content(version, content)) content.clear();
    chh::wire::Packet begin(chh::wire::Type::update_notes_begin);
    if (!begin.add_string(version, chh::wire::app_version_limit) ||
        !begin.add_number(static_cast<uint32_t>(content.size())) || !send_packet(stream, begin)) return false;
    for (size_t offset = 0; offset < content.size();) {
        const size_t size = (std::min)(content.size() - offset, chh::wire::payload_size);
        chh::wire::Packet chunk(chh::wire::Type::update_notes_chunk);
        if (!chunk.add_bytes(reinterpret_cast<const uint8_t*>(content.data() + offset), size) ||
            !send_packet(stream, chunk)) return false;
        offset += size;
    }
    return send_packet(stream, chh::wire::Packet(chh::wire::Type::update_notes_end));
}
void serve(SOCKET socket, unsigned long ip, chh::tls::Credential& credential) {
    chh::tls::Stream stream(socket, &stopping);
    if (!stream.accept(credential, 10000)) return;
    if (limiter.blocked(ip)) { send_error(stream, chh::wire::Error::busy); return; }
    chh::wire::Packet request;
    struct ClearPacket { chh::wire::Packet& p; ~ClearPacket() { SecureZeroMemory(p.bytes.data(), p.bytes.size()); } } clear{request};
    if (!stream.read_exact(request.bytes.data(), request.bytes.size(), 10000)) return;
    // Public optional metadata uses its own pinned TLS connection. Admission
    // limits above still apply; one request finishes before any login handling.
    if (request.type() == chh::wire::Type::update_notes_request) {
        serve_update_notes(stream, request);
        return;
    }
    std::string id, password, machine, login_key, client_version;
    uint32_t client_kind = 0;
    struct ClearPassword { std::string& p; ~ClearPassword() { if (!p.empty()) SecureZeroMemory(p.data(), p.size()); } } clear_password{password};
    chh::wire::Reader reader{request};
    bool login_shape_valid = request.type() == chh::wire::Type::login && reader.string(id, 64);
    // A game client may use the compact accountID + client-kind form. Empty
    // credential placeholders remain accepted for compatibility with the
    // previous fixed five-field packet used by early game builds.
    if (login_shape_valid && request.length() - reader.offset == sizeof(uint32_t)) {
        login_shape_valid = reader.number(client_kind);
    } else if (login_shape_valid) {
        login_shape_valid = reader.string(password, 256, true) && reader.string(machine, 64, true) &&
            reader.string(login_key, chh::login_proof::key_limit, true) && reader.number(client_kind);
    }
    if (!login_shape_valid ||
        (client_kind != uint32_t(chh::wire::ClientKind::primary) && client_kind != uint32_t(chh::wire::ClientKind::secondary))) {
        send_error(stream, chh::wire::Error::protocol); return;
    }
    const bool primary_client = client_kind == uint32_t(chh::wire::ClientKind::primary);
    // The primary client proves the account with the normal password, machine
    // code and login key. The game client deliberately sends only accountID;
    // its admission is tied to an already-authenticated primary session below.
    if (primary_client) {
        if (password.empty() || !chh::login_proof::valid_request(login_key, machine)) {
            send_error(stream, chh::wire::Error::protocol); return;
        }
    } else if (!password.empty() || !machine.empty() || !login_key.empty()) {
        send_error(stream, chh::wire::Error::protocol); return;
    }
    // Missing version identifies an older primary client and receives the same
    // explicit update response as a mismatched version. Game packets end after
    // the client type field; they never carry a version or credentials.
    if (primary_client) {
        if (!reader.done() &&
            (!reader.string(client_version, chh::wire::app_version_limit, true) || !reader.done())) {
            send_error(stream, chh::wire::Error::protocol); return;
        }
    }
    else if (!reader.done()) {
        send_error(stream, chh::wire::Error::protocol); return;
    }
    const auto client_role = primary_client ? chh::server::ClientRole::primary : chh::server::ClientRole::secondary;
    chh::server::AccountProfile profile;
    chh::server::AuthAuditInfo audit;
    SecureZeroMemory(request.bytes.data(), request.bytes.size());
    const auto record_result = [&](chh::server::AuthResult outcome) {
        std::string diagnostic;
        if (!chh::server::record_login(id, password, machine, audit, outcome, primary_client, &diagnostic)) {
            // Never put credentials, account metadata or MySQL error text in console logs.
            std::cerr << ("登录记录写入失败：" + diagnostic + "\n");
        }
        if (!password.empty()) SecureZeroMemory(password.data(), password.size());
        password.clear();
    };
    if (stopping.load()) return;
    if (primary_client) {
        std::string latest_version, diagnostic;
        if (!chh::server::latest_app_version(latest_version, &diagnostic)) {
            std::cerr << ("版本检查失败：" + diagnostic + "\n");
            send_error(stream, chh::wire::Error::version_unavailable); return;
        }
        if (!chh::wire::valid_app_version(client_version) || client_version != latest_version) {
            chh::wire::Packet update(chh::wire::Type::update_required);
            if (update.add_string(latest_version, chh::wire::app_version_limit))
                send_packet(stream, update);
            // No account lookup, credential failure penalty or online admission
            // occurs for version refusal. Audit I/O follows the response.
            record_result(chh::server::AuthResult::update_required);
            return;
        }
    }
    std::unique_lock authentication_lock(account_gate(id));
    if (primary_client) {
        const auto auth_result = chh::server::authenticate_account(id, password, machine, profile, &audit);
        if (auth_result != chh::server::AuthResult::ok) {
            authentication_lock.unlock();
            chh::wire::Error error = chh::wire::Error::unavailable;
            switch (auth_result) {
            case chh::server::AuthResult::account_not_found: error = chh::wire::Error::account_not_found; break;
            case chh::server::AuthResult::password_mismatch: error = chh::wire::Error::password_mismatch; break;
            case chh::server::AuthResult::machine_mismatch: error = chh::wire::Error::machine_mismatch; break;
            case chh::server::AuthResult::expired: error = chh::wire::Error::expired; break;
            case chh::server::AuthResult::banned: error = chh::wire::Error::banned; break;
            default: break;
            }
            if (chh::wire::is_auth_error(error)) limiter.blocked(ip, true);
            send_error(stream, error);
            if (auth_result != chh::server::AuthResult::unavailable) record_result(auth_result);
            return;
        }
    } else {
        // The game client cannot authenticate by itself. Holding the same
        // account gate as primary admission makes the check and enter() atomic
        // with respect to a primary disconnect or replacement login.
        if (!online.primary_online(id, ip)) {
            authentication_lock.unlock();
            send_error(stream, chh::wire::Error::session_limit);
            return;
        }
        profile.account_id = id;
        const auto primary_profile = online.primary_profile(id, ip);
        if (primary_profile.name.empty()) {
            authentication_lock.unlock();
            send_error(stream, chh::wire::Error::session_limit);
            return;
        }
        profile.name = primary_profile.name;
    }
    chh::wire::Packet response(chh::wire::Type::profile);
    if (primary_client) {
        std::string proof;
        if (!chh::login_proof::make_response(login_key,
                {id, machine, client_version, profile.name, profile.expiration}, proof) ||
            !response.add_string(profile.name, 512) || profile.name.empty() ||
            !response.add_string(profile.expiration, 19) || profile.expiration.empty() ||
            !response.add_string(proof, chh::login_proof::response_limit)) {
            send_error(stream, chh::wire::Error::unavailable); return;
        }
    } else if (!response.add_string(std::string{}, 512) ||
        !response.add_string(std::string{}, 19) ||
        !response.add_string(std::string{}, chh::login_proof::response_limit)) {
        send_error(stream, chh::wire::Error::unavailable); return;
    }
    Membership member{online.enter(profile.account_id, ip, client_role, profile.name, static_cast<uintptr_t>(socket)), true};
    if (member.binding && primary_client) {
        if (!online_time.start(profile.account_id, member.binding.token))
            report_online_time_error("主客户端计时会话冲突");
        bool hidden = true;
        const bool ready = chh::server::get_account_preferences(profile.account_id, hidden) == chh::wire::AccountResult::ok;
        apply_account_preferences(profile.account_id, ready, hidden);
    }
    authentication_lock.unlock();
    member.admitting = false;
    if (!member.binding) { send_error(stream, chh::wire::Error::session_limit); return; }
    if (!send_packet(stream, response)) return;
    bool audit_recorded = false;
    uint64_t sent_revision = UINT64_MAX;
    uint64_t sent_game_revision = UINT64_MAX;
    uint64_t sent_sight_revision = UINT64_MAX;
    uint64_t sent_sight_command_revision = UINT64_MAX;
    uint64_t sent_skill_hint_command_revision = UINT64_MAX;
    uint64_t sent_status_display_command_revision = UINT64_MAX;
    uint64_t sent_auto_cleanse_command_revision = UINT64_MAX;
    uint64_t sent_auto_guisuo_command_revision = UINT64_MAX;
    std::array<uint64_t, chh::wire::game_auto_guisuo_side_count> sent_auto_guisuo_roster_revisions;
    std::array<uint64_t, chh::wire::game_auto_guisuo_side_count> sent_auto_guisuo_exclusions_revisions;
    sent_auto_guisuo_roster_revisions.fill(UINT64_MAX);
    sent_auto_guisuo_exclusions_revisions.fill(UINT64_MAX);
    uint64_t sent_auto_dihuo_command_revision = UINT64_MAX;
    uint64_t sent_auto_dihuo_roster_revision = UINT64_MAX;
    uint64_t sent_auto_dihuo_exclusions_revision = UINT64_MAX;
    uint64_t sent_auto_douji_command_revision = UINT64_MAX;
    uint64_t sent_auto_douji_roster_revision = UINT64_MAX;
    uint64_t sent_auto_douji_exclusions_revision = UINT64_MAX;
    uint64_t sent_auto_dark_shield_command_revision = UINT64_MAX;
    uint64_t sent_auto_dark_shield_roster_revision = UINT64_MAX;
    uint64_t sent_auto_dark_shield_exclusions_revision = UINT64_MAX;
    uint64_t sent_auto_heal_command_revision = UINT64_MAX;
    uint64_t sent_auto_heal_roster_revision = UINT64_MAX;
    uint64_t sent_auto_heal_exclusions_revision = UINT64_MAX;
    uint64_t sent_auto_disarm_command_revision = UINT64_MAX;
    uint64_t sent_auto_disarm_roster_revision = UINT64_MAX;
    uint64_t sent_auto_disarm_exclusions_revision = UINT64_MAX;
    std::array<uint64_t, chh::wire::game_near_item_count> sent_near_item_command_revisions;
    std::array<uint64_t, chh::wire::game_near_item_count> sent_near_item_roster_revisions;
    std::array<uint64_t, chh::wire::game_near_item_count> sent_near_item_exclusions_revisions;
    sent_near_item_command_revisions.fill(UINT64_MAX);
    sent_near_item_roster_revisions.fill(UINT64_MAX);
    sent_near_item_exclusions_revisions.fill(UINT64_MAX);
    uint64_t sent_training_mode_command_revision = UINT64_MAX;
    uint64_t sent_auto_smite_command_revision = UINT64_MAX;
    uint64_t sent_jungle_timer_command_revision = UINT64_MAX;
    uint64_t sent_eye_timer_command_revision = UINT64_MAX;
    uint64_t sent_trap_timer_command_revision = UINT64_MAX;
    uint64_t sent_disappearance_hint_command_revision = UINT64_MAX;
    uint64_t sent_movement_trace_command_revision = UINT64_MAX;
    uint64_t sent_tower_range_command_revision = UINT64_MAX;
    uint64_t sent_attack_range_command_revision = UINT64_MAX;
    uint64_t sent_auto_afk_command_revision = UINT64_MAX;
    uint64_t sent_move_distance_command_revision = UINT64_MAX;
    uint64_t sent_hero_keys_command_revision = UINT64_MAX;
    uint64_t sent_hero_delay_command_revision = UINT64_MAX;
    uint64_t sent_skin_block_command_revision = UINT64_MAX;
    uint64_t sent_projectile_display_command_revision = UINT64_MAX;
    uint64_t sent_auto_dodge_command_revision = UINT64_MAX;
    uint64_t sent_low_health_hint_command_revision = UINT64_MAX;
    uint64_t sent_instant_pick_command_revision = UINT64_MAX;
    uint64_t sent_auto_accept_command_revision = UINT64_MAX;
    uint64_t sent_selection_shout_command_revision = UINT64_MAX;
    uint64_t sent_auto_ignite_command_revision = UINT64_MAX;
    uint64_t sent_auto_fairy_command_revision = UINT64_MAX;
    uint64_t sent_auto_devour_command_revision = UINT64_MAX;
    uint64_t sent_auto_glove_command_revision = UINT64_MAX;
    uint64_t sent_auto_lingzhou_command_revision = UINT64_MAX;
    uint64_t sent_hero_select_command_revision = UINT64_MAX;
    uint64_t sent_game_close_revision = UINT64_MAX;
    uint64_t sent_team_command_revision = UINT64_MAX;
    uint64_t sent_players_revision = UINT64_MAX;
    uint64_t sent_players_request_revision = UINT64_MAX;
    uint64_t sent_output_revision = UINT64_MAX;
    uint64_t sent_output_request_revision = UINT64_MAX;
    IncomingRoster incoming_players, incoming_output;
    uint32_t incoming_stats_request_id = 0, incoming_stats_expected = 0;
    std::vector<chh::server::Sessions::Game::StatsEntry> incoming_stats;
    bool incoming_stats_loading = false;
    bool chat_joined = false, chat_reset_requested = false, chat_reset_active = false;
    bool account_joined = false, account_preferences = false, account_online_time = false;
    AccountOnlineTime account_online_state;
    uint64_t sent_account_revision = UINT64_MAX;
    uint64_t sent_visibility_revision = UINT64_MAX;
    Clock::time_point next_preferences_retry = Clock::now() + std::chrono::seconds(5);
    Clock::time_point next_hero_request{};
    Clock::time_point next_hero_permissions_request{};
    std::vector<uint32_t> available_hero_ids;
    Clock::time_point available_heroes_expires{};
    // Creating notes grows a shared table; team actions query friends and
    // push commands into another player's game. Both are paced per connection.
    Clock::time_point next_note_create{}, next_team_request{};
    uint64_t chat_cursor = 0;
    uint64_t chat_recall_revision = 0;
    chh::server::ChatRoom::Batch chat_history;
    size_t chat_history_offset = 0;
    chh::server::ChatRateLimit chat_rate;
    chh::server::ChatRateLimit chat_recall_rate;
    SocialState social;
    chh::server::AvatarSession avatars(profile.account_id);
    chh::server::MediaSession media(profile.account_id);
    const auto send_avatar = [&](const chh::wire::Packet& packet) { return send_packet(stream, packet); };
    // Re-checked after waiting on the gate or the database: a replaced or
    // invalidated session must stop acting for the account.
    const auto still_primary = [&] {
        if (online.current_primary(member.binding)) return true;
        send_error(stream, chh::wire::Error::credentials_changed);
        return false;
    };
    const auto send_chat = [&](const chh::server::ChatRoom::Message& message) {
        if (!send_chat_message(stream, message, profile.account_id, social.enhanced, social.preferences)) return false;
        avatars.observe_chat(message);
        media.observe_chat(message);
        return true;
    };
    const auto lobby_image_visible = [&](const std::string& hash) { return chat_joined && lobby.contains_image(hash); };
    // bytes: a fresh upload, kept in lobby memory beside the message; null for
    // an image already in memory or a favourite served from the database.
    const auto publish_image = [&](const std::string& hash, const chh::server::ChatRoom::Bytes& bytes) {
        if (bytes && !lobby.image_fits(bytes->size())) return chh::wire::MediaResult::too_large;
        std::lock_guard gate(account_gate(profile.account_id));
        if (!online.current_primary(member.binding)) return chh::wire::MediaResult::unavailable;
        const auto current = online.profile_snapshot(member.binding);
        profile.name = current.name;
        const auto timestamp = unix_seconds32();
        return timestamp && lobby.publish(profile.account_id, profile.name, "[图片]", *timestamp, hash, current.hide_account_id, bytes) ?
            chh::wire::MediaResult::ok : chh::wire::MediaResult::unavailable;
    };
    const auto lobby_image_bytes = [&](const std::string& hash) {
        return chat_joined ? lobby.image(hash) : chh::server::ChatRoom::Bytes{};
    };
    const auto private_image_saved = [&](uint32_t request, const std::string& peer, const std::string& nonce,
        uint64_t id, uint32_t timestamp, bool inserted) {
        if (inserted) {
            online.conversation_changed(profile.account_id, peer); social.next_refresh = {};
            if (social.peer == peer) social.next_private = {};
        }
        return !social.local_history || send_private_stored(stream, request, 1, id, timestamp, peer, nonce);
    };
    const auto accepted_friend = [&](const std::string& peer) {
        std::vector<chh::server::FriendRecord> entries;
        std::string diagnostic;
        if (chh::server::list_friends(profile.account_id, entries, &diagnostic) != chh::wire::SocialResult::ok)
            return false;
        return std::any_of(entries.begin(), entries.end(), [&](const auto& entry) {
            return entry.id == peer && entry.relation == chh::wire::FriendRelation::accepted;
        });
    };
    const auto sync_private_clears = [&]() {
        const auto changes = online.private_clears(member.binding, social.clear_revision);
        if (changes.reset) {
            // A stalled old client cannot discard an unknown set of cached peers.
            if (!social.clear_enabled && !social.peer.empty()) return false;
            if (social.clear_enabled) { social.clears_loaded = false; social.clear_after_peer.clear(); }
        }
        for (const auto& event : changes.changes) {
            if (social.peer == event.peer) {
                social.peer.clear(); social.private_cursor = 0; media.reset_private({});
                if (!social.clear_enabled) return false;
            }
            if (social.clear_enabled && !send_private_cleared(stream, event.peer, event.through)) return false;
        }
        if (changes.revision != social.clear_revision) social.next_refresh = {};
        social.clear_revision = changes.revision;
        if (social.clear_enabled && !social.clears_loaded) {
            std::vector<chh::server::PrivateClear> rows;
            bool has_more = false; std::string diagnostic;
            const auto code = chh::server::list_private_clears(profile.account_id, social.clear_after_peer, rows, has_more, &diagnostic);
            report_social_database_result(social, "同步私聊清除记录", code, diagnostic);
            if (code != chh::wire::SocialResult::ok) {
                // Never keep displaying an unverified pre-reconnect private cache.
                return false;
            }
            for (const auto& row : rows)
                if (row.through && !send_private_cleared(stream, row.peer_id, row.through)) return false;
            if (!rows.empty()) social.clear_after_peer = rows.back().peer_id;
            social.clears_loaded = !has_more;
        }
        return true;
    };
    const auto media_revision = [&]() {
        // Both components are monotonic within this connection. Clear only
        // invalidates media grants for the owner, without global friend reloads.
        return social_revision.load() + online.private_clears(member.binding, UINT64_MAX).revision;
    };
    auto last_received = Clock::now();
    while (!stopping.load()) {
        if (!online.current(member.binding)) { send_error(stream, chh::wire::Error::credentials_changed); break; }
        if (!sync_private_clears()) break;
        if (social.joined && online.current_primary(member.binding)) {
            const auto recalls = online.private_recalls(member.binding, social.recall_revision);
            if (recalls.reset) social.recall_revision = recalls.revision;
            else {
                for (const auto& event : recalls.changes)
                    if (!send_private_recalled(stream, event.peer, event.id, event.timestamp)) { social.joined = false; break; }
                if (!social.joined) break;
                social.recall_revision = recalls.revision;
            }
        }
        auto latest_profile = online.profile_snapshot(member.binding);
        if (latest_profile.name.empty()) { send_error(stream, chh::wire::Error::credentials_changed); break; }
        profile.name = latest_profile.name;
        if (primary_client && !latest_profile.preferences_ready && Clock::now() >= next_preferences_retry) {
            next_preferences_retry = Clock::now() + std::chrono::seconds(5);
            std::lock_guard gate(account_gate(profile.account_id));
            if (!still_primary()) break;
            bool hidden = true;
            if (chh::server::get_account_preferences(profile.account_id, hidden) == chh::wire::AccountResult::ok) {
                apply_account_preferences(profile.account_id, true, hidden);
                latest_profile = online.profile_snapshot(member.binding);
            }
        }
        const bool account_time_refreshed = account_joined && account_online_time &&
            account_online_state.refresh(profile.account_id, Clock::now());
        if (account_joined && (latest_profile.revision != sent_account_revision || account_time_refreshed)) {
            if (!send_account_state(stream, profile, latest_profile, account_preferences,
                account_online_time ? &account_online_state : nullptr)) break;
            sent_account_revision = latest_profile.revision;
        }
        const auto count = online.snapshot();
        if (count.revision != sent_revision) {
            chh::wire::Packet packet(chh::wire::Type::online); packet.set_number(count.accounts);
            if (!send_packet(stream, packet)) break;
            sent_revision = count.revision;
            if (primary_client && !audit_recorded) {
                // Only primary connection logins are audited here. Game logins
                // are recorded separately when a 0x5BDE event arrives.
                record_result(chh::server::AuthResult::ok);
                audit_recorded = true;
            }
        }
        if (member.binding.role == chh::server::ClientRole::primary) {
            if (const auto expired = online.expire_explorer_request(member.binding)) {
                std::ostringstream output;
                output << "explorer.exe 请求 " << expired << " 等待结果超过6分钟，sock：" << socket
                    << "；实际运行状态未知，未自动重试。\n";
                std::cout << output.str() << std::flush;
            }
            if (const auto request_id = online.take_explorer_request(member.binding)) {
                chh::wire::Packet command(chh::wire::Type::explorer_launch);
                if (!command.add_u64(request_id) || !send_packet(stream, command)) break;
            }
            const auto game = online.game_snapshot(member.binding);
            if (game.revision != sent_game_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_status);
                packet.set_game_status(game.uid, game.state, game.connected,
                    game.available_virtual_memory, game.player_name, game.map_id, game.mode,
                    game.team ? 1u : 0u);
                if (!packet.add_u64(game.game_connection_id) || !send_packet(stream, packet)) break;
                sent_game_revision = game.revision;
            }
            // Both frames describe the same locked snapshot, including its request id.
            const auto disarm_roster = online.auto_disarm_roster_snapshot(member.binding);
            if (disarm_roster.ready && game.connected &&
                disarm_roster.game_connection_id == game.game_connection_id) {
                const bool roster_changed = disarm_roster.roster_revision != sent_auto_disarm_roster_revision;
                if (roster_changed) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_disarm_roster);
                    if (!chh::wire::make_game_auto_disarm_roster(disarm_roster.roster, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_disarm_roster_revision = disarm_roster.roster_revision;
                }
                if (roster_changed || disarm_roster.exclusions_revision != sent_auto_disarm_exclusions_revision) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_disarm_exclusions_set);
                    if (!chh::wire::make_game_auto_disarm_exclusions(disarm_roster.exclusions, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_disarm_exclusions_revision = disarm_roster.exclusions_revision;
                }
            }
            // Lists are copied only when their revision moved past the one already sent.
            // Near-item roster snapshots share the game status token above.
            bool near_items_sent = true;
            for (uint32_t item_slot = 1; item_slot <= chh::wire::game_near_item_count; ++item_slot) {
                const auto item = online.near_item_roster_snapshot(member.binding, item_slot);
                if (!item.ready || !game.connected || item.game_connection_id != game.game_connection_id) continue;
                auto& sent_roster = sent_near_item_roster_revisions[item_slot - 1];
                auto& sent_exclusions = sent_near_item_exclusions_revisions[item_slot - 1];
                const bool roster_changed = item.roster_revision != sent_roster;
                if (roster_changed) {
                    chh::wire::Packet packet;
                    const bool encoded = item_slot == 1 ? chh::wire::make_game_near_item1_roster(item.roster, packet) :
                        chh::wire::make_game_near_item_roster(item_slot, item.roster, packet);
                    if (!encoded || !send_packet(stream, packet)) { near_items_sent = false; break; }
                    sent_roster = item.roster_revision;
                }
                if (roster_changed || item.exclusions_revision != sent_exclusions) {
                    chh::wire::Packet packet;
                    const bool encoded = item_slot == 1 ? chh::wire::make_game_near_item1_exclusions(item.exclusions, packet) :
                        chh::wire::make_game_near_item_exclusions(item_slot, item.exclusions, packet);
                    if (!encoded || !send_packet(stream, packet)) { near_items_sent = false; break; }
                    sent_exclusions = item.exclusions_revision;
                }
            }
            if (!near_items_sent) break;
            const auto players = online.game_players_snapshot(member.binding, sent_players_revision);
            if (players.initialized && players.revision != sent_players_revision) {
                if (!send_roster(stream, chh::wire::Type::game_players_begin, chh::wire::Type::game_player_entry,
                    chh::wire::Type::game_players_end, players.players, [&](chh::wire::Packet& entry, const GamePlayer& player) {
                        const auto la_owner = online.game_uid_owner(player.uid);
                        return entry.add_number(player.uid) && entry.add_number(player.team) &&
                            entry.add_string(player.name, chh::wire::profile_name_limit) &&
                            entry.add_number(player.level) && entry.add_number(player.vip) &&
                            entry.add_number(player.competitive_power) &&
                            entry.add_number(la_owner.user_name.empty() ? 0u : 1u) &&
                            entry.add_string(chh::server::roster_display_name(la_owner.user_name), chh::wire::profile_name_limit) &&
                            entry.add_string(la_owner.account_id, 64) &&
                            entry.add_number(la_owner.hide_account_id ? 1u : 0u) &&
                            entry.add_number(player.identification_code) &&
                            entry.add_number(player.hero_id) &&
                            entry.add_number(player.damage_dealt);
                    })) break;
                sent_players_revision = players.revision;
            }
            bool guisuo_rosters_sent = true;
            for (uint32_t side = 0; side < chh::wire::game_auto_guisuo_side_count; ++side) {
                const auto target = online.auto_guisuo_roster_snapshot(member.binding, side);
                if (!target.ready || !game.connected || target.game_connection_id != game.game_connection_id) continue;
                auto& sent_roster = sent_auto_guisuo_roster_revisions[side];
                auto& sent_exclusions = sent_auto_guisuo_exclusions_revisions[side];
                const bool roster_changed = target.roster_revision != sent_roster;
                if (roster_changed) {
                    chh::wire::Packet packet;
                    if (!chh::wire::make_game_auto_guisuo_roster(side, target.roster, packet) ||
                        !send_packet(stream, packet)) { guisuo_rosters_sent = false; break; }
                    sent_roster = target.roster_revision;
                }
                if (roster_changed || target.exclusions_revision != sent_exclusions) {
                    chh::wire::Packet packet;
                    if (!chh::wire::make_game_auto_guisuo_exclusions(side, target.exclusions, packet) ||
                        !send_packet(stream, packet)) { guisuo_rosters_sent = false; break; }
                    sent_exclusions = target.exclusions_revision;
                }
            }
            if (!guisuo_rosters_sent) break;
            const auto dihuo_roster = online.auto_dihuo_roster_snapshot(member.binding);
            if (dihuo_roster.ready && game.connected &&
                dihuo_roster.game_connection_id == game.game_connection_id) {
                const bool roster_changed = dihuo_roster.roster_revision != sent_auto_dihuo_roster_revision;
                if (roster_changed) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_dihuo_roster);
                    if (!chh::wire::make_game_auto_dihuo_roster(dihuo_roster.roster, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_dihuo_roster_revision = dihuo_roster.roster_revision;
                }
                if (roster_changed || dihuo_roster.exclusions_revision != sent_auto_dihuo_exclusions_revision) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_dihuo_exclusions_set);
                    if (!chh::wire::make_game_auto_dihuo_exclusions(dihuo_roster.exclusions, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_dihuo_exclusions_revision = dihuo_roster.exclusions_revision;
                }
            }
            const auto douji_roster = online.auto_douji_roster_snapshot(member.binding);
            if (douji_roster.ready && game.connected &&
                douji_roster.game_connection_id == game.game_connection_id) {
                const bool roster_changed = douji_roster.roster_revision != sent_auto_douji_roster_revision;
                if (roster_changed) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_douji_roster);
                    if (!chh::wire::make_game_auto_douji_roster(douji_roster.roster, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_douji_roster_revision = douji_roster.roster_revision;
                }
                if (roster_changed || douji_roster.exclusions_revision != sent_auto_douji_exclusions_revision) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_douji_exclusions_set);
                    if (!chh::wire::make_game_auto_douji_exclusions(douji_roster.exclusions, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_douji_exclusions_revision = douji_roster.exclusions_revision;
                }
            }
            const auto dark_shield_roster = online.auto_dark_shield_roster_snapshot(member.binding);
            if (dark_shield_roster.ready && game.connected &&
                dark_shield_roster.game_connection_id == game.game_connection_id) {
                const bool roster_changed = dark_shield_roster.roster_revision != sent_auto_dark_shield_roster_revision;
                if (roster_changed) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_dark_shield_roster);
                    if (!chh::wire::make_game_auto_dark_shield_roster(dark_shield_roster.roster, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_dark_shield_roster_revision = dark_shield_roster.roster_revision;
                }
                if (roster_changed || dark_shield_roster.exclusions_revision != sent_auto_dark_shield_exclusions_revision) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_dark_shield_exclusions_set);
                    if (!chh::wire::make_game_auto_dark_shield_exclusions(dark_shield_roster.exclusions, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_dark_shield_exclusions_revision = dark_shield_roster.exclusions_revision;
                }
            }
            const auto heal_roster = online.auto_heal_roster_snapshot(member.binding);
            if (heal_roster.ready && game.connected &&
                heal_roster.game_connection_id == game.game_connection_id) {
                const bool roster_changed = heal_roster.roster_revision != sent_auto_heal_roster_revision;
                if (roster_changed) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_heal_roster);
                    if (!chh::wire::make_game_auto_heal_roster(heal_roster.roster, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_heal_roster_revision = heal_roster.roster_revision;
                }
                if (roster_changed || heal_roster.exclusions_revision != sent_auto_heal_exclusions_revision) {
                    chh::wire::Packet packet(chh::wire::Type::game_auto_heal_exclusions_set);
                    if (!chh::wire::make_game_auto_heal_exclusions(heal_roster.exclusions, packet) ||
                        !send_packet(stream, packet)) break;
                    sent_auto_heal_exclusions_revision = heal_roster.exclusions_revision;
                }
            }
            const auto output = online.game_output_snapshot(member.binding, sent_output_revision);
            if (output.initialized && output.revision != sent_output_revision) {
                if (!send_roster(stream, chh::wire::Type::game_output_begin, chh::wire::Type::game_output_entry,
                    chh::wire::Type::game_output_end, output.players, [](chh::wire::Packet& entry, const GamePlayer& player) {
                        return entry.add_number(player.uid) && entry.add_number(player.team) &&
                            entry.add_string(player.name, chh::wire::profile_name_limit) &&
                            entry.add_number(player.hero_id) && entry.add_number(player.damage_dealt) &&
                            entry.add_number(player.damage_taken);
                    })) break;
                sent_output_revision = output.revision;
            }
            bool relayed = true;
            for (const auto& batch : online.take_game_stats_results(member.binding)) {
                if (!batch.request_id || batch.entries.size() > chh::wire::game_stats_player_limit) continue;
                relayed = send_numbers(stream, chh::wire::Type::game_stats_begin,
                    {batch.request_id, static_cast<uint32_t>(batch.entries.size())});
                for (const auto& entry : batch.entries) {
                    if (!relayed) break;
                    chh::wire::Packet packet(chh::wire::Type::game_stats_entry);
                    relayed = packet.add_number(batch.request_id) && packet.add_number(entry.uid) &&
                        packet.add_number(entry.result) && packet.add_number(entry.level) &&
                        packet.add_number(entry.vip) && packet.add_number(entry.competitive_power);
                    for (const auto& mode : entry.modes) {
                        relayed = relayed && packet.add_number(mode.win_rate_x100) && packet.add_number(mode.match_count);
                        for (const auto& match : mode.recent)
                            relayed = relayed && packet.add_number(match.hero_id) && packet.add_number(match.result);
                    }
                    relayed = relayed && send_packet(stream, packet);
                }
                relayed = relayed && send_numbers(stream, chh::wire::Type::game_stats_end, {batch.request_id});
                if (!relayed) break;
            }
            if (!relayed) break;
            // The game endpoint sends its post-LoadInit SightI independently
            // of the lobby/game-state packet.  Polling the session snapshot
            // keeps TLS writes owned by this worker and avoids cross-thread
            // stream access.
            const auto sight = online.sight_snapshot(member.binding);
            if (sight.initialized && sight.revision != sent_sight_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_sight);
                if (!packet.add_number(sight.value) || !send_packet(stream, packet)) break;
                sent_sight_revision = sight.revision;
            }
        } else {
            // Primary control commands are queued in the pair and delivered to
            // the matching game endpoint on its own TLS worker.
            const auto auto_afk = online.auto_afk_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_afk_set, auto_afk,
                sent_auto_afk_command_revision, {auto_afk.enabled})) break;
            const auto move_distance = online.move_distance_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_move_distance_set, move_distance,
                sent_move_distance_command_revision, {move_distance.distance_bits})) break;
            const auto hero_keys = online.hero_keys_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_hero_keys_set, hero_keys,
                sent_hero_keys_command_revision, {hero_keys.combo_key, hero_keys.toggle_key, hero_keys.lane_key})) break;
            const auto hero_delay = online.hero_delay_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_hero_delay_set, hero_delay,
                sent_hero_delay_command_revision, {hero_delay.delay_ms})) break;
            const auto skin_block = online.skin_block_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_skin_block_set, skin_block,
                sent_skin_block_command_revision, {skin_block.enabled})) break;
            const auto attack_range = online.attack_range_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_attack_range_set, attack_range,
                sent_attack_range_command_revision, {attack_range.enabled})) break;
            bool guisuo_exclusions_sent = true;
            for (uint32_t side = 0; side < chh::wire::game_auto_guisuo_side_count; ++side) {
                const auto target = online.auto_guisuo_roster_snapshot(member.binding, side);
                auto& sent_exclusions = sent_auto_guisuo_exclusions_revisions[side];
                if (!target.ready || target.exclusions_revision == sent_exclusions) continue;
                chh::wire::Packet packet;
                if (!chh::wire::make_game_auto_guisuo_exclusions(side, target.exclusions, packet) ||
                    !send_packet(stream, packet)) { guisuo_exclusions_sent = false; break; }
                sent_exclusions = target.exclusions_revision;
            }
            if (!guisuo_exclusions_sent) break;
            const auto auto_guisuo = online.auto_guisuo_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_guisuo_set, auto_guisuo,
                sent_auto_guisuo_command_revision, {auto_guisuo.enabled, auto_guisuo.min_count})) break;
            const auto dihuo_exclusions = online.auto_dihuo_roster_snapshot(member.binding);
            if (dihuo_exclusions.ready &&
                dihuo_exclusions.exclusions_revision != sent_auto_dihuo_exclusions_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_auto_dihuo_exclusions_set);
                if (!chh::wire::make_game_auto_dihuo_exclusions(dihuo_exclusions.exclusions, packet) ||
                    !send_packet(stream, packet)) break;
                sent_auto_dihuo_exclusions_revision = dihuo_exclusions.exclusions_revision;
            }
            const auto auto_dihuo = online.auto_dihuo_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_dihuo_set, auto_dihuo,
                sent_auto_dihuo_command_revision, {auto_dihuo.enabled, auto_dihuo.range_bits})) break;
            const auto douji_exclusions = online.auto_douji_roster_snapshot(member.binding);
            if (douji_exclusions.ready &&
                douji_exclusions.exclusions_revision != sent_auto_douji_exclusions_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_auto_douji_exclusions_set);
                if (!chh::wire::make_game_auto_douji_exclusions(douji_exclusions.exclusions, packet) ||
                    !send_packet(stream, packet)) break;
                sent_auto_douji_exclusions_revision = douji_exclusions.exclusions_revision;
            }
            const auto auto_douji = online.auto_douji_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_douji_set, auto_douji,
                sent_auto_douji_command_revision, {auto_douji.flags, auto_douji.range_bits})) break;
            const auto dark_shield_exclusions = online.auto_dark_shield_roster_snapshot(member.binding);
            if (dark_shield_exclusions.ready &&
                dark_shield_exclusions.exclusions_revision != sent_auto_dark_shield_exclusions_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_auto_dark_shield_exclusions_set);
                if (!chh::wire::make_game_auto_dark_shield_exclusions(dark_shield_exclusions.exclusions, packet) ||
                    !send_packet(stream, packet)) break;
                sent_auto_dark_shield_exclusions_revision = dark_shield_exclusions.exclusions_revision;
            }
            const auto auto_dark_shield = online.auto_dark_shield_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_dark_shield_set, auto_dark_shield,
                sent_auto_dark_shield_command_revision, {auto_dark_shield.enabled, auto_dark_shield.min_count,
                    auto_dark_shield.use_dark_shield, auto_dark_shield.use_dining_table})) break;
            const auto heal_exclusions = online.auto_heal_roster_snapshot(member.binding);
            if (heal_exclusions.ready &&
                heal_exclusions.exclusions_revision != sent_auto_heal_exclusions_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_auto_heal_exclusions_set);
                if (!chh::wire::make_game_auto_heal_exclusions(heal_exclusions.exclusions, packet) ||
                    !send_packet(stream, packet)) break;
                sent_auto_heal_exclusions_revision = heal_exclusions.exclusions_revision;
            }
            const auto auto_heal = online.auto_heal_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_heal_set, auto_heal,
                sent_auto_heal_command_revision, {auto_heal.enabled, auto_heal.health_percent})) break;
            // Near-item controls and exclusions are independent for each slot.
            bool near_items_sent = true;
            for (uint32_t item_slot = 1; item_slot <= chh::wire::game_near_item_count; ++item_slot) {
                const auto roster = online.near_item_roster_snapshot(member.binding, item_slot);
                auto& sent_exclusions = sent_near_item_exclusions_revisions[item_slot - 1];
                if (roster.ready && roster.exclusions_revision != sent_exclusions) {
                    chh::wire::Packet packet;
                    const bool encoded = item_slot == 1 ? chh::wire::make_game_near_item1_exclusions(roster.exclusions, packet) :
                        chh::wire::make_game_near_item_exclusions(item_slot, roster.exclusions, packet);
                    if (!encoded || !send_packet(stream, packet)) { near_items_sent = false; break; }
                    sent_exclusions = roster.exclusions_revision;
                }
                const auto item = online.near_item_command_snapshot(member.binding, item_slot);
                auto& sent_command = sent_near_item_command_revisions[item_slot - 1];
                if (item.initialized && item.revision != sent_command) {
                    const bool sent = item_slot == 1 ? send_numbers(stream, chh::wire::Type::game_near_item1_set,
                        {item.enabled, item.direct, item.on_attack, item.on_attacked, item.range_bits, item.health_percent}) :
                        send_numbers(stream, chh::wire::Type::game_near_item_set,
                            {item_slot, item.enabled, item.direct, item.on_attack, item.on_attacked, item.range_bits, item.health_percent});
                    if (!sent) { near_items_sent = false; break; }
                    sent_command = item.revision;
                }
            }
            if (!near_items_sent) break;
            const auto auto_glove = online.auto_glove_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_glove_set, auto_glove,
                sent_auto_glove_command_revision, {auto_glove.enabled})) break;
            const auto auto_lingzhou = online.auto_lingzhou_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_lingzhou_set, auto_lingzhou,
                sent_auto_lingzhou_command_revision, {auto_lingzhou.enabled})) break;
            const auto auto_fairy = online.auto_fairy_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_fairy_set, auto_fairy,
                sent_auto_fairy_command_revision, {auto_fairy.enabled})) break;
            const auto auto_devour = online.auto_devour_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_devour_set, auto_devour,
                sent_auto_devour_command_revision, {auto_devour.enabled, auto_devour.minions})) break;
            const auto auto_ignite = online.auto_ignite_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_ignite_set, auto_ignite,
                sent_auto_ignite_command_revision, {auto_ignite.enabled})) break;
            const auto command = online.sight_command_snapshot(member.binding);
            if (command.initialized && command.revision != sent_sight_command_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_sight_set);
                if (!packet.add_number(command.value) || !send_packet(stream, packet)) break;
                sent_sight_command_revision = command.revision;
            }
            const auto disarm_exclusions = online.auto_disarm_roster_snapshot(member.binding);
            if (disarm_exclusions.ready &&
                disarm_exclusions.exclusions_revision != sent_auto_disarm_exclusions_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_auto_disarm_exclusions_set);
                if (!chh::wire::make_game_auto_disarm_exclusions(disarm_exclusions.exclusions, packet) ||
                    !send_packet(stream, packet)) break;
                sent_auto_disarm_exclusions_revision = disarm_exclusions.exclusions_revision;
            }
            const auto auto_disarm = online.auto_disarm_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_disarm_set, auto_disarm,
                sent_auto_disarm_command_revision,
                {auto_disarm.enabled, auto_disarm.on_skill, auto_disarm.direct, auto_disarm.range_bits})) break;
            const auto auto_cleanse = online.auto_cleanse_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_cleanse_set, auto_cleanse,
                sent_auto_cleanse_command_revision,
                {auto_cleanse.enabled, auto_cleanse.ignite, auto_cleanse.disarm, auto_cleanse.root,
                    auto_cleanse.control, auto_cleanse.silence, auto_cleanse.stun, auto_cleanse.taunt,
                    auto_cleanse.delay_ms, auto_cleanse.min_count, auto_cleanse.control_time_ms})) break;
            const auto skill_hint = online.skill_hint_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_skill_hint_set, skill_hint,
                sent_skill_hint_command_revision, skill_hint.hero_skills ?
                std::initializer_list<uint32_t>{skill_hint.enabled, skill_hint.ally, skill_hint.enemy, skill_hint.hero_skills} :
                std::initializer_list<uint32_t>{skill_hint.enabled, skill_hint.ally, skill_hint.enemy})) break;
            const auto auto_accept = online.auto_accept_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_accept_set, auto_accept,
                sent_auto_accept_command_revision, {auto_accept.enabled})) break;
            const auto selection_shout = online.selection_shout_command_snapshot(member.binding);
            if (selection_shout.initialized && selection_shout.revision != sent_selection_shout_command_revision) {
                chh::wire::Packet packet;
                if (!chh::wire::make_game_selection_shout_config(selection_shout.enabled, selection_shout.text, packet) ||
                    !send_packet(stream, packet)) break;
                sent_selection_shout_command_revision = selection_shout.revision;
            }
            const auto instant_pick = online.instant_pick_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_instant_pick_set, instant_pick,
                sent_instant_pick_command_revision, {instant_pick.enabled})) break;
            const auto status_display = online.status_display_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_status_display_set, status_display,
                sent_status_display_command_revision, {status_display.enabled, status_display.ally, status_display.enemy})) break;
            const auto training_mode = online.training_mode_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_training_mode_set, training_mode,
                sent_training_mode_command_revision,
                {training_mode.enabled, training_mode.unlimited_cooldown, training_mode.attack_dummy})) break;
            const auto auto_smite = online.auto_smite_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_smite_set, auto_smite,
                sent_auto_smite_command_revision,
                {auto_smite.enabled, auto_smite.dragon, auto_smite.buffs, auto_smite.monster_catcher})) break;
            const auto jungle_timer = online.jungle_timer_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_jungle_timer_set, jungle_timer,
                sent_jungle_timer_command_revision, {jungle_timer.enabled})) break;
            const auto eye_timer = online.eye_timer_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_eye_timer_set, eye_timer,
                sent_eye_timer_command_revision, {eye_timer.enabled})) break;
            const auto trap_timer = online.trap_timer_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_trap_timer_set, trap_timer,
                sent_trap_timer_command_revision, {trap_timer.enabled})) break;
            const auto disappearance_hint = online.disappearance_hint_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_disappearance_hint_set, disappearance_hint,
                sent_disappearance_hint_command_revision,
                {disappearance_hint.enabled, disappearance_hint.duration_seconds, disappearance_hint.show_heroes})) break;
            const auto movement_trace = online.movement_trace_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_movement_trace_set, movement_trace,
                sent_movement_trace_command_revision, {movement_trace.enabled})) break;
            const auto tower_range = online.tower_range_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_tower_range_set, tower_range,
                sent_tower_range_command_revision, {tower_range.enabled})) break;
            const auto projectile_display = online.projectile_display_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_projectile_display_set, projectile_display,
                sent_projectile_display_command_revision,
                {projectile_display.enabled, projectile_display.enemy, projectile_display.ally})) break;
            const auto auto_dodge = online.auto_dodge_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_auto_dodge_set, auto_dodge,
                sent_auto_dodge_command_revision, {auto_dodge.enabled, auto_dodge.rectangles, auto_dodge.circles})) break;
            const auto low_health_hint = online.low_health_hint_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_low_health_hint_set, low_health_hint,
                sent_low_health_hint_command_revision, {low_health_hint.enabled, low_health_hint.threshold_percent})) break;
            const auto hero_select = online.hero_select_command_snapshot(member.binding);
            if (!send_command(stream, chh::wire::Type::game_hero_select_set, hero_select,
                sent_hero_select_command_revision, {hero_select.hero_id})) break;
            // A close request is a one-shot command queued by the matching
            // primary.  It carries no payload and is sent only to the game
            // endpoint for this account/IP pair.
            const auto close = online.game_close_snapshot(member.binding);
            if (close.initialized && close.revision != sent_game_close_revision) {
                if (!send_packet(stream, chh::wire::Packet(chh::wire::Type::game_close))) break;
                sent_game_close_revision = close.revision;
            }
            bool extra_actions_sent = true;
            for (const auto action : online.take_game_extra_actions(member.binding)) {
                if (!send_numbers(stream, chh::wire::Type::game_extra_action, {action})) {
                    extra_actions_sent = false; break;
                }
            }
            if (!extra_actions_sent) break;
            bool skin_actions_sent = true;
            for (const auto& action : online.take_game_skin_actions(member.binding)) {
                if (!send_numbers(stream, chh::wire::Type::game_skin_action, {action.action, action.skin_id})) {
                    skin_actions_sent = false; break;
                }
            }
            if (!skin_actions_sent) break;
            const auto team = online.game_team_snapshot(member.binding);
            if (team.initialized && team.revision != sent_team_command_revision) {
                chh::wire::Packet packet(chh::wire::Type::game_join_team);
                if (!packet.add_string(team.player_name, chh::wire::game_team_name_limit) || !send_packet(stream, packet)) break;
                sent_team_command_revision = team.revision;
            }
            const auto players_request = online.game_players_request_snapshot(member.binding);
            if (players_request.initialized && players_request.revision != sent_players_request_revision) {
                if (!send_packet(stream, chh::wire::Packet(chh::wire::Type::game_players_request))) break;
                sent_players_request_revision = players_request.revision;
            }
            const auto output_request = online.game_output_request_snapshot(member.binding);
            if (output_request.initialized && output_request.revision != sent_output_request_revision) {
                if (!send_packet(stream, chh::wire::Packet(chh::wire::Type::game_output_request))) break;
                sent_output_request_revision = output_request.revision;
            }
            bool relayed = true;
            for (const auto& request : online.take_game_stats_requests(member.binding)) {
                if (!request.request_id || request.targets.empty() ||
                    request.targets.size() > chh::wire::game_stats_player_limit) continue;
                chh::wire::Packet packet(chh::wire::Type::game_stats_request);
                bool built = packet.add_number(request.request_id) &&
                    packet.add_number(static_cast<uint32_t>(request.targets.size()));
                for (const auto& target : request.targets)
                    built = built && target.uid && target.identification_code &&
                        packet.add_number(target.uid) && packet.add_number(target.identification_code);
                // Sessions never queues zero targets, so a malformed request is skipped, not fatal.
                if (!built) continue;
                if (!send_packet(stream, packet)) { relayed = false; break; }
            }
            if (!relayed) break;
        }
        int wait_ms = 200;
        if (chat_joined && social.enhanced) {
            const auto visibility = lobby.visibility_since(sent_visibility_revision);
            if (visibility.revision != sent_visibility_revision) {
                if (visibility.reset) chat_reset_requested = true;
                if (social.preferences) {
                    bool delivered = true;
                    for (const auto& [author, hidden] : visibility.authors) {
                        chh::wire::Packet update(chh::wire::Type::chat_identity_visibility);
                        if (!update.add_string(author, 64) || !update.add_number(hidden ? 1u : 0u) || !send_packet(stream, update)) {
                            delivered = false; break;
                        }
                    }
                    if (!delivered) break;
                } else chat_reset_requested = true;
                sent_visibility_revision = visibility.revision;
            }
        }
        if (chat_joined) {
            // No socket references escape this worker and no history lock is
            // held during a TLS write. A reset is a stable, bounded snapshot.
            bool delivered = true;
            if (!chat_reset_active) {
                const auto recalls = lobby.recalls_since(chat_recall_revision);
                // Missed events are covered by a history reset, which carries recalled flags.
                if (recalls.reset) chat_reset_requested = true;
                for (const auto& event : recalls.changes) {
                    if (!send_chat_recalled(stream, event)) { delivered = false; break; }
                    chat_recall_revision = event.revision;
                }
            }
            if (delivered && !chat_reset_active) {
                auto batch = lobby.after(chat_cursor, chat_reset_requested);
                chat_reset_requested = false;
                if (batch.reset) {
                    chh::wire::Packet begin(chh::wire::Type::chat_history_begin);
                    begin.set_number(static_cast<uint32_t>(batch.messages.size()));
                    if (!send_packet(stream, begin)) break;
                    avatars.reset_chat();
                    media.reset_chat();
                    chat_history = std::move(batch);
                    chat_recall_revision = chat_history.recall_revision;
                    chat_history_offset = 0;
                    chat_reset_active = true;
                } else {
                    for (const auto& message : batch.messages) {
                        if (!send_chat(message)) { delivered = false; break; }
                        chat_cursor = message.id;
                    }
                    if (batch.messages.size() == chh::server::ChatRoom::batch_limit) wait_ms = 1;
                }
            }
            if (delivered && chat_reset_active) {
                size_t sent = 0;
                while (chat_history_offset < chat_history.messages.size() && sent < chh::server::ChatRoom::batch_limit) {
                    if (!send_chat(chat_history.messages[chat_history_offset])) { delivered = false; break; }
                    ++chat_history_offset; ++sent;
                }
                if (delivered && chat_history_offset == chat_history.messages.size()) {
                    if (!send_packet(stream, chh::wire::Packet(chh::wire::Type::chat_history_end))) break;
                    chat_cursor = chat_history.cursor;
                    chat_history = {};
                    chat_reset_active = false;
                } else wait_ms = 1;
            }
            if (!delivered) break;
        }
        if (social.clear_enabled && !social.clears_loaded) { wait_ms = 1; }
        else if (!refresh_social(stream, social, profile.account_id, online.conversation_revision(member.binding),
            online.notes_revision(member.binding))) break;
        if (!refresh_private_notices(social, profile.account_id, wait_ms, [&](uint64_t id, const std::string& peer) {
            chh::wire::Packet notice(chh::wire::Type::private_notice);
            return notice.add_u64(id) && notice.add_string(peer, 64) && send_packet(stream, notice);
        })) break;
        if (social.joined && social.local_history && online.current_primary(member.binding)) {
            if (!deliver_private(stream, social, profile.account_id, online.conversation_revision(member.binding), wait_ms) ||
                !export_private(stream, social, profile.account_id)) break;
            purge_private(social, profile.account_id);
        }
        if (social.joined && !social.peer.empty() && Clock::now() >= social.next_private) {
            social.next_private = Clock::now() + std::chrono::seconds(1);
            const auto peer = social.peer;
            std::vector<chh::server::PrivateMessage> messages;
            std::string diagnostic;
            const auto result = chh::server::private_since(profile.account_id, peer, social.private_cursor, messages, &diagnostic);
            report_social_database_result(social, "读取私聊新消息", result, diagnostic);
            if (!sync_private_clears()) break;
            if (result == chh::wire::SocialResult::ok) {
                bool delivered = true;
                for (const auto& message : messages) {
                    if (social.peer != peer) break;
                    if (!send_private_message(stream, message, profile.account_id, peer, &media)) { delivered = false; break; }
                    social.private_cursor = message.id;
                }
                if (!delivered) break;
                if (messages.size() == 16) { social.next_private = Clock::now(); wait_ms = 1; }
            } else {
                social.next_private = Clock::now() + std::chrono::seconds(5);
            }
        }
        if (!avatars.tick(social.friends, send_avatar)) break;
        if (avatars.pending()) wait_ms = 1;
        if (!sync_private_clears()) break;
        if (!media.tick(social.peer, media_revision(), lobby_image_visible, send_avatar)) break;
        if (media.pending()) wait_ms = 1;
        if (Clock::now() - last_received > std::chrono::seconds(45)) break;
        if (!stream.wait_readable(wait_ms)) { if (!stream.valid()) break; continue; }
        chh::wire::Packet packet;
        ClearPacket clear_packet{packet};
        if (!stream.read_exact(packet.bytes.data(), packet.bytes.size(), 10000)) break;
        if (!online.current(member.binding)) { send_error(stream, chh::wire::Error::credentials_changed); break; }
        if (!packet.valid()) {
            send_error(stream, chh::wire::Error::protocol); break;
        }
        last_received = Clock::now();
        if (packet.type() == chh::wire::Type::game_notes_request && online.current_primary(member.binding)) {
            chh::wire::Reader body{packet}; uint32_t request = 0, page = 0; std::string query, uid, account;
            // The marker-account filter is an optional trailing string; when present it is non-empty.
            if (!body.number(request) || !request || !body.number(page) ||
                !body.string(query, chh::wire::game_note_query_limit, true) ||
                !body.string(uid, chh::wire::game_note_uid_limit, true) ||
                (!body.done() && !body.string(account, chh::wire::game_note_account_id_limit)) || !body.done() ||
                page > (chh::wire::game_note_limit - 1) / chh::wire::game_note_page_size ||
                !chh::wire::valid_game_note_query(query) || !chh::wire::valid_game_note_uid_filter(uid) ||
                !chh::wire::valid_game_note_account_filter(account)) { send_error(stream, chh::wire::Error::protocol); break; }
            chh::server::NoticePage notes; std::string diagnostic;
            const auto result = chh::server::list_notices(page, std::move(query), std::move(uid), std::move(account),
                notes, &diagnostic);
            if (result != chh::wire::GameNoteResult::ok) {
                if (!send_game_note_result(stream, request, chh::wire::GameNoteAction::list, result)) break;
            } else if (!send_game_notes(stream, request, notes)) break;
        } else if (packet.type() == chh::wire::Type::game_note_action && online.current_primary(member.binding)) {
            chh::wire::Reader body{packet}; uint32_t request = 0, action = 0;
            chh::server::NoticeRecord original; std::string replacement;
            const bool shape = body.number(request) && request && body.number(action) &&
                (action == uint32_t(chh::wire::GameNoteAction::create) ||
                    action == uint32_t(chh::wire::GameNoteAction::update) ||
                    action == uint32_t(chh::wire::GameNoteAction::remove)) &&
                body.string(original.account_id, 64) && body.string(original.uid, chh::wire::game_note_uid_limit) &&
                body.string(original.game_name, chh::wire::game_note_name_limit) &&
                body.string(original.notes_time, chh::wire::game_note_time_limit,
                    action == uint32_t(chh::wire::GameNoteAction::create)) &&
                (action == uint32_t(chh::wire::GameNoteAction::create) ||
                    action == uint32_t(chh::wire::GameNoteAction::update)
                    ? body.string(replacement, chh::wire::game_note_text_limit, true) : true) && body.done();
            if (!shape) { send_error(stream, chh::wire::Error::protocol); break; }
            if (action == uint32_t(chh::wire::GameNoteAction::create)) {
                // The wire has no rate-limited code for notes; a refused create
                // is reported as unavailable, like any other failed attempt.
                const auto now = Clock::now();
                if (now < next_note_create) {
                    if (!send_game_note_result(stream, request, chh::wire::GameNoteAction::create,
                        chh::wire::GameNoteResult::unavailable)) break;
                    continue;
                }
                next_note_create = now + std::chrono::seconds(1);
            }
            std::string diagnostic;
            const auto result = action == uint32_t(chh::wire::GameNoteAction::create)
                ? chh::server::create_notice(profile.account_id, original.uid, original.game_name, replacement, &diagnostic)
                : action == uint32_t(chh::wire::GameNoteAction::update)
                ? chh::server::update_notice(profile.account_id, original, replacement, &diagnostic)
                : chh::server::delete_notice(profile.account_id, original, &diagnostic);
            if (!send_game_note_result(stream, request, static_cast<chh::wire::GameNoteAction>(action), result)) break;
        } else if (packet.type() == chh::wire::Type::hero_permissions_request) {
            // Both authenticated roles query only the account bound at login.
            // Check again after database I/O so revocation cannot publish a snapshot.
            if (!online.current(member.binding)) { send_error(stream, chh::wire::Error::credentials_changed); break; }
            chh::wire::Reader body{packet}; uint32_t request = 0, scope = 0;
            if (!body.number(request) || !request) { send_error(stream, chh::wire::Error::protocol); break; }
            const bool heroes = !body.done();
            if (heroes && (!body.number(scope) ||
                    (scope != chh::wire::hero_permissions_scope_heroes && scope != chh::wire::hero_permissions_scope_catalog) ||
                    !body.done() || (scope == chh::wire::hero_permissions_scope_heroes && !online.current_primary(member.binding)))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const bool primary_heroes = heroes && online.current_primary(member.binding);
            const auto now = Clock::now();
            if (now < next_hero_permissions_request) {
                if (!send_hero_permissions_result(stream, request, chh::wire::HeroPermissionsResult::rate_limited)) break;
                continue;
            }
            next_hero_permissions_request = now + std::chrono::milliseconds(350);
            std::vector<uint32_t> ids; std::string diagnostic;
            std::vector<chh::server::HeroCatalogEntry> catalog;
            if (primary_heroes) { available_hero_ids.clear(); available_heroes_expires = now + std::chrono::seconds(30); }
            const bool names = scope == chh::wire::hero_permissions_scope_catalog;
            const bool loaded = names ? chh::server::load_hero_catalog(profile.account_id, catalog, &diagnostic) :
                heroes ? chh::server::load_available_heroes(profile.account_id, ids, &diagnostic) :
                    chh::server::load_hero_permissions(profile.account_id, ids, &diagnostic);
            if (!online.current(member.binding)) { send_error(stream, chh::wire::Error::credentials_changed); break; }
            if (names && primary_heroes && loaded) for (const auto& row : catalog) if (row.available) ids.push_back(row.id);
            if (primary_heroes && !online.clear_unavailable_hero_select(member.binding, loaded ? ids : available_hero_ids)) {
                send_error(stream, chh::wire::Error::credentials_changed); break;
            }
            if (!loaded) {
                if (!send_hero_permissions_result(stream, request, chh::wire::HeroPermissionsResult::unavailable)) break;
            } else {
                if (!(names ? send_hero_catalog(stream, request, catalog) : send_hero_permissions(stream, request, ids, heroes))) break;
                if (primary_heroes) available_hero_ids = std::move(ids);
            }
        } else if (packet.type() == chh::wire::Type::hero_rank_request && online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0, mode = 0, sort = 0, page = 0; std::string query;
            if (!body.number(request_id) || !request_id || !body.number(mode) || !body.number(sort) ||
                !body.number(page) || !private_request_text(packet, body.offset, query)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto now = Clock::now();
            if (now < next_hero_request) {
                if (!send_hero_rank_result(stream, request_id, chh::wire::HeroRankResult::rate_limited)) break;
                continue;
            }
            next_hero_request = now + std::chrono::milliseconds(350);
            if (!chh::wire::valid_hero_request(mode, sort, page, query)) {
                if (!send_hero_rank_result(stream, request_id, chh::wire::HeroRankResult::invalid)) break;
                continue;
            }
            const auto seconds = unix_seconds();
            std::string diagnostic;
            if (!still_primary()) break;
            if (mode == uint32_t(chh::wire::HeroRankMode::online_time)) {
                const auto snapshot = seconds > 0 ? online_time_ranking.get(now, static_cast<uint64_t>(seconds),
                    chh::server::load_online_time_records, &diagnostic) : nullptr;
                if (!snapshot) {
                    if (!diagnostic.empty()) report_hero_rank_error(diagnostic);
                    if (!send_hero_rank_result(stream, request_id, chh::wire::HeroRankResult::unavailable)) break;
                    continue;
                }
                const auto result = chh::server::online_time_rank_page(*snapshot, page, std::move(query));
                if (!send_hero_rank_page(stream, request_id, result)) break;
            } else if (mode == uint32_t(chh::wire::HeroRankMode::permissions)) {
                const auto snapshot = seconds > 0 ? permission_ranking.get(now, static_cast<uint64_t>(seconds),
                    chh::server::load_permission_records, &diagnostic) : nullptr;
                if (!snapshot) {
                    if (!diagnostic.empty()) report_hero_rank_error(diagnostic);
                    if (!send_hero_rank_result(stream, request_id, chh::wire::HeroRankResult::unavailable)) break;
                    continue;
                }
                const auto result = chh::server::permission_rank_page(*snapshot, sort, page, std::move(query));
                if (!send_hero_rank_page(stream, request_id, result)) break;
            } else {
                const auto snapshot = seconds > 0 ? hero_ranking.get(now, static_cast<uint64_t>(seconds),
                    chh::server::load_hero_usage, &diagnostic) : nullptr;
                if (!snapshot) {
                    if (!diagnostic.empty()) report_hero_rank_error(diagnostic);
                    if (!send_hero_rank_result(stream, request_id, chh::wire::HeroRankResult::unavailable)) break;
                    continue;
                }
                const auto result = chh::server::hero_rank_page(*snapshot, mode, sort, page, std::move(query));
                if (!send_hero_rank_page(stream, request_id, result)) break;
            }
        } else if (packet.type() == chh::wire::Type::account_join &&
            (packet.length() == 0 || (packet.length() == 4 &&
                (packet.number() == chh::wire::account_feature_preferences ||
                 packet.number() == chh::wire::account_features))) &&
            online.current_primary(member.binding)) {
            account_preferences = account_preferences || packet.length() == 4;
            account_online_time = account_online_time ||
                (packet.length() == 4 && (packet.number() & chh::wire::account_feature_online_time) != 0);
            account_joined = true; sent_account_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::account_update && account_joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0, action = 0, hidden = 0;
            std::string name, current_password, next_password, confirmation;
            ClearPassword clear_current{current_password}, clear_next{next_password}, clear_confirmation{confirmation};
            if (!body.number(request_id) || !request_id || !body.number(action) ||
                (action != uint32_t(chh::wire::AccountAction::rename) && action != uint32_t(chh::wire::AccountAction::password) &&
                    (action != uint32_t(chh::wire::AccountAction::hide_account_id) || !account_preferences)) ||
                (action == uint32_t(chh::wire::AccountAction::rename)
                    ? !body.string(name, chh::wire::profile_name_limit, true)
                    : action == uint32_t(chh::wire::AccountAction::password)
                        ? (!body.string(current_password, 256, true) || !body.string(next_password, 256, true) ||
                            !body.string(confirmation, 256, true)) : !body.number(hidden)) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            SecureZeroMemory(packet.bytes.data(), packet.bytes.size());
            std::unique_lock gate(account_gate(profile.account_id));
            if (!still_primary()) break;
            chh::wire::AccountResult result = chh::wire::AccountResult::invalid;
            std::string diagnostic;
            if (!online.begin_account_update(member.binding, Clock::now())) result = chh::wire::AccountResult::rate_limited;
            else if (action == uint32_t(chh::wire::AccountAction::rename)) {
                if (chh::wire::valid_profile_name(name)) {
                    result = chh::server::rename_account(profile.account_id, name, &diagnostic);
                    if (result == chh::wire::AccountResult::ok) {
                        online.rename_account(profile.account_id, name); profile.name = name;
                        ++social_revision; social.next_refresh = {};
                    }
                }
            } else if (action == uint32_t(chh::wire::AccountAction::hide_account_id)) {
                if (hidden <= 1) {
                    result = chh::server::set_hide_account_id(profile.account_id, hidden != 0, &diagnostic);
                    if (result == chh::wire::AccountResult::ok) {
                        apply_account_preferences(profile.account_id, true, hidden != 0);
                        // Friends display the peer's privacy flag in their
                        // list; force every social worker to refresh it.
                        ++social_revision;
                    }
                }
            } else if (!current_password.empty() && current_password.size() <= 256 &&
                chh::wire::valid_text(current_password) && chh::wire::valid_profile_password(next_password)) {
                result = next_password != confirmation ? chh::wire::AccountResult::confirmation_mismatch :
                    chh::server::change_account_password(profile.account_id, current_password, next_password, &diagnostic);
            }
            if (result == chh::wire::AccountResult::unavailable)
                std::cerr << ("账号资料更新失败：" + (diagnostic.empty() ? std::string("数据库操作未完成") : diagnostic) + "\n");
            const bool password_changed = action == uint32_t(chh::wire::AccountAction::password) &&
                result == chh::wire::AccountResult::ok;
            if (!password_changed && result == chh::wire::AccountResult::ok) {
                const auto current = online.profile_snapshot(member.binding);
                if (!send_account_state(stream, profile, current, account_preferences,
                    account_online_time ? &account_online_state : nullptr)) break;
                sent_account_revision = current.revision;
            }
            if (password_changed && online.invalidate_account(profile.account_id)) {
                finish_online_time(member.binding);
                record_last_primary_offline(profile.account_id);
            }
            if (!send_account_result(stream, request_id, action, result)) break;
            if (password_changed) break;
        } else if (packet.type() == chh::wire::Type::ping && packet.length() == 0) {
            if (!send_packet(stream, chh::wire::Packet(chh::wire::Type::pong))) break;
        } else if (packet.type() == chh::wire::Type::ping && packet.length() == 8) {
            chh::wire::Reader body{packet};
            uint64_t available_virtual_memory = 0;
            // A Windows user-mode address space is far below this bound; the
            // limit rejects malformed values without assuming a 32-bit client.
            if (!body.u64(available_virtual_memory) || available_virtual_memory > (uint64_t(1) << 60) ||
                !body.done() ||
                !online.update_game_memory(member.binding, available_virtual_memory) ||
                !send_packet(stream, chh::wire::Packet(chh::wire::Type::pong))) break;
        } else if (packet.type() == chh::wire::Type::game_auto_afk_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_auto_afk(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_move_distance_set) {
            chh::wire::Reader body{packet};
            uint32_t distance_bits = 0;
            if (!body.number(distance_bits) || !body.done() ||
                !online.set_game_move_distance(member.binding, distance_bits)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_hero_keys_set) {
            chh::wire::Reader body{packet};
            uint32_t combo_key = 0, toggle_key = 0, lane_key = 0;
            if (!body.number(combo_key) || !body.number(toggle_key) || !body.number(lane_key) || !body.done() ||
                !online.set_game_hero_keys(member.binding, combo_key, toggle_key, lane_key)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_hero_delay_set) {
            chh::wire::Reader body{packet};
            uint32_t delay_ms = 0;
            if (!body.number(delay_ms) || !body.done() ||
                !online.set_game_hero_delay(member.binding, delay_ms)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_skin_block_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_skin_block(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_attack_range_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_attack_range(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_glove_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() || !online.set_game_auto_glove(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_lingzhou_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() || !online.set_game_auto_lingzhou(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_fairy_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() || !online.set_game_auto_fairy(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_devour_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, minions = 0;
            if (!body.number(enabled) || !body.number(minions) || !body.done() ||
                !online.set_game_auto_devour(member.binding, enabled, minions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_ignite_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_auto_ignite(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_sight) {
            // Only the game endpoint may establish/update the initial sight
            // value; the primary can only use game_sight_set below.
            if (packet.length() != 4 || !chh::wire::valid_game_sight(packet.number()) ||
                !online.update_game_sight(member.binding, packet.number())) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_sight_set) {
            // Only the authenticated primary may control the paired game
            // endpoint.  The command is retained until that endpoint polls it.
            if (packet.length() != 4 || !chh::wire::valid_game_sight(packet.number()) ||
                !online.set_game_sight(member.binding, packet.number())) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_guisuo_roster) {
            uint32_t side = 0;
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_guisuo_roster(packet, side, roster) ||
                !online.update_game_auto_guisuo_roster(member.binding, side, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_guisuo_exclusions_set) {
            uint32_t side = 0;
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_guisuo_exclusions(packet, side, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_guisuo_exclusions(member.binding, side, std::move(exclusions));
            if (result == chh::server::Sessions::AutoGuisuoExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_guisuo_exclusions_revisions[side] = UINT64_MAX;
            if (result == chh::server::Sessions::AutoGuisuoExclusionsResult::ignored)
                sent_auto_guisuo_roster_revisions[side] = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_guisuo_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, min_count = 0;
            if (!body.number(enabled) || !body.number(min_count) || !body.done() ||
                !online.set_game_auto_guisuo(member.binding, enabled, min_count)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_dihuo_roster) {
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_dihuo_roster(packet, roster) ||
                !online.update_game_auto_dihuo_roster(member.binding, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_dihuo_exclusions_set) {
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_dihuo_exclusions(packet, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_dihuo_exclusions(member.binding, std::move(exclusions));
            if (result == chh::server::Sessions::AutoDihuoExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_dihuo_exclusions_revision = UINT64_MAX;
            if (result == chh::server::Sessions::AutoDihuoExclusionsResult::ignored)
                sent_auto_dihuo_roster_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_dihuo_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, range_bits = 0;
            if (!body.number(enabled) || !body.number(range_bits) || !body.done() ||
                !online.set_game_auto_dihuo(member.binding, enabled, range_bits)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_douji_roster) {
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_douji_roster(packet, roster) ||
                !online.update_game_auto_douji_roster(member.binding, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_douji_exclusions_set) {
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_douji_exclusions(packet, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_douji_exclusions(member.binding, std::move(exclusions));
            if (result == chh::server::Sessions::AutoDoujiExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_douji_exclusions_revision = UINT64_MAX;
            if (result == chh::server::Sessions::AutoDoujiExclusionsResult::ignored)
                sent_auto_douji_roster_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_douji_set) {
            chh::wire::Reader body{packet};
            uint32_t flags = 0, range_bits = 0;
            if (!body.number(flags) || !body.number(range_bits) || !body.done() ||
                !online.set_game_auto_douji(member.binding, flags, range_bits)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_dark_shield_roster) {
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_dark_shield_roster(packet, roster) ||
                !online.update_game_auto_dark_shield_roster(member.binding, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_dark_shield_exclusions_set) {
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_dark_shield_exclusions(packet, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_dark_shield_exclusions(member.binding, std::move(exclusions));
            if (result == chh::server::Sessions::AutoDarkShieldExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_dark_shield_exclusions_revision = UINT64_MAX;
            if (result == chh::server::Sessions::AutoDarkShieldExclusionsResult::ignored)
                sent_auto_dark_shield_roster_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_dark_shield_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, min_count = 0, use_dark_shield = 0, use_dining_table = 0;
            if (!body.number(enabled) || !body.number(min_count) || !body.number(use_dark_shield) ||
                !body.number(use_dining_table) || !body.done() ||
                !online.set_game_auto_dark_shield(member.binding, enabled, min_count, use_dark_shield, use_dining_table)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_heal_roster) {
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_heal_roster(packet, roster) ||
                !online.update_game_auto_heal_roster(member.binding, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_heal_exclusions_set) {
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_heal_exclusions(packet, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_heal_exclusions(member.binding, std::move(exclusions));
            if (result == chh::server::Sessions::AutoHealExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_heal_exclusions_revision = UINT64_MAX;
            if (result == chh::server::Sessions::AutoHealExclusionsResult::ignored)
                sent_auto_heal_roster_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_heal_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, health_percent = 0;
            if (!body.number(enabled) || !body.number(health_percent) || !body.done() ||
                !online.set_game_auto_heal(member.binding, enabled, health_percent)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_near_item1_roster ||
            packet.type() == chh::wire::Type::game_near_item_roster) {
            uint32_t item_slot = 1;
            chh::wire::GameAutoDisarmRoster roster;
            const bool decoded = packet.type() == chh::wire::Type::game_near_item1_roster ?
                chh::wire::read_game_near_item1_roster(packet, roster) : chh::wire::read_game_near_item_roster(packet, item_slot, roster);
            if (!decoded || !online.update_game_near_item_roster(member.binding, item_slot, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_near_item1_exclusions_set ||
            packet.type() == chh::wire::Type::game_near_item_exclusions_set) {
            uint32_t item_slot = 1;
            chh::wire::GameAutoDisarmExclusions exclusions;
            const bool decoded = packet.type() == chh::wire::Type::game_near_item1_exclusions_set ?
                chh::wire::read_game_near_item1_exclusions(packet, exclusions) :
                chh::wire::read_game_near_item_exclusions(packet, item_slot, exclusions);
            if (!decoded) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_near_item_exclusions(member.binding, item_slot, std::move(exclusions));
            if (result == chh::server::Sessions::NearItemExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_near_item_exclusions_revisions[item_slot - 1] = UINT64_MAX;
            if (result == chh::server::Sessions::NearItemExclusionsResult::ignored)
                sent_near_item_roster_revisions[item_slot - 1] = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_near_item1_set ||
            packet.type() == chh::wire::Type::game_near_item_set) {
            chh::wire::Reader body{packet};
            uint32_t item_slot = 1;
            uint32_t enabled = 0, direct = 0, on_attack = 0, on_attacked = 0, range_bits = 0, health_percent = 0;
            if ((packet.type() == chh::wire::Type::game_near_item_set && !body.number(item_slot)) ||
                !body.number(enabled) || !body.number(direct) || !body.number(on_attack) || !body.number(on_attacked) ||
                !body.number(range_bits) || !body.number(health_percent) || !body.done() ||
                !online.set_game_near_item(member.binding, item_slot, enabled, direct, on_attack, on_attacked, range_bits, health_percent)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_disarm_roster) {
            chh::wire::GameAutoDisarmRoster roster;
            if (!chh::wire::read_game_auto_disarm_roster(packet, roster) ||
                !online.update_game_auto_disarm_roster(member.binding, std::move(roster))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_disarm_exclusions_set) {
            chh::wire::GameAutoDisarmExclusions exclusions;
            if (!chh::wire::read_game_auto_disarm_exclusions(packet, exclusions)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto result = online.set_game_auto_disarm_exclusions(member.binding, std::move(exclusions));
            if (result == chh::server::Sessions::AutoDisarmExclusionsResult::invalid) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Confirm retries, and refresh a UI whose match or request became stale.
            sent_auto_disarm_exclusions_revision = UINT64_MAX;
            if (result == chh::server::Sessions::AutoDisarmExclusionsResult::ignored)
                sent_auto_disarm_roster_revision = UINT64_MAX;
        } else if (packet.type() == chh::wire::Type::game_auto_disarm_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, on_skill = 0, direct = 0, range_bits = 0;
            if (!body.number(enabled) || !body.number(on_skill) || !body.number(direct) ||
                !body.number(range_bits) || !body.done() ||
                !online.set_game_auto_disarm(member.binding, enabled, on_skill, direct, range_bits)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_cleanse_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, ignite = 0, disarm = 0, root = 0, control = 0, silence = 0, stun = 0;
            uint32_t taunt = 0, delay_ms = 0, min_count = 0, control_time_ms = 0;
            if (!body.number(enabled) || !body.number(ignite) || !body.number(disarm) || !body.number(root) ||
                !body.number(control) || !body.number(silence) || !body.number(stun) ||
                !body.number(taunt) || !body.number(delay_ms) || !body.number(min_count) ||
                !body.number(control_time_ms) || !body.done() ||
                !online.set_game_auto_cleanse(member.binding, enabled, ignite, disarm, root, control, silence, stun,
                    taunt, delay_ms, min_count, control_time_ms)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_skill_hint_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, ally = 0, enemy = 0, hero_skills = 0;
            if (!body.number(enabled) || !body.number(ally) || !body.number(enemy) ||
                (!body.done() && !body.number(hero_skills)) || !body.done() ||
                !online.set_game_skill_hint(member.binding, enabled, ally, enemy, hero_skills)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_accept_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_auto_accept(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_selection_shout_set) {
            uint32_t enabled = 0;
            std::string text;
            if (!chh::wire::read_game_selection_shout_config(packet, enabled, text) ||
                !online.set_game_selection_shout(member.binding, enabled, std::move(text))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_instant_pick_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_instant_pick(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_status_display_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, ally = 0, enemy = 0;
            if (!body.number(enabled) || !body.number(ally) || !body.number(enemy) || !body.done() ||
                !online.set_game_status_display(member.binding, enabled, ally, enemy)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_training_mode_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, unlimited_cooldown = 0, attack_dummy = 0;
            if (!body.number(enabled) || !body.number(unlimited_cooldown) || !body.number(attack_dummy) || !body.done() ||
                !online.set_game_training_mode(member.binding, enabled, unlimited_cooldown, attack_dummy)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_smite_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, dragon = 0, buffs = 0, monster_catcher = 0;
            if (!body.number(enabled) || !body.number(dragon) || !body.number(buffs) ||
                !body.number(monster_catcher) || !body.done() ||
                !online.set_game_auto_smite(member.binding, enabled, dragon, buffs, monster_catcher)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_jungle_timer_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_jungle_timer(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_eye_timer_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_eye_timer(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_trap_timer_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_trap_timer(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_disappearance_hint_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, duration_seconds = 0, show_heroes = 0;
            if (!body.number(enabled) || !body.number(duration_seconds) || !body.number(show_heroes) || !body.done() ||
                !online.set_game_disappearance_hint(member.binding, enabled, duration_seconds, show_heroes)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_movement_trace_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_movement_trace(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_tower_range_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0;
            if (!body.number(enabled) || !body.done() ||
                !online.set_game_tower_range(member.binding, enabled)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_projectile_display_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, enemy = 1, ally = 0;
            if (!body.number(enabled) || !body.number(enemy) || !body.number(ally) || !body.done() ||
                !online.set_game_projectile_display(member.binding, enabled, enemy, ally)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_auto_dodge_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, rectangles = 0, circles = 0;
            if (!body.number(enabled) || !body.number(rectangles) || !body.number(circles) || !body.done() ||
                !online.set_game_auto_dodge(member.binding, enabled, rectangles, circles)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_low_health_hint_set) {
            chh::wire::Reader body{packet};
            uint32_t enabled = 0, threshold_percent = 0;
            if (!body.number(enabled) || !body.number(threshold_percent) || !body.done() ||
                !online.set_game_low_health_hint(member.binding, enabled, threshold_percent)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_hero_select_set) {
            chh::wire::Reader body{packet};
            uint32_t hero_id = 0;
            if (!body.number(hero_id) || !body.done() || !online.current_primary(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (hero_id != 0) {
                if (hero_id > 65535) continue;
                const auto now = Clock::now();
                if (now >= available_heroes_expires) {
                    available_hero_ids.clear();
                    available_heroes_expires = now + std::chrono::seconds(30);
                    std::vector<uint32_t> ids; std::string diagnostic;
                    const bool loaded = chh::server::load_available_heroes(profile.account_id, ids, &diagnostic);
                    if (!online.current_primary(member.binding)) { send_error(stream, chh::wire::Error::credentials_changed); break; }
                    if (loaded) available_hero_ids = std::move(ids);
                    if (!online.clear_unavailable_hero_select(member.binding, available_hero_ids)) {
                        send_error(stream, chh::wire::Error::credentials_changed); break;
                    }
                }
                if (!std::binary_search(available_hero_ids.begin(), available_hero_ids.end(), hero_id)) continue;
            }
            if (!online.set_game_hero_select(member.binding, hero_id)) {
                send_error(stream, chh::wire::Error::credentials_changed); break;
            }
        } else if (packet.type() == chh::wire::Type::game_close) {
            // The primary may request a close only with an empty payload. A
            // stale request after the game has disconnected is harmless and
            // is simply ignored; this avoids dropping the primary TLS session
            // during a normal game shutdown race.
            if (packet.length() != 0 || !online.current_primary(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            online.request_game_close(member.binding);
        } else if (packet.type() == chh::wire::Type::game_extra_action) {
            chh::wire::Reader body{packet};
            uint32_t action = 0;
            uint64_t target_game_connection_id = 0;
            if (!body.number(action) || !body.u64(target_game_connection_id) || !body.done() ||
                !chh::wire::valid_game_extra_action(action) || !target_game_connection_id ||
                !online.current_primary(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Ignore an unavailable/replaced game or full queue without dropping
            // the primary. A delayed click must never target a new game connection.
            (void)online.request_game_extra_action(member.binding, action, target_game_connection_id);
        } else if (packet.type() == chh::wire::Type::game_skin_action) {
            chh::wire::Reader body{packet};
            uint32_t action = 0, skin_id = 0;
            uint64_t target_game_connection_id = 0;
            if (!body.number(action) || !body.number(skin_id) || !body.u64(target_game_connection_id) || !body.done() ||
                !chh::wire::valid_game_skin_action(action, skin_id) || !target_game_connection_id ||
                !online.current_primary(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            (void)online.request_game_skin_action(member.binding, action, skin_id, target_game_connection_id);
        } else if (packet.type() == chh::wire::Type::game_players_request ||
                   packet.type() == chh::wire::Type::game_output_request) {
            // Like game_close: a refresh racing the game's exit is ignored
            // rather than dropping the primary session.
            if (packet.length() != 0 || !online.current_primary(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (packet.type() == chh::wire::Type::game_players_request) (void)online.request_game_players(member.binding);
            else (void)online.request_game_output(member.binding);
        } else if ((packet.type() == chh::wire::Type::team_join_request ||
                    packet.type() == chh::wire::Type::team_invite_request) &&
                   online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            std::string peer;
            const bool invite = packet.type() == chh::wire::Type::team_invite_request;
            const auto now = Clock::now();
            // Excess clicks are dropped like other stale fire-and-forget requests.
            const bool paced = now >= next_team_request;
            if (paced) next_team_request = now + std::chrono::milliseconds(300);
            const bool valid_request = body.string(peer, 64) && !peer.empty() && body.done() &&
                chh::wire::valid_text(peer) && peer != profile.account_id && paced;
            bool accepted = false;
            if (valid_request) accepted = accepted_friend(peer);
            // Resolve names, validate current sessions and select the endpoint
            // atomically after the friendship lookup, which can block on SQL.
            if (accepted) (void)online.request_game_team(member.binding, peer, invite);
            // These are fire-and-forget UI actions. Invalid or stale requests
            // are ignored; the next presence refresh updates button state.
        } else if (packet.type() == chh::wire::Type::game_players_begin &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 4 &&
                   packet.number() <= 14) {
            incoming_players.begin(packet.number());
        } else if (packet.type() == chh::wire::Type::game_player_entry &&
                   member.binding.role == chh::server::ClientRole::secondary && incoming_players.loading) {
            chh::wire::Reader body{packet};
            GamePlayer player;
            if (!body.number(player.uid) || !body.number(player.team) ||
                !body.string(player.name, chh::wire::profile_name_limit) ||
                !body.number(player.level) || !body.number(player.vip) ||
                !body.number(player.competitive_power) || !body.number(player.identification_code) ||
                (!body.done() && !body.number(player.hero_id)) ||
                (!body.done() && !body.number(player.damage_dealt)) || !body.done() ||
                incoming_players.full() || !player.uid || player.team > 255 ||
                player.identification_code > 0xffffu ||
                !chh::wire::valid_profile_name(player.name)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            incoming_players.players.push_back(std::move(player));
        } else if (packet.type() == chh::wire::Type::game_players_end &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 0 &&
                   incoming_players.complete()) {
            if (!online.update_game_players(member.binding, incoming_players.take())) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_output_begin &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 4 &&
                   packet.number() <= 14) {
            incoming_output.begin(packet.number());
        } else if (packet.type() == chh::wire::Type::game_output_entry &&
                   member.binding.role == chh::server::ClientRole::secondary && incoming_output.loading) {
            chh::wire::Reader body{packet};
            GamePlayer player;
            if (!body.number(player.uid) || !body.number(player.team) ||
                !body.string(player.name, chh::wire::profile_name_limit) ||
                !body.number(player.hero_id) || !body.number(player.damage_dealt) ||
                !body.number(player.damage_taken) || !body.done() ||
                incoming_output.full() || !player.uid || player.team > 255 ||
                !chh::wire::valid_profile_name(player.name)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            incoming_output.players.push_back(std::move(player));
        } else if (packet.type() == chh::wire::Type::game_output_end &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 0 &&
                   incoming_output.complete()) {
            if (!online.update_game_output(member.binding, incoming_output.take())) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::game_stats_begin &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 8) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0, expected = 0;
            if (!body.number(request_id) || !body.number(expected) || !body.done() || !request_id ||
                expected > chh::wire::game_stats_player_limit || incoming_stats_loading) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            incoming_stats_request_id = request_id;
            incoming_stats_expected = expected;
            incoming_stats.clear(); incoming_stats.reserve(expected);
            incoming_stats_loading = true;
        } else if (packet.type() == chh::wire::Type::game_stats_entry &&
                   member.binding.role == chh::server::ClientRole::secondary && incoming_stats_loading) {
            chh::wire::Reader body{packet};
            chh::server::Sessions::Game::StatsEntry entry;
            bool valid = body.number(entry.request_id) && body.number(entry.uid) && body.number(entry.result) &&
                body.number(entry.level) && body.number(entry.vip) && body.number(entry.competitive_power);
            for (auto& mode : entry.modes) {
                valid = valid && body.number(mode.win_rate_x100) && body.number(mode.match_count);
                for (auto& match : mode.recent)
                    valid = valid && body.number(match.hero_id) && body.number(match.result);
            }
            if (!valid || !body.done() || entry.request_id != incoming_stats_request_id || !entry.uid ||
                !chh::wire::valid_game_stats_result(entry.result) || entry.uid == 0 ||
                incoming_stats.size() >= incoming_stats_expected) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            bool values_valid = true;
            for (const auto& mode : entry.modes) {
                if (mode.win_rate_x100 > 10000 || mode.match_count > 100000000) { values_valid = false; break; }
                for (const auto& match : mode.recent)
                    if (match.result > 255) { values_valid = false; break; }
                if (!values_valid) break;
            }
            if (!values_valid) { send_error(stream, chh::wire::Error::protocol); break; }
            incoming_stats.push_back(std::move(entry));
        } else if (packet.type() == chh::wire::Type::game_stats_end &&
                   member.binding.role == chh::server::ClientRole::secondary && packet.length() == 4) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0;
            if (!body.number(request_id) || !body.done() || !incoming_stats_loading ||
                request_id != incoming_stats_request_id || incoming_stats.size() != incoming_stats_expected ||
                !online.complete_game_stats(member.binding, request_id, std::move(incoming_stats))) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            incoming_stats.clear(); incoming_stats_expected = 0; incoming_stats_request_id = 0;
            incoming_stats_loading = false;
        } else if (packet.type() == chh::wire::Type::explorer_ready) {
            if (packet.length() != 0 || !online.enable_explorer(member.binding)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        } else if (packet.type() == chh::wire::Type::explorer_result &&
                   member.binding.role == chh::server::ClientRole::primary) {
            chh::wire::Reader body{packet};
            uint64_t request_id = 0;
            uint32_t status = 0;
            std::string message;
            if (!body.u64(request_id) || !request_id || !body.number(status) ||
                !chh::wire::valid_explorer_status(status) ||
                !body.string(message, chh::wire::explorer_message_limit, true) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (online.explorer_result(member.binding, request_id,
                    status != uint32_t(chh::wire::ExplorerStatus::downloading))) {
                const char* label = "失败";
                switch (chh::wire::ExplorerStatus(status)) {
                case chh::wire::ExplorerStatus::downloading: label = "正在下载"; break;
                case chh::wire::ExplorerStatus::launched: label = "已启动"; break;
                case chh::wire::ExplorerStatus::busy: label = "客户端忙"; break;
                case chh::wire::ExplorerStatus::cancelled: label = "已取消"; break;
                default: break;
                }
                std::ostringstream output;
                output << "explorer.exe 请求 " << request_id << "，sock：" << socket << "，" << label;
                if (!message.empty()) output << "；" << message;
                output << '\n';
                std::cout << output.str() << std::flush;
            }
        } else if (packet.type() == chh::wire::Type::game_login_event &&
                   member.binding.role == chh::server::ClientRole::secondary) {
            chh::wire::Reader body{packet};
            uint32_t uid = 0;
            std::string player_name;
            if (!body.number(uid) || !uid ||
                !body.string(player_name, chh::wire::profile_name_limit) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (!online.current(member.binding)) {
                send_error(stream, chh::wire::Error::credentials_changed); break;
            }
            std::string diagnostic;
            // The LA account comes from this authenticated connection, not the event payload.
            if (!chh::server::record_game_login(member.binding.key.first, uid, player_name, &diagnostic))
                std::cerr << ("游戏登录记录写入失败：" + diagnostic + "\n");
        } else if (packet.type() == chh::wire::Type::game_status) {
            chh::wire::Reader body{packet};
            uint32_t uid = 0, game_state = 0, map_id = 0, mode = 0, team = 0;
            std::string player_name;
            // Game status now carries uid, state and the UTF-8 player name
            // parsed from the game's 0x5BDE login packet.
            if (packet.length() < 10 || !body.number(uid) || !body.number(game_state) ||
                !body.string(player_name, chh::wire::profile_name_limit, true) ||
                (!body.done() && (!body.number(map_id) || !body.number(mode))) ||
                (!body.done() && !body.number(team)) || !body.done() ||
                !chh::wire::valid_game_mode(mode) ||
                team > 1 ||
                !online.update_game(member.binding, uid, game_state, std::move(player_name), map_id, mode, team)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            // Stored by accountID + observed IP; only the matching primary receives it.
        } else if (packet.type() == chh::wire::Type::chat_join && packet.length() == 0 &&
            online.current_primary(member.binding)) {
            if (!chat_joined) {
                chat_joined = true; chat_reset_requested = true;
                chat_recall_revision = lobby.recall_revision();
            }
        } else if (packet.type() == chh::wire::Type::chat_send && chat_joined &&
            online.current_primary(member.binding)) {
            uint32_t request_id = 0;
            std::string text;
            if (!chh::server::read_chat_request(packet, request_id, text)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (!chh::wire::valid_chat_text(text)) {
                if (!send_chat_result(stream, request_id, chh::wire::ChatResult::invalid_text)) break;
                continue;
            }
            const auto now = Clock::now();
            if (!chat_rate.ready(now)) {
                if (!send_chat_result(stream, request_id, chh::wire::ChatResult::rate_limited)) break;
                continue;
            }
            const auto timestamp = unix_seconds32();
            std::lock_guard gate(account_gate(profile.account_id));
            if (!still_primary()) break;
            const auto current = online.profile_snapshot(member.binding);
            profile.name = current.name;
            if (!timestamp || !lobby.publish(profile.account_id, profile.name, text, *timestamp, {}, current.hide_account_id)) {
                send_error(stream, chh::wire::Error::unavailable); break;
            }
            chat_rate.accepted(now);
            if (!send_chat_result(stream, request_id, chh::wire::ChatResult::accepted)) break;
        } else if (packet.type() == chh::wire::Type::chat_recall && chat_joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; uint64_t message_id = 0;
            if (!body.number(request_id) || !request_id || !body.u64(message_id) || !message_id || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto now = Clock::now();
            if (!chat_recall_rate.ready(now)) {
                if (!send_chat_result(stream, request_id, chh::wire::ChatResult::rate_limited)) break;
                continue;
            }
            chh::server::ChatRoom::Recall event;
            std::lock_guard gate(account_gate(profile.account_id));
            if (!still_primary()) break;
            const auto result = lobby.recall(profile.account_id, message_id, &event);
            if (result == chh::wire::ChatResult::recalled) chat_recall_rate.accepted(now);
            if (!send_chat_result(stream, request_id, result)) break;
        } else if (packet.type() == chh::wire::Type::social_join && online.current_primary(member.binding)) {
            uint32_t features = 0; bool has_notice_cursor = false; uint64_t notice_cursor = 0;
            if (!parse_social_join(packet, features, has_notice_cursor, notice_cursor)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto now = Clock::now();
            if (!social.join_rate.ready(now)) {
                if (!send_social_result(stream, 0, 8, chh::wire::SocialResult::rate_limited)) break;
                continue;
            }
            social.join_rate.accepted(now);
            const bool enhanced = features != 0;
            const bool preferences = (features & 2) != 0;
            const bool clear_enabled = (features & 4) != 0;
            const bool offline_enabled = (features & 8) != 0;
            const bool game_presence = (features & chh::wire::social_game_presence) != 0;
            const bool game_team = (features & chh::wire::social_game_team) != 0;
            if ((features & chh::wire::social_feature_local_history) != 0 && !social.local_history) {
                // History is served from the client's own store: no notices,
                // no server paging and no open-conversation poll or mappings.
                social.local_history = true; social.notices_enabled = false;
                social.peer.clear(); social.private_cursor = 0; media.reset_private({});
                social.next_delivery = {}; social.purge = {}; social.purging = true; social.next_purge = {};
            }
            if ((features & chh::wire::social_feature_private_media) != 0 && social.local_history && !social.private_media) {
                social.private_media = true; social.media_rights = {}; social.next_delivery = {};
            }
            if ((features & 16) != 0 && !social.notices_enabled && !social.local_history) {
                social.notices_enabled = true; social.notice_cursor_known = has_notice_cursor;
                social.notice_cursor = has_notice_cursor ? notice_cursor : 0; social.next_notice = {};
            }
            if ((!social.enhanced && enhanced) || (!social.preferences && preferences)) {
                social.enhanced = social.enhanced || enhanced;
                social.preferences = social.preferences || preferences;
                sent_visibility_revision = UINT64_MAX;
                if (chat_joined) chat_reset_requested = true;
            }
            if (!social.clear_enabled && clear_enabled) {
                social.clear_enabled = true; social.clears_loaded = false; social.clear_after_peer.clear();
            }
            if (!social.offline_enabled && offline_enabled) social.offline_enabled = true;
            if ((features & chh::wire::social_feature_online_time) != 0 && !social.online_time_enabled) {
                social.online_time_enabled = true; social.next_online_time_refresh = {};
            }
            if (!social.game_presence && game_presence) social.game_presence = true;
            if (!social.game_team && game_team) social.game_team = true;
            social.offline_loaded = false;
            social.contacts_loaded = false; social.conversations_revision = UINT64_MAX;
            social.joined = true; social.loaded = false;
            social.relation_revision = UINT64_MAX; social.next_refresh = {};
        } else if (packet.type() == chh::wire::Type::private_clear && social.joined && social.clear_enabled &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; std::string peer;
            if (!body.number(request_id) || !request_id || !body.string(peer, 64) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            std::lock_guard gate(account_gate(profile.account_id));
            if (!still_primary()) break;
            const auto now = Clock::now();
            uint64_t through = 0; std::string diagnostic;
            std::vector<std::string> released;
            auto result = chh::wire::SocialResult::rate_limited;
            if (social.action_rate.ready(now)) {
                social.action_rate.accepted(now);
                result = chh::server::clear_private_conversation(profile.account_id, peer, through, &diagnostic, &released);
                report_social_database_result(social, "清除私聊记录", result, diagnostic);
            }
            if (result == chh::wire::SocialResult::ok) {
                chh::server::media_collector.candidates(released);
                online.private_cleared(profile.account_id, peer, through);
                if (!sync_private_clears()) break; // Watermark precedes the initiating ACK.
            }
            if (!send_social_result(stream, request_id, 10, result)) break;
        } else if (packet.type() == chh::wire::Type::friend_note_update && social.preferences && social.joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; std::string peer, note;
            if (!body.number(request_id) || !request_id || !body.string(peer, 64) ||
                !body.string(note, chh::wire::profile_name_limit, true) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto now = Clock::now();
            auto result = chh::wire::SocialResult::invalid;
            std::string diagnostic;
            if (!social.action_rate.ready(now)) result = chh::wire::SocialResult::rate_limited;
            else {
                social.action_rate.accepted(now);
                if (chh::wire::valid_friend_note(note))
                    result = chh::server::set_friend_note(profile.account_id, peer, note, &diagnostic);
            }
            report_social_database_result(social, "修改好友备注", result, diagnostic);
            if (result == chh::wire::SocialResult::ok) {
                online.friend_notes_changed(profile.account_id); social.next_refresh = {};
            }
            if (!send_social_result(stream, request_id, 9, result)) break;
        } else if (packet.type() == chh::wire::Type::friend_action && social.joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0, action = 0; std::string peer;
            if (!body.number(request_id) || !request_id || !body.number(action) || action < 1 || action > 5 ||
                !body.string(peer, 64) || !body.done()) { send_error(stream, chh::wire::Error::protocol); break; }
            const auto now = Clock::now();
            if (!social.action_rate.ready(now)) {
                if (!send_social_result(stream, request_id, action, chh::wire::SocialResult::rate_limited)) break;
                continue;
            }
            social.action_rate.accepted(now);
            std::string diagnostic;
            const auto result = chh::server::change_friend(profile.account_id, peer, chh::wire::FriendAction(action), &diagnostic);
            static constexpr const char* operations[] = {
                "发送好友申请", "接受好友申请", "拒绝好友申请", "删除好友", "取消好友申请"};
            report_social_database_result(social, operations[action - 1], result, diagnostic);
            if (result == chh::wire::SocialResult::ok) { ++social_revision; social.next_refresh = {}; }
            if (!send_social_result(stream, request_id, action, result)) break;
        } else if (packet.type() == chh::wire::Type::private_open && social.joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; uint64_t before = 0; std::string peer;
            if (!body.number(request_id) || !request_id || !body.string(peer, 64) || !body.u64(before) || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            if (social.local_history) {
                // Pages come from the client's store; a stray request still completes.
                if (!send_private_page(stream, request_id, profile.account_id, peer, chh::server::PrivatePage{})) break;
                continue;
            }
            const bool switched = social.peer != peer;
            if (switched) { social.peer.clear(); social.private_cursor = 0; }
            const auto now = Clock::now();
            if (now < social.next_open) {
                if (!send_social_result(stream, request_id, 7, chh::wire::SocialResult::rate_limited)) break;
                continue;
            }
            social.next_open = now + std::chrono::milliseconds(500);
            chh::server::PrivatePage page; std::string diagnostic;
            const auto result = chh::server::private_history(profile.account_id, peer, before, page, &diagnostic);
            report_social_database_result(social, "读取私聊历史", result, diagnostic);
            if (result != chh::wire::SocialResult::ok) {
                if (!send_social_result(stream, request_id, 7, result)) break;
            } else {
                // A clear on another owner session may race the history query.
                // Notify before transmitting the page, then recheck its DB watermark.
                const auto pending_clears = online.private_clears(member.binding, social.clear_revision);
                const bool cleared_while_loading = pending_clears.reset ||
                    std::any_of(pending_clears.changes.begin(), pending_clears.changes.end(),
                        [&](const auto& event) { return event.peer == peer; });
                if (!sync_private_clears()) break;
                uint64_t through = 0;
                const auto clear_result = chh::server::get_private_clear_watermark(profile.account_id, peer, through, &diagnostic);
                report_social_database_result(social, "读取私聊清除位置", clear_result, diagnostic);
                if (clear_result != chh::wire::SocialResult::ok) {
                    if (!send_social_result(stream, request_id, 7, clear_result)) break;
                    continue;
                }
                if (social.clear_enabled && through && !send_private_cleared(stream, peer, through)) break;
                page.messages.erase(std::remove_if(page.messages.begin(), page.messages.end(),
                    [&](const auto& message) { return message.id <= through; }), page.messages.end());
                if (page.messages.empty()) page.has_older = false;
                page.latest_id = (std::max)(page.latest_id, through);
                if (!send_private_page(stream, request_id, profile.account_id, peer, page, &media)) break;
                if (!cleared_while_loading) {
                    social.peer = peer;
                    if (switched || !before) social.private_cursor = page.latest_id;
                } else media.reset_private(social.peer);
                social.next_private = Clock::now() + std::chrono::seconds(1);
            }
        } else if (packet.type() == chh::wire::Type::private_send && social.joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; std::string peer, nonce, text;
            if (!body.number(request_id) || !request_id || !body.string(peer, 64) || !body.string(nonce, 32) ||
                !private_request_text(packet, body.offset, text)) { send_error(stream, chh::wire::Error::protocol); break; }
            const auto now = Clock::now();
            uint64_t saved = 0; uint32_t timestamp = 0; bool inserted = false; std::string diagnostic;
            const auto result = chh::server::send_private(profile.account_id, peer, nonce, text, saved, timestamp, inserted,
                &diagnostic, social.private_rate.ready(now));
            report_social_database_result(social, "发送私聊消息", result, diagnostic);
            if (result == chh::wire::SocialResult::ok) {
                if (inserted) {
                    social.private_rate.accepted(now);
                    online.conversation_changed(profile.account_id, peer); social.next_refresh = {};
                }
                if (social.peer == peer) social.next_private = {};
                // The sender's own copy: written before the result that completes the send.
                if (social.local_history && !send_private_stored(stream, request_id, 0, saved, timestamp, peer, nonce)) break;
            }
            if (!send_social_result(stream, request_id, 6, result)) break;
        } else if (packet.type() == chh::wire::Type::private_recall && social.joined &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; uint64_t message_id = 0; std::string peer;
            if (!body.number(request_id) || !request_id || !body.string(peer, 64) ||
                !body.u64(message_id) || !message_id || !body.done()) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto now = Clock::now();
            if (!social.recall_rate.ready(now)) {
                if (!send_social_result(stream, request_id, 11, chh::wire::SocialResult::rate_limited)) break;
                continue;
            }
            std::string diagnostic;
            uint32_t timestamp = 0;
            std::vector<std::string> released;
            const auto result = chh::server::recall_private(profile.account_id, peer, message_id, timestamp, &diagnostic, &released);
            report_social_database_result(social, "撤回私聊消息", result, diagnostic);
            if (result == chh::wire::SocialResult::ok) chh::server::media_collector.candidates(released);
            // Refused recalls took the same account locks, so they are paced too.
            if (result != chh::wire::SocialResult::unavailable) social.recall_rate.accepted(now);
            if (result == chh::wire::SocialResult::ok) {
                // Without a stored row only the durable event (type 110) applies.
                if (timestamp) online.private_recalled(profile.account_id, peer, message_id, timestamp);
                online.conversation_changed(profile.account_id, peer);
                if (social.peer == peer) social.next_private = {};
            }
            if (!send_social_result(stream, request_id, 11, result)) break;
        } else if (packet.type() == chh::wire::Type::private_delivery_ack && social.joined && social.local_history &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint64_t through = 0;
            if (!body.u64(through) || !body.done()) { send_error(stream, chh::wire::Error::protocol); break; }
            if (social.delivery.acknowledge(through, Clock::now())) social.next_delivery = {};
        } else if (packet.type() == chh::wire::Type::private_recall_ack && social.joined && social.local_history &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint64_t through = 0;
            if (!body.u64(through) || !body.done()) { send_error(stream, chh::wire::Error::protocol); break; }
            if (social.recall.acknowledge(through, Clock::now())) social.next_delivery = {};
        } else if (packet.type() == chh::wire::Type::private_media_ack && social.joined && social.local_history &&
            social.private_media && online.current_primary(member.binding)) {
            // The image is durably in the client's data cache (or the server
            // answered not_found/forbidden for it): its rights can go. An
            // unknown hash is harmless; beyond the queue the next login pushes
            // the right again. The batch is applied by deliver_private on its
            // own timer, and a freed window slot is refilled by the next paced
            // rights read: nothing here wakes the message inbox.
            chh::wire::Reader body{packet};
            std::string hash;
            if (!body.string(hash, 64) || !body.done() || !chh::wire::valid_sha256_hex(hash)) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            social.media_rights.outstanding.erase(hash);
            auto& acks = social.media_rights.acks;
            if (acks.size() < private_media_ack_queue && std::find(acks.begin(), acks.end(), hash) == acks.end())
                acks.push_back(hash);
        } else if ((packet.type() == chh::wire::Type::private_export_request ||
            packet.type() == chh::wire::Type::private_export_done) && social.joined && social.local_history &&
            online.current_primary(member.binding)) {
            chh::wire::Reader body{packet};
            uint32_t request_id = 0; uint64_t value = 0;
            // Export steps are strictly sequential; a second request before the
            // previous one was answered is a client error.
            if (!body.number(request_id) || !request_id || !body.u64(value) || !body.done() || social.export_request) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const bool finish = packet.type() == chh::wire::Type::private_export_done;
            // Completion is recorded once per connection; a repeat would only
            // cost another upsert and restart the purge.
            if (!finish || !social.export_recorded) {
                social.export_request = request_id; social.export_value = value; social.export_finish = finish;
            }
        } else {
            const auto media_handled = media.handle(packet, online.current_primary(member.binding), chat_joined, social.joined,
                media_revision(), chat_rate, social.private_rate, lobby_image_visible, publish_image, lobby_image_bytes,
                private_image_saved, send_avatar);
            if (media_handled == chh::server::MediaSession::Dispatch::failed) break;
            if (media_handled == chh::server::MediaSession::Dispatch::handled) continue;
            if (media_handled == chh::server::MediaSession::Dispatch::protocol_error) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
            const auto handled = avatars.handle(packet, online.current_primary(member.binding), send_avatar);
            if (handled == chh::server::AvatarSession::Dispatch::failed) break;
            if (handled != chh::server::AvatarSession::Dispatch::handled) {
                send_error(stream, chh::wire::Error::protocol); break;
            }
        }
    }
}
}

int run_server(int argc, wchar_t** argv) {
    // Declared first so it runs last, after workers and the database are released.
    struct ShutdownSignal { ~ShutdownSignal() { if (shutdown_complete) SetEvent(shutdown_complete); } } shutdown_signal;
    // Defaults support public deployment; local tests explicitly pass --bind 127.0.0.1.
    std::wstring bind_address = L"0.0.0.0", certificate;
    unsigned long port = 5555;
    wchar_t exe[MAX_PATH]{}; GetModuleFileNameW(nullptr, exe, MAX_PATH);
    certificate = (std::filesystem::path(exe).parent_path() / L"server.pfx").wstring();
    for (int i = 1; i < argc; ++i) {
        const std::wstring option = argv[i];
        if (option == L"--help") { std::cout << "茶会服务器8：--bind IPv4地址 --port 5555 --cert server.pfx\n控制台操作：按0清屏，按1查看连接列表，按2输入客户端sock并启动explorer.exe，按q正常退出。\n"; return 0; }
        if (i + 1 >= argc) { std::cerr << "启动参数缺少对应的值。\n"; return 2; }
        if (option == L"--bind") bind_address = argv[++i];
        else if (option == L"--cert") certificate = argv[++i];
        else if (option == L"--port") {
            wchar_t* end = nullptr; port = wcstoul(argv[++i], &end, 10);
            if (!end || *end || !port || port > 65535) { std::cerr << "端口无效，允许范围为1至65535。\n"; return 2; }
        } else { std::cerr << "无法识别启动参数，请使用 --help 查看用法。\n"; return 2; }
    }
    size_t lobby_image_limit = 0; std::string lobby_image_error;
    if (!chh::server::lobby_image_memory_limit(lobby_image_limit, &lobby_image_error)) {
        std::cerr << lobby_image_error << '\n'; return 2;
    }
    lobby.set_image_limit(lobby_image_limit);
    WSADATA data{};
    const int startup_error = WSAStartup(MAKEWORD(2, 2), &data);
    if (startup_error != 0) { std::cerr << "网络组件初始化失败，错误码：" << startup_error << '\n'; return 1; }
    struct WinsockCleanup { ~WinsockCleanup() { WSACleanup(); } } winsock_cleanup;
    chh::tls::Credential credential;
    if (!credential.load_server_pfx(certificate)) { std::cerr << "TLS证书加载失败：" << certificate_error_zh(credential.last_error()) << '\n'; return 1; }
    if (!chh::server::initialize_database()) { std::cerr << "数据库组件初始化失败。\n"; return 1; }
    struct DatabaseCleanup { ~DatabaseCleanup() { chh::server::shutdown_database(); } } database_cleanup;
    OnlineTimeWorker online_time_worker;
    // Declared after the database cleanup, so the collector stops first.
    chh::server::media_collector.start([](const std::string& hash) {
        return lobby.contains_image(hash) || chh::server::media_downloading(hash);
    });
    struct CollectorStop { ~CollectorStop() { chh::server::media_collector.stop(); } } collector_stop;
    SOCKET listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listener == INVALID_SOCKET) { std::cerr << "创建监听套接字失败，错误码：" << WSAGetLastError() << '\n'; return 1; }
    struct ListenerCleanup { SOCKET s; ~ListenerCleanup() { closesocket(s); } } listener_cleanup{listener};
    BOOL exclusive = TRUE; setsockopt(listener, SOL_SOCKET, SO_EXCLUSIVEADDRUSE, reinterpret_cast<char*>(&exclusive), sizeof(exclusive));
    sockaddr_in address{}; address.sin_family = AF_INET; address.sin_port = htons(static_cast<u_short>(port));
    if (InetPtonW(AF_INET, bind_address.c_str(), &address.sin_addr) != 1) {
        std::cerr << "监听地址无效，请填写IPv4地址。\n"; return 1;
    }
    if (bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(listener, SOMAXCONN) != 0) {
        std::cerr << "端口监听失败，错误码：" << WSAGetLastError() << '\n'; return 1;
    }
    u_long nonblocking = 1;
    if (ioctlsocket(listener, FIONBIO, &nonblocking) != 0) { std::cerr << "配置监听套接字失败，错误码：" << WSAGetLastError() << '\n'; return 1; }
    SetConsoleCtrlHandler(console_signal, TRUE);
    DWORD console_mode = 0;
    const bool interactive = GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &console_mode) != 0;
    std::cout << "茶会服务器8已启动，正在监听端口 " << port << "。按0清屏，按1查看连接列表，按2输入客户端sock并启动explorer.exe，按q正常退出。\n" << std::flush;
    struct Worker { std::thread thread; std::shared_ptr<std::atomic_bool> done; unsigned long ip = 0; };
    std::vector<Worker> workers;
    // Live connections per source IP (accept thread only). Handshake and login
    // each wait up to 10 s, so without this one host could hold every slot.
    // The cap still leaves room for many primary + game pairs behind one NAT.
    constexpr size_t max_workers = 200, max_workers_per_ip = 32;
    std::map<unsigned long, size_t> workers_per_ip;
    bool entering_explorer_socket = false, skip_extended_key = false;
    std::string explorer_socket_input;
    chh::server::Sessions::ConnectionsSnapshot explorer_candidates;
    bool failed = false;
    try {
        while (!stopping.load()) {
            for (auto it = workers.begin(); it != workers.end();) {
                if (!it->done->load()) { ++it; continue; }
                it->thread.join();
                if (--workers_per_ip[it->ip] == 0) workers_per_ip.erase(it->ip);
                it = workers.erase(it);
            }
            if (interactive && _kbhit()) {
                const int key = _getch();
                if (skip_extended_key) skip_extended_key = false;
                else if (key == 0 || key == 224) skip_extended_key = true;
                else if (entering_explorer_socket) {
                    if (key == 27) {
                        entering_explorer_socket = false;
                        explorer_socket_input.clear(); explorer_candidates = {};
                        std::cout << "\n已取消。\n" << std::flush;
                    } else if (key == '\r' || key == '\n') {
                        entering_explorer_socket = false;
                        std::cout << '\n';
                        request_console_explorer(explorer_socket_input, explorer_candidates);
                        explorer_socket_input.clear(); explorer_candidates = {};
                    } else if (key == '\b' && !explorer_socket_input.empty()) {
                        explorer_socket_input.pop_back(); std::cout << "\b \b" << std::flush;
                    } else if (key >= '0' && key <= '9' && explorer_socket_input.size() < 21) {
                        explorer_socket_input.push_back(static_cast<char>(key));
                        std::cout << static_cast<char>(key) << std::flush;
                    }
                } else {
                    if (key == 'q' || key == 'Q') { stopping.store(true); break; }
                    if (key == '0') { clear_console(); continue; }
                    if (key == '1') print_online_clients();
                    if (key == '2') {
                        explorer_candidates = online.connections_snapshot();
                        explorer_socket_input.clear(); entering_explorer_socket = true;
                        std::cout << "请输入 LA 客户端 sock（回车确认，Esc取消）：" << std::flush;
                    }
                }
            }
            fd_set reads; FD_ZERO(&reads); FD_SET(listener, &reads); timeval timeout{0, 200000};
            const int ready = select(0, &reads, nullptr, nullptr, &timeout);
            if (ready == SOCKET_ERROR) { std::cerr << "监听轮询失败，错误码：" << WSAGetLastError() << '\n'; failed = true; break; }
            if (!ready) continue;
            sockaddr_in peer{}; int peer_size = sizeof(peer);
            SOCKET accepted = accept(listener, reinterpret_cast<sockaddr*>(&peer), &peer_size);
            if (accepted == INVALID_SOCKET) continue;
            const unsigned long ip = peer.sin_addr.s_addr;
            const auto per_ip = workers_per_ip.find(ip);
            if (workers.size() >= max_workers || (per_ip != workers_per_ip.end() && per_ip->second >= max_workers_per_ip) ||
                limiter.blocked(ip)) { closesocket(accepted); continue; }
            auto done = std::make_shared<std::atomic_bool>(false);
            workers.push_back(Worker{}); workers.back().done = done; workers.back().ip = ip;
            try {
                workers.back().thread = std::thread([accepted, ip, &credential, done] {
                    try { serve(accepted, ip, credential); } catch (...) { /* Stream owns socket during serve. */ }
                    done->store(true);
                });
            } catch (...) { closesocket(accepted); workers.pop_back(); throw; }
            ++workers_per_ip[ip];
        }
    } catch (...) { std::cerr << "服务器因内部错误停止。\n"; failed = true; }
    stopping.store(true);
    for (auto& worker : workers) if (worker.thread.joinable()) worker.thread.join();
    SetConsoleCtrlHandler(console_signal, FALSE);
    return failed ? 1 : 0;
}

int wmain(int argc, wchar_t** argv) {
    return chh::startup::run(argc, argv, run_server);
}

