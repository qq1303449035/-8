#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace chh::server {
// One ledger per server process. Only admitted primary sessions call start/finish.
// Database I/O never holds the ledger mutex; a disconnect/reconnect can proceed
// while an earlier checkpoint is in flight. Finished, unsaved entries survive
// disconnects and are retried by the same worker.
class OnlineTimeTracker {
public:
    using Clock = std::chrono::steady_clock;
    using Time = Clock::time_point;
    bool start(const std::string& account, uint64_t token, Time now = Clock::now()) {
        if (account.empty() || !token) return false;
        std::lock_guard lock(mutex_);
        auto found = entries_.find(account);
        if (found == entries_.end()) {
            auto fresh = std::make_shared<Entry>();
            fresh->next_flush = now + std::chrono::seconds(60);
            found = entries_.emplace(account, std::move(fresh)).first;
        }
        auto& entry = found->second;
        if (entry->token) return false;
        entry->token = token; entry->started = now;
        return true;
    }
    bool finish(const std::string& account, uint64_t token, Time now = Clock::now()) {
        std::lock_guard lock(mutex_);
        const auto found = entries_.find(account);
        if (found == entries_.end() || !token || found->second->token != token) return false;
        auto& entry = *found->second;
        if (now > entry.started) entry.elapsed += now - entry.started;
        entry.token = 0; entry.next_flush = now;
        return true;
    }
    // load(account, base, diagnostic), save(account, absolute_total, diagnostic).
    // A fixed baseline + accumulated duration makes retries idempotent. Never
    // reload that baseline after a save whose acknowledgement may have failed.
    template<class Load, class Save, class Report>
    void flush_due(Time now, Load load, Save save, Report report, bool force = false) {
        std::lock_guard flush_lock(flush_mutex_);
        struct Work { std::string account; std::shared_ptr<Entry> entry; uint64_t seconds; };
        std::vector<Work> work;
        {
            std::lock_guard lock(mutex_);
            for (const auto& [account, entry] : entries_) {
                if (!force && now < entry->next_flush) continue;
                work.push_back({account, entry, seconds_at(*entry, now)});
                entry->next_flush = now + std::chrono::seconds(60);
            }
        }
        for (auto& item : work) {
            auto& entry = *item.entry;
            std::string diagnostic;
            bool ok = true;
            try {
                if (item.seconds && !entry.loaded) {
                    ok = load(item.account, entry.base, &diagnostic);
                    if (ok) { entry.loaded = true; entry.saved = entry.base; }
                }
                if (ok && item.seconds) {
                    const uint64_t target = entry.base + (std::min)(item.seconds, UINT64_MAX - entry.base);
                    if (target > entry.saved) {
                        ok = save(item.account, target, &diagnostic);
                        if (ok) entry.saved = target;
                    }
                }
            } catch (...) { ok = false; diagnostic = "在线时长保存发生本地异常"; }
            {
                std::lock_guard lock(mutex_);
                if (!ok) entry.next_flush = (std::min)(entry.next_flush, now + std::chrono::seconds(5));
                // The session may have ended or restarted during the I/O. Only
                // discard an inactive entry after all its whole seconds were saved.
                else if (!entry.token && seconds_at(entry, now) <= item.seconds) entries_.erase(item.account);
            }
            if (!ok) report(diagnostic);
        }
    }
    size_t pending_accounts() const {
        std::lock_guard lock(mutex_);
        return entries_.size();
    }
private:
    struct Entry {
        uint64_t token = 0, base = 0, saved = 0;
        bool loaded = false;
        Time started{}, next_flush{};
        Clock::duration elapsed{};
    };
    static uint64_t seconds_at(const Entry& entry, Time now) {
        auto elapsed = entry.elapsed;
        if (entry.token && now > entry.started) elapsed += now - entry.started;
        return static_cast<uint64_t>(std::chrono::duration_cast<std::chrono::seconds>(elapsed).count());
    }
    mutable std::mutex mutex_;
    std::mutex flush_mutex_;
    std::map<std::string, std::shared_ptr<Entry>> entries_;
};
}
