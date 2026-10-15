#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <conio.h>
#include <cstdio>
#include <cwchar>
#include <exception>
#include <iostream>
#include <mutex>
#include <streambuf>
#include <string>
#include <vector>
#include "console_output.h"

// Usage: int wmain(int argc, wchar_t** argv) {
//     return chh::startup::run(argc, argv, run_server);
// }
// Entry diagnostics must contain error descriptions, never credential values.
// Arguments/environment and arbitrary exception.what() strings are not logged.
namespace chh::startup {
namespace detail {

inline void direct_message(std::streambuf* destination, const char* message) noexcept {
    if (!destination) return;
    try {
        destination->sputn(message, static_cast<std::streamsize>(std::char_traits<char>::length(message)));
        destination->pubsync();
    } catch (...) {}
}

class StartupLog {
public:
    explicit StartupLog(std::streambuf* console) noexcept : console_(console) {
        try {
            std::vector<wchar_t> path(32768);
            const DWORD length = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
            if (!length || length >= path.size()) {
                report_failure("定位程序所在目录", length ? ERROR_INSUFFICIENT_BUFFER : GetLastError());
                return;
            }
            std::wstring filename(path.data(), length);
            const size_t separator = filename.find_last_of(L"\\/");
            if (separator == std::wstring::npos) {
                report_failure("定位程序所在目录", ERROR_PATH_NOT_FOUND);
                return;
            }
            filename.resize(separator + 1);
            filename += L"server-startup.log";
            handle_ = CreateFileW(filename.c_str(), FILE_APPEND_DATA,
                FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_ALWAYS,
                FILE_ATTRIBUTE_NORMAL | FILE_FLAG_WRITE_THROUGH, nullptr);
            if (handle_ == INVALID_HANDLE_VALUE) report_failure("打开 server-startup.log", GetLastError());
        } catch (...) { report_failure("准备 server-startup.log", ERROR_NOT_ENOUGH_MEMORY); }
    }
    ~StartupLog() { if (handle_ != INVALID_HANDLE_VALUE) CloseHandle(handle_); }
    StartupLog(const StartupLog&) = delete;
    StartupLog& operator=(const StartupLog&) = delete;

    void line(const std::string& text) noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (handle_ == INVALID_HANDLE_VALUE) return;
            SYSTEMTIME time{};
            GetLocalTime(&time);
            char timestamp[48]{};
            std::snprintf(timestamp, sizeof(timestamp),
                "[%04u-%02u-%02u %02u:%02u:%02u.%03u] ",
                time.wYear, time.wMonth, time.wDay, time.wHour,
                time.wMinute, time.wSecond, time.wMilliseconds);
            std::string lower = text;
            for (char& character : lower)
                if (character >= 'A' && character <= 'Z') character = static_cast<char>(character - 'A' + 'a');
            bool sensitive = false;
            for (const char* marker : {"password=", "password:", "password =", "password :",
                    "passwd=", "passwd:", "pwd=", "pwd:", "secret=", "secret:",
                    "token=", "token:", "authorization:", "bearer ", "username=",
                    "username:", "user=", "--password", "--token", "://"}) {
                if (lower.find(marker) != std::string::npos) { sensitive = true; break; }
            }
            const std::string record = std::string(timestamp) +
                (sensitive ? "[含凭据的诊断内容已省略]" : text) + "\r\n";
            size_t offset = 0;
            while (offset < record.size()) {
                DWORD written = 0;
                if (!WriteFile(handle_, record.data() + offset,
                        static_cast<DWORD>(record.size() - offset), &written, nullptr) || !written) {
                    const DWORD error = GetLastError();
                    CloseHandle(handle_);
                    handle_ = INVALID_HANDLE_VALUE;
                    report_failure("写入 server-startup.log", error);
                    return;
                }
                offset += written;
            }
        } catch (...) { report_failure("格式化 server-startup.log 记录", ERROR_NOT_ENOUGH_MEMORY); }
    }

private:
    HANDLE handle_ = INVALID_HANDLE_VALUE;
    std::streambuf* console_ = nullptr;
    std::mutex mutex_;
    void report_failure(const char* operation, DWORD error) noexcept {
        char message[256]{};
        std::snprintf(message, sizeof(message),
            "启动诊断：无法%s（Windows 错误码 %lu）。错误信息仍会显示在此处。\n",
            operation, static_cast<unsigned long>(error));
        direct_message(console_, message);
    }
};

