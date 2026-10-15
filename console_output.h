#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <iostream>
#include <mutex>
#include <streambuf>

namespace chh::console {
namespace detail {

// The program's narrow strings are UTF-8 (/utf-8). Console handles receive
// UTF-16 directly; redirected streams retain their original UTF-8 bytes.
class Utf8OutputBuffer final : public std::streambuf {
public:
    Utf8OutputBuffer(HANDLE handle, std::streambuf* original) noexcept
        : handle_(handle), original_(original) {
        DWORD mode = 0;
        console_ = GetConsoleMode(handle_, &mode) != FALSE;
    }

    void finish() noexcept {
        try {
            std::lock_guard<std::mutex> lock(mutex_);
            if (console_) {
                if (remaining_) emit(0xfffd);
                remaining_ = 0;
                flush_wide();
            } else if (original_) {
                original_->pubsync();
            }
        } catch (...) {}
    }

protected:
    std::streamsize xsputn(const char* data, std::streamsize size) override {
        if (size <= 0) return 0;
        std::lock_guard<std::mutex> lock(mutex_);
        if (!console_) return original_ ? original_->sputn(data, size) : 0;
        if (failed_) return 0;
        for (std::streamsize i = 0; i < size; ++i) {
            consume(static_cast<unsigned char>(data[i]));
            if (failed_) return i;
        }
        return flush_wide() ? size : 0;
    }

    int_type overflow(int_type value) override {
        if (traits_type::eq_int_type(value, traits_type::eof()))
            return sync() == 0 ? traits_type::not_eof(value) : traits_type::eof();
        const char character = traits_type::to_char_type(value);
        return xsputn(&character, 1) == 1 ? value : traits_type::eof();
    }

    int sync() override {
        std::lock_guard<std::mutex> lock(mutex_);
        // Keep an incomplete UTF-8 character across flushes: cerr is unitbuf,
        // and an insertion may contain only part of a multibyte character.
        if (console_) return flush_wide() ? 0 : -1;
        return original_ ? original_->pubsync() : -1;
    }

private:
    HANDLE handle_;
    std::streambuf* original_;
    std::mutex mutex_;
    bool console_ = false;
    bool failed_ = false;
    bool previous_cr_ = false;
    unsigned remaining_ = 0;
    std::uint32_t scalar_ = 0;
    std::uint32_t minimum_ = 0;
    wchar_t wide_[1024]{};
    DWORD used_ = 0;

    bool flush_wide() noexcept {
        DWORD offset = 0;
        while (!failed_ && offset < used_) {
            DWORD written = 0;
            if (!WriteConsoleW(handle_, wide_ + offset, used_ - offset, &written, nullptr) || !written)
                failed_ = true;
            else offset += written;
        }
        used_ = 0;
        return !failed_;
    }

    void emit(std::uint32_t scalar) noexcept {
        // Leave room for a surrogate pair or CRLF without splitting the pair
        // between WriteConsoleW calls. Match the CRT's text-mode newlines.
        if (used_ + 2 > sizeof(wide_) / sizeof(wide_[0])) flush_wide();
        if (failed_) return;
        if (scalar == '\n' && !previous_cr_) wide_[used_++] = L'\r';
        previous_cr_ = scalar == '\r';
        if (scalar <= 0xffff) wide_[used_++] = static_cast<wchar_t>(scalar);
        else {
            scalar -= 0x10000;
            wide_[used_++] = static_cast<wchar_t>(0xd800 + (scalar >> 10));
            wide_[used_++] = static_cast<wchar_t>(0xdc00 + (scalar & 0x3ff));
        }
    }

    void consume(unsigned char byte) noexcept {
        if (remaining_) {
            if ((byte & 0xc0) == 0x80) {
                scalar_ = (scalar_ << 6) | (byte & 0x3f);
                if (--remaining_ == 0) {
                    const bool valid = scalar_ >= minimum_ && scalar_ <= 0x10ffff &&
                        !(scalar_ >= 0xd800 && scalar_ <= 0xdfff);
                    emit(valid ? scalar_ : 0xfffd);
                }
                return;
            }
            remaining_ = 0;
            emit(0xfffd);
        }
        if (byte < 0x80) emit(byte);
        else if (byte >= 0xc2 && byte <= 0xdf) {
            scalar_ = byte & 0x1f; minimum_ = 0x80; remaining_ = 1;
        } else if (byte >= 0xe0 && byte <= 0xef) {
            scalar_ = byte & 0x0f; minimum_ = 0x800; remaining_ = 2;
        } else if (byte >= 0xf0 && byte <= 0xf4) {
            scalar_ = byte & 0x07; minimum_ = 0x10000; remaining_ = 3;
        } else emit(0xfffd);
    }
};
} // namespace detail

class OutputRedirect final {
public:
    OutputRedirect()
        : original_out_(std::cout.rdbuf()), original_err_(std::cerr.rdbuf()),
          original_log_(std::clog.rdbuf()),
          output_(GetStdHandle(STD_OUTPUT_HANDLE), original_out_),
          error_(GetStdHandle(STD_ERROR_HANDLE), original_err_),
          log_(GetStdHandle(STD_ERROR_HANDLE), original_log_) {
        std::cout.flush();
        std::cerr.flush();
        std::clog.flush();
        std::cout.rdbuf(&output_);
        std::cerr.rdbuf(&error_);
        std::clog.rdbuf(&log_);
    }

    ~OutputRedirect() noexcept {
        output_.finish();
        error_.finish();
        log_.finish();
        try { std::cout.rdbuf(original_out_); } catch (...) {}
        try { std::cerr.rdbuf(original_err_); } catch (...) {}
        try { std::clog.rdbuf(original_log_); } catch (...) {}
    }
    OutputRedirect(const OutputRedirect&) = delete;
    OutputRedirect& operator=(const OutputRedirect&) = delete;

private:
    std::streambuf* original_out_;
    std::streambuf* original_err_;
    std::streambuf* original_log_;
    detail::Utf8OutputBuffer output_;
    detail::Utf8OutputBuffer error_;
    detail::Utf8OutputBuffer log_;
};
} // namespace chh::console
