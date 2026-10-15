#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <iostream>
#include <map>
#include <mutex>
#include <string>

namespace chh::server {

// Console report for an optional feature that is temporarily unavailable:
// "<prefix><diagnostic or fallback>", at most once per 30 seconds.
// Thread-safe, so one static instance can serve every worker.
class ThrottledReport {
public:
    using Clock = std::chrono::steady_clock;
    ThrottledReport(const char* prefix, const char* fallback) : prefix_(prefix), fallback_(fallback) {}
    void operator()(const std::string& diagnostic) {
        std::lock_guard<std::mutex> lock(mutex_);
        const auto now = Clock::now();
        if (last_ != Clock::time_point{} && now - last_ < std::chrono::seconds(30)) return;
        last_ = now;
        std::cerr << (prefix_ + (diagnostic.empty() ? std::string(fallback_) : diagnostic) + "\n");
    }
private:
    std::string prefix_;
    const char* fallback_;
    std::mutex mutex_;
    Clock::time_point last_{};
};

// One instance per client connection. Operation names are fixed code labels,
// never account names or message text. A different operation's success must
// not hide an outstanding failure, nor can another client's success do so.
class SocialDiagnostics {
public:
    using Clock = std::chrono::steady_clock;

    SocialDiagnostics() : connection_id_(next_id().fetch_add(1, std::memory_order_relaxed)) {}
    SocialDiagnostics(const SocialDiagnostics&) = delete;
    SocialDiagnostics& operator=(const SocialDiagnostics&) = delete;

    ~SocialDiagnostics() noexcept {
        try {
            for (const auto& entry : failures_) {
                const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(Clock::now() - entry.second.first).count();
                emit(entry.first + "：连接结束，尚未确认恢复；累计失败 " +
                    std::to_string(entry.second.count) + " 次；距首次失败 " + std::to_string(elapsed) + " ms");
            }
        } catch (...) {}
    }

    void failure(const char* operation, const std::string& diagnostic,
        Clock::time_point now = Clock::now()) noexcept {
        try {
            auto& failure = failures_[operation];
            if (!failure.count) failure.first = now;
            ++failure.count;
            if (failure.count != 1 && now - failure.last_report < std::chrono::seconds(30)) return;
            failure.last_report = now;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - failure.first).count();
            emit(std::string(operation) + "失败；" +
                (diagnostic.empty() ? "数据库操作未完成" : diagnostic) +
                "；累计失败 " + std::to_string(failure.count) + " 次；距首次失败 " + std::to_string(elapsed) + " ms");
        } catch (...) {}
    }

    void recovered(const char* operation, Clock::time_point now = Clock::now()) noexcept {
        try {
            if (failures_.empty()) return;
            const auto found = failures_.find(operation);
            if (found == failures_.end()) return;
            const auto elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(now - found->second.first).count();
            emit(std::string(operation) + "已恢复；距首次失败 " + std::to_string(elapsed) +
                " ms；累计失败 " + std::to_string(found->second.count) + " 次");
            failures_.erase(found);
        } catch (...) {}
    }

private:
    struct Failure {
        Clock::time_point first{}, last_report{};
        std::uint64_t count = 0;
    };
    std::uint64_t connection_id_;
    std::map<std::string, Failure> failures_;

    static std::atomic<std::uint64_t>& next_id() {
        static std::atomic<std::uint64_t> value{1};
        return value;
    }

    void emit(const std::string& message) const {
        SYSTEMTIME local{};
        GetLocalTime(&local);
        char timestamp[40]{};
        std::snprintf(timestamp, sizeof(timestamp), "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
            unsigned(local.wYear), unsigned(local.wMonth), unsigned(local.wDay),
            unsigned(local.wHour), unsigned(local.wMinute), unsigned(local.wSecond), unsigned(local.wMilliseconds));
        const auto line = std::string(timestamp) + "[好友/私聊][连接#" +
            std::to_string(connection_id_) + "] " + message + "\n";
        static std::mutex mutex;
        std::lock_guard<std::mutex> lock(mutex);
        std::cerr << line;
    }
};

} // namespace chh::server