class ErrorTee final : public std::streambuf {
public:
    ErrorTee(std::streambuf* console, StartupLog& log) : console_(console), log_(log) {}
    void finish() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (!pending_.empty()) { log_.line(pending_); pending_.clear(); }
            if (console_) console_->pubsync();
        } catch (...) { direct_message(console_, "启动诊断：无法完成错误日志写入。\n"); }
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (size <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        // Display each insertion immediately; log only complete lines so a
        // credential marker split across ostream insertions is still redacted.
        // The console is best effort: a closed console or stderr pipe must not
        // set badbit on std::cerr and so stop the log file too.
        if (console_) {
            console_->sputn(data, size);
            console_->pubsync();
        }
        try {
            for (std::streamsize i = 0; i < size; ++i) {
                if (data[i] == '\n') {
                    if (!pending_.empty() && pending_.back() == '\r') pending_.pop_back();
                    if (!discarding_) log_.line(pending_);
                    pending_.clear();
                    discarding_ = false;
                } else if (!discarding_) {
                    if (pending_.size() < 16384) pending_ += data[i];
                    else {
                        log_.line("[过长的诊断记录已省略]");
                        pending_.clear();
                        discarding_ = true;
                    }
                }
            }
        } catch (...) { direct_message(console_, "启动诊断：无法缓存待写入日志的错误信息。\n"); }
        return size;
    }
    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof())) return traits_type::not_eof(value);
        const char character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }
    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        if (console_) console_->pubsync();
        return 0;
    }

private:
    std::streambuf* console_;
    StartupLog& log_;
    std::mutex mutex_;
    std::string pending_;
    bool discarding_ = false;
};

class RedirectErrors {
public:
    explicit RedirectErrors(ErrorTee& tee) : tee_(tee), original_(std::cerr.rdbuf(&tee_)) {}
    ~RedirectErrors() noexcept {
        tee_.finish();
        try { std::cerr.rdbuf(original_); }
        catch (...) { direct_message(original_, "启动诊断：无法恢复标准错误输出。\n"); }
    }
    RedirectErrors(const RedirectErrors&) = delete;
    RedirectErrors& operator=(const RedirectErrors&) = delete;
private:
    ErrorTee& tee_;
    std::streambuf* original_;
};

inline bool owns_interactive_console() noexcept {
    DWORD input_mode = 0;
    if (!GetConsoleMode(GetStdHandle(STD_INPUT_HANDLE), &input_mode)) return false;
    DWORD process_ids[2]{};
    return GetConsoleProcessList(process_ids, 2) == 1;
}
} // namespace detail

inline int run(int argc, wchar_t** argv, int (*entry)(int, wchar_t**)) {
    // Keep Unicode console output active through both logging and the final
    // error/pause prompt. ErrorTee still receives UTF-8 for the log file.
    chh::console::OutputRedirect console_output;
    bool no_pause = false;
    // Determine this before any allocation so even a setup failure honors it.
    for (int i = 1; argv && i < argc; ++i)
        if (argv[i] && std::wcscmp(argv[i], L"--no-pause") == 0) no_pause = true;
    int result = 1;
    try {
        detail::StartupLog log(std::cerr.rdbuf());
        detail::ErrorTee tee(std::cerr.rdbuf(), log);
        detail::RedirectErrors redirect(tee);
        log.line("服务器启动（不记录启动参数和凭据）");
        try {
            std::vector<wchar_t*> forwarded;
            for (int i = 0; argv && i < argc; ++i)
                if (i == 0 || !argv[i] || std::wcscmp(argv[i], L"--no-pause") != 0)
                    forwarded.push_back(argv[i]);
            const int forwarded_count = static_cast<int>(forwarded.size());
            forwarded.push_back(nullptr);
            if (!entry) std::cerr << "服务器启动失败：缺少程序入口。\n";
            else result = entry(forwarded_count, forwarded.data());
        } catch (const std::exception&) {
            std::cerr << "服务器因未处理的 C++ 异常而停止。异常详情可能含有凭据，已省略。\n";
            result = 1;
        } catch (...) {
            std::cerr << "服务器因未处理的异常而停止。\n";
            result = 1;
        }
        tee.finish();
        log.line("服务器已结束运行；退出码 " + std::to_string(result));
    } catch (...) {
        detail::direct_message(std::cerr.rdbuf(), "服务器启动诊断发生意外错误。\n");
        result = 1;
    }
    if (result != 0 && !no_pause && detail::owns_interactive_console()) {
        detail::direct_message(std::cerr.rdbuf(), "启动失败。请查看上方错误信息或 server-startup.log。按任意键关闭此窗口……\n");
        (void)_getwch();
    }
    return result;
}
} // namespace chh::startup
