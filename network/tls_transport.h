#pragma once

// Windows desktop, C++20. The caller initializes Winsock and connects/accepts TCP.
// Credential c; c.initialize_client(); Stream s(connected_socket, &stop);
// s.connect(c, "server.example", sha256_of_certificate_der_hex);
// Server: c.load_server_pfx(L"server.pfx", L"password"); s.accept(c);
// Every Stream owns its socket and is used by ONE I/O thread. Share initialized
// Credentials across streams, but do not reconfigure a Credential concurrently.
// wait_readable() returning false with timed_out()==true is an ordinary idle wait.
// All other failures, including exact-read/write/handshake timeouts, close the
// stream. A partial read may have modified the destination: discard it on failure.
// Pinning authenticates the certificate itself; DNS/CA trust is not consulted.
// Renegotiation is intentionally refused. There is no plaintext fallback.

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif
#include <winsock2.h>
#include <windows.h>
#include <security.h>
#include <schannel.h>
#include <wincrypt.h>
#include <bcrypt.h>
#include <ncrypt.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "ncrypt.lib")
#pragma comment(lib, "ws2_32.lib")

namespace chh::tls {
namespace detail {
// Accepts DWORD, WSA int errors and SECURITY_STATUS without call-site casts.
template <typename Code>
inline std::string error_code(const char* operation, Code code) {
    char hex[16]{};
    std::snprintf(hex, sizeof(hex), "0x%08lX", static_cast<unsigned long>(code));
    return std::string(operation) + " failed (" + hex + ")";
}

struct CredentialState {
    CredHandle handle{};
    HCERTSTORE store = nullptr;
    PCCERT_CONTEXT certificate = nullptr;
    NCRYPT_PROV_HANDLE key_provider = 0;
    NCRYPT_KEY_HANDLE owned_key = 0;
    bool server = false;

    CredentialState() { SecInvalidateHandle(&handle); }
    ~CredentialState() {
        if (SecIsValidHandle(&handle)) FreeCredentialsHandle(&handle);
        if (certificate) CertFreeCertificateContext(certificate);
        if (store) CertCloseStore(store, 0);
        if (owned_key && NCryptDeleteKey(owned_key, 0) != ERROR_SUCCESS)
            NCryptFreeObject(owned_key);
        if (key_provider) NCryptFreeObject(key_provider);
    }
    CredentialState(const CredentialState&) = delete;
    CredentialState& operator=(const CredentialState&) = delete;
};

struct FileHandle {
    HANDLE handle = INVALID_HANDLE_VALUE;
    explicit FileHandle(HANDLE value) : handle(value) {}
    ~FileHandle() { if (handle != INVALID_HANDLE_VALUE) CloseHandle(handle); }
    FileHandle(const FileHandle&) = delete;
    FileHandle& operator=(const FileHandle&) = delete;
};

struct SecretBytes {
    std::vector<BYTE> bytes;
    ~SecretBytes() { if (!bytes.empty()) SecureZeroMemory(bytes.data(), bytes.size()); }
};
} // namespace detail

class Stream;

class Credential {
public:
    Credential() = default;
    Credential(const Credential&) = delete;
    Credential& operator=(const Credential&) = delete;

    bool initialize_client() {
        error_.clear();
        state_.reset();
        auto state = std::make_shared<detail::CredentialState>();
        SCHANNEL_CRED config{};
        config.dwVersion = SCHANNEL_CRED_VERSION;
        config.grbitEnabledProtocols = SP_PROT_TLS1_2_CLIENT;
        config.dwMinimumCipherStrength = 128;
        config.dwFlags = SCH_CRED_MANUAL_CRED_VALIDATION |
            SCH_CRED_NO_DEFAULT_CREDS | SCH_USE_STRONG_CRYPTO;
        return acquire(std::move(state), config, SECPKG_CRED_OUTBOUND);
    }

    bool load_server_pfx(const std::wstring& path,
                         const std::wstring& password = L"") {
        error_.clear();
        state_.reset();
        if (path.empty() || path.find(L'\0') != std::wstring::npos ||
            password.find(L'\0') != std::wstring::npos) {
            error_ = "Invalid PFX path or password";
            return false;
        }
        detail::FileHandle file(CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ,
            nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
        if (file.handle == INVALID_HANDLE_VALUE) {
            error_ = detail::error_code("Open PFX", GetLastError());
            return false;
        }
        LARGE_INTEGER length{};
        if (!GetFileSizeEx(file.handle, &length) || length.QuadPart <= 0 ||
            length.QuadPart > 4 * 1024 * 1024) {
            error_ = "PFX must be between 1 byte and 4 MiB";
            return false;
        }
        std::vector<BYTE> bytes(static_cast<size_t>(length.QuadPart));
        DWORD read = 0;
        const BOOL read_ok = ReadFile(file.handle, bytes.data(),
            static_cast<DWORD>(bytes.size()), &read, nullptr);
        const DWORD read_error = read_ok ? ERROR_HANDLE_EOF : GetLastError();
        if (!read_ok || read != bytes.size()) {
            SecureZeroMemory(bytes.data(), bytes.size());
            error_ = detail::error_code("Read PFX", read_error);
            return false;
        }
        auto state = std::make_shared<detail::CredentialState>();
        CRYPT_DATA_BLOB blob{static_cast<DWORD>(bytes.size()), bytes.data()};
        // Decode without installing the PFX's original key container. A uniquely
        // named temporary CNG container is created below for Schannel/LSASS.
        state->store = PFXImportCertStore(&blob, password.c_str(),
            PKCS12_NO_PERSIST_KEY | PKCS12_ALWAYS_CNG_KSP | CRYPT_EXPORTABLE);
        const DWORD import_error = GetLastError();
        SecureZeroMemory(bytes.data(), bytes.size());
        if (!state->store) {
            error_ = detail::error_code("Import PFX", import_error);
            return false;
        }
        PCCERT_CONTEXT candidate = nullptr;
        while ((candidate = CertEnumCertificatesInStore(state->store, candidate))) {
            DWORD key_size = 0;
            if (CertGetCertificateContextProperty(candidate,
                    CERT_KEY_CONTEXT_PROP_ID, nullptr, &key_size)) {
                if (state->certificate) {
                    CertFreeCertificateContext(candidate);
                    error_ = "PFX must contain exactly one certificate with a private key";
                    return false;
                }
                state->certificate = CertDuplicateCertificateContext(candidate);
            }
        }
        if (!state->certificate) {
            error_ = "PFX has no certificate with an accessible private key";
            return false;
        }
        if (CertVerifyTimeValidity(nullptr, state->certificate->pCertInfo) != 0) {
            error_ = "Server certificate is expired or not yet valid";
            return false;
        }
        if (!make_schannel_key(*state)) return false;
        state->server = true;
        SCHANNEL_CRED config{};
        config.dwVersion = SCHANNEL_CRED_VERSION;
        config.cCreds = 1;
        config.paCred = &state->certificate;
        config.grbitEnabledProtocols = SP_PROT_TLS1_2_SERVER;
        config.dwMinimumCipherStrength = 128;
        config.dwFlags = SCH_USE_STRONG_CRYPTO | SCH_CRED_NO_SYSTEM_MAPPER |
            SCH_CRED_CACHE_ONLY_URL_RETRIEVAL_ON_CREATE;
        return acquire(std::move(state), config, SECPKG_CRED_INBOUND);
    }

    const std::string& last_error() const noexcept { return error_; }

private:
    friend class Stream;
    std::shared_ptr<detail::CredentialState> state_;
    std::string error_;

    bool make_schannel_key(detail::CredentialState& state) {
        // Schannel may marshal certificate credentials to LSASS, where an
        // ephemeral process-local CNG handle cannot be used. Persist an isolated,
        // non-exportable copy under the current user's protected key store and
        // delete it when the last Stream releases this CredentialState. Abrupt
        // process termination can leave this randomly named container behind.
        CERT_KEY_CONTEXT original{};
        DWORD context_size = sizeof(original);
        if (!CertGetCertificateContextProperty(state.certificate,
                CERT_KEY_CONTEXT_PROP_ID, &original, &context_size) ||
            original.dwKeySpec != CERT_NCRYPT_KEY_SPEC) {
            error_ = "PFX private key is not a CNG key";
            return false;
        }
        DWORD export_policy = NCRYPT_ALLOW_EXPORT_FLAG | NCRYPT_ALLOW_PLAINTEXT_EXPORT_FLAG;
        SECURITY_STATUS status = NCryptSetProperty(original.hNCryptKey,
            NCRYPT_EXPORT_POLICY_PROPERTY, reinterpret_cast<PBYTE>(&export_policy),
            sizeof(export_policy), 0);
        if (status != ERROR_SUCCESS) {
            error_ = detail::error_code("Prepare temporary private key", status);
            return false;
        }
        DWORD byte_count = 0;
        status = NCryptExportKey(original.hNCryptKey, 0,
            NCRYPT_PKCS8_PRIVATE_KEY_BLOB, nullptr, nullptr, 0, &byte_count, 0);
        if (status != ERROR_SUCCESS || byte_count == 0 || byte_count > 1024 * 1024) {
            error_ = detail::error_code("Size private key", status);
            return false;
        }
        detail::SecretBytes exported;
        exported.bytes.resize(byte_count);
        status = NCryptExportKey(original.hNCryptKey, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
            nullptr, exported.bytes.data(), byte_count, &byte_count, 0);
        if (status != ERROR_SUCCESS) {
            error_ = detail::error_code("Export temporary private key", status);
            return false;
        }
        std::array<BYTE, 16> random{};
        if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
                BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
            error_ = "Cannot generate temporary private key name";
            return false;
        }
        std::wstring name = L"chh8d-tls-";
        constexpr wchar_t digits[] = L"0123456789abcdef";
        for (BYTE value : random) { name += digits[value >> 4]; name += digits[value & 15]; }
        status = NCryptOpenStorageProvider(&state.key_provider, MS_KEY_STORAGE_PROVIDER, 0);
        if (status != ERROR_SUCCESS) {
            error_ = detail::error_code("Open private key provider", status);
            return false;
        }
        NCryptBuffer name_buffer{static_cast<ULONG>((name.size() + 1) * sizeof(wchar_t)),
            NCRYPTBUFFER_PKCS_KEY_NAME, name.data()};
        NCryptBufferDesc parameters{NCRYPTBUFFER_VERSION, 1, &name_buffer};
        status = NCryptImportKey(state.key_provider, 0, NCRYPT_PKCS8_PRIVATE_KEY_BLOB,
            &parameters, &state.owned_key, exported.bytes.data(), byte_count, 0);
        if (status != ERROR_SUCCESS) {
            error_ = detail::error_code("Import temporary private key", status);
            return false;
        }
        CRYPT_KEY_PROV_INFO provider_info{};
        provider_info.pwszContainerName = name.data();
        provider_info.pwszProvName = const_cast<wchar_t*>(MS_KEY_STORAGE_PROVIDER);
        provider_info.dwKeySpec = 0; // CNG NCryptOpenKey legacy key spec.
        if (!CertSetCertificateContextProperty(state.certificate, CERT_KEY_CONTEXT_PROP_ID, 0, nullptr) ||
            !CertSetCertificateContextProperty(state.certificate, CERT_KEY_PROV_INFO_PROP_ID, 0, &provider_info)) {
            error_ = detail::error_code("Attach temporary private key", GetLastError());
            return false;
        }
        return true;
    }

    bool acquire(std::shared_ptr<detail::CredentialState> state,
                 SCHANNEL_CRED& config, ULONG direction) {
        TimeStamp expiry{};
        const SECURITY_STATUS status = AcquireCredentialsHandleA(nullptr,
            const_cast<char*>(UNISP_NAME_A), direction, nullptr, &config,
            nullptr, nullptr, &state->handle, &expiry);
        if (status != SEC_E_OK) {
            error_ = detail::error_code("Acquire TLS credentials", status);
            return false;
        }
        state_ = std::move(state);
        return true;
    }
};

class Stream {
public:
    explicit Stream(SOCKET socket, const std::atomic_bool* stop = nullptr)
        : socket_(socket), stop_(stop) {
        SecInvalidateHandle(&context_);
        if (socket_ == INVALID_SOCKET) {
            error_ = "Invalid socket";
            return;
        }
        u_long nonblocking = 1;
        if (ioctlsocket(socket_, FIONBIO, &nonblocking) == SOCKET_ERROR) {
            fail(detail::error_code("Set nonblocking socket", WSAGetLastError()));
            return;
        }
        // Small request/response records must not wait for a delayed ACK (Nagle).
        // Best effort: a failure only affects latency, never correctness.
        BOOL no_delay = TRUE;
        setsockopt(socket_, IPPROTO_TCP, TCP_NODELAY,
            reinterpret_cast<const char*>(&no_delay), sizeof(no_delay));
    }

    ~Stream() { close(); }
    Stream(const Stream&) = delete;
    Stream& operator=(const Stream&) = delete;
    Stream(Stream&&) = delete;
    Stream& operator=(Stream&&) = delete;

    bool connect(Credential& credential, const std::string& host,
                 const std::string& pinned_sha256, int timeout_ms = 10000) {
        begin_operation();
        std::array<BYTE, 32> pin{};
        if (!parse_pin(pinned_sha256, pin))
            return fail("Certificate pin must contain exactly 64 hexadecimal digits");
        if (host.empty() || host.find('\0') != std::string::npos)
            return fail("TLS host is empty or invalid");
        if (!prepare(credential, false)) return false;
        if (!handshake(false, host, deadline(timeout_ms))) return false;
        if (!verify_pin(pin)) return false;
        established_ = true;
        return true;
    }

    bool accept(Credential& credential, int timeout_ms = 10000) {
        begin_operation();
        if (!prepare(credential, true)) return false;
        if (!handshake(true, {}, deadline(timeout_ms))) return false;
        established_ = true;
        return true;
    }

    bool read_exact(void* destination, size_t size, int timeout_ms = 10000) {
        begin_operation();
        if (!require_established()) return false;
        if (size != 0 && !destination) return fail("Null read buffer");
        auto* out = static_cast<BYTE*>(destination);
        const auto until = deadline(timeout_ms);
        size_t copied = 0;
        while (copied != size) {
            if (!check_deadline(until)) return false;
            if (plain_offset_ == plaintext_.size() && !decrypt_next(until))
                return false;
            const size_t count = (std::min)(size - copied,
                plaintext_.size() - plain_offset_);
            std::memcpy(out + copied, plaintext_.data() + plain_offset_, count);
            copied += count;
            plain_offset_ += count;
        }
        return true;
    }

    bool write_all(const void* source, size_t size, int timeout_ms = 10000) {
        begin_operation();
        if (!require_established()) return false;
        if (size != 0 && !source) return fail("Null write buffer");
        const auto* input = static_cast<const BYTE*>(source);
        const auto until = deadline(timeout_ms);
        size_t sent = 0;
        while (sent != size) {
            if (!check_deadline(until)) return false;
            const size_t count = (std::min)(size - sent,
                static_cast<size_t>(sizes_.cbMaximumMessage));
            const size_t record_size = sizes_.cbHeader + count + sizes_.cbTrailer;
            if (record_.size() < record_size) record_.resize(record_size);
            std::memcpy(record_.data() + sizes_.cbHeader, input + sent, count);
            SecBuffer buffers[4]{};
            buffers[0] = {sizes_.cbHeader, SECBUFFER_STREAM_HEADER, record_.data()};
            buffers[1] = {static_cast<ULONG>(count), SECBUFFER_DATA,
                record_.data() + sizes_.cbHeader};
            buffers[2] = {sizes_.cbTrailer, SECBUFFER_STREAM_TRAILER,
                record_.data() + sizes_.cbHeader + count};
            buffers[3].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = EncryptMessage(&context_, 0, &message, 0);
            if (status != SEC_E_OK)
                return fail(detail::error_code("Encrypt TLS record", status));
            // Header, data and trailer are laid out back to back in record_, so the
            // whole record normally goes out in one send (avoids write-write-read stalls).
            const BYTE* base = record_.data();
            const size_t encrypted_size = static_cast<size_t>(buffers[0].cbBuffer) +
                buffers[1].cbBuffer + buffers[2].cbBuffer;
            if (buffers[0].pvBuffer == base &&
                buffers[1].pvBuffer == base + buffers[0].cbBuffer &&
                buffers[2].pvBuffer == base + buffers[0].cbBuffer + buffers[1].cbBuffer &&
                encrypted_size <= record_.size()) {
                if (!send_raw(base, encrypted_size, until)) return false;
            } else {
                for (size_t i = 0; i < 3; ++i) {
                    if (buffers[i].cbBuffer && !send_raw(
                            static_cast<const BYTE*>(buffers[i].pvBuffer),
                            buffers[i].cbBuffer, until)) return false;
                }
            }
            sent += count;
        }
        return true;
    }

    bool wait_readable(int timeout_ms) {
        begin_operation();
        if (!require_established()) return false;
        if (stopped()) return fail("TLS operation cancelled");
        if (plain_offset_ != plaintext_.size() || !encrypted_.empty()) return true;
        return wait_socket(false, deadline(timeout_ms), true);
    }

    bool valid() const noexcept { return socket_ != INVALID_SOCKET; }
    bool timed_out() const noexcept { return timed_out_; }
    const std::string& last_error() const noexcept { return error_; }

    // Immediate local close: no blocking TLS shutdown exchange in destructors.
    // The application protocol must confirm any transaction before calling close.
    void close() noexcept {
        established_ = false;
        if (socket_ != INVALID_SOCKET) {
            shutdown(socket_, SD_BOTH);
            closesocket(socket_);
            socket_ = INVALID_SOCKET;
        }
        if (SecIsValidHandle(&context_)) {
            DeleteSecurityContext(&context_);
            SecInvalidateHandle(&context_);
        }
        credential_.reset();
        if (!record_.empty()) SecureZeroMemory(record_.data(), record_.size());
        record_.clear();
        // DecryptMessage works in place, so this buffer may still hold plaintext.
        if (!encrypted_.empty()) SecureZeroMemory(encrypted_.data(), encrypted_.size());
        encrypted_.clear();
        if (!plaintext_.empty()) SecureZeroMemory(plaintext_.data(), plaintext_.size());
        plaintext_.clear();
        plain_offset_ = 0;
    }

private:
    using Clock = std::chrono::steady_clock;
    using Deadline = Clock::time_point;
    static constexpr size_t max_input = 256 * 1024;
    SOCKET socket_ = INVALID_SOCKET;
    const std::atomic_bool* stop_ = nullptr;
    CtxtHandle context_{};
    std::shared_ptr<detail::CredentialState> credential_;
    SecPkgContext_StreamSizes sizes_{};
    std::vector<BYTE> record_;
    std::vector<BYTE> encrypted_;
    std::vector<BYTE> plaintext_;
    size_t plain_offset_ = 0;
    bool established_ = false;
    bool timed_out_ = false;
    std::string error_;

    static Deadline deadline(int timeout_ms) {
        return Clock::now() + std::chrono::milliseconds((std::max)(0, timeout_ms));
    }
    bool stopped() const { return stop_ && stop_->load(std::memory_order_relaxed); }
    void begin_operation() {
        timed_out_ = false;
        if (valid()) error_.clear();
    }
    bool fail(std::string error) {
        error_ = std::move(error);
        close();
        return false;
    }
    bool check_deadline(Deadline until) {
        if (stopped()) return fail("TLS operation cancelled");
        if (Clock::now() >= until) {
            timed_out_ = true;
            return fail("TLS operation timed out");
        }
        return true;
    }
    bool require_established() {
        if (!valid()) return false;
        if (!established_) return fail("TLS handshake has not completed");
        return true;
    }
    bool prepare(Credential& credential, bool server) {
        if (!valid()) return false;
        if (SecIsValidHandle(&context_)) return fail("TLS handshake already started");
        if (!credential.state_ || credential.state_->server != server)
            return fail("TLS credentials are missing or have the wrong role");
        credential_ = credential.state_;
        return true;
    }

    bool wait_socket(bool writing, Deadline until, bool idle_allowed = false) {
        for (;;) {
            if (stopped()) return fail("TLS operation cancelled");
            if (!valid()) return false;
            const auto now = Clock::now();
            auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
                until - now).count();
            remaining = (std::clamp)(remaining, 0LL, 100000LL);
            timeval interval{0, static_cast<long>(remaining)};
            fd_set ready{}, exceptional{};
            FD_ZERO(&ready);
            FD_ZERO(&exceptional);
            FD_SET(socket_, &ready);
            FD_SET(socket_, &exceptional);
            const int result = select(0, writing ? nullptr : &ready,
                writing ? &ready : nullptr, &exceptional, &interval);
            if (result == SOCKET_ERROR)
                return fail(detail::error_code("Poll TLS socket", WSAGetLastError()));
            if (FD_ISSET(socket_, &exceptional)) return fail("TLS socket error");
            if (result > 0 && FD_ISSET(socket_, &ready)) return true;
            if (Clock::now() >= until) {
                timed_out_ = true;
                if (idle_allowed) return false;
                return fail("TLS operation timed out");
            }
        }
    }

    bool receive_more(Deadline until) {
        if (encrypted_.size() >= max_input) return fail("TLS input exceeds 256 KiB");
        std::array<BYTE, 16384> incoming{};
        for (;;) {
            if (!check_deadline(until)) return false;
            const int count = recv(socket_, reinterpret_cast<char*>(incoming.data()),
                static_cast<int>((std::min)(incoming.size(), max_input - encrypted_.size())), 0);
            if (count > 0) {
                encrypted_.insert(encrypted_.end(), incoming.begin(), incoming.begin() + count);
                return true;
            }
            if (count == 0) return fail("TLS peer closed the connection");
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
                return fail(detail::error_code("Receive TLS data", error));
            if (!wait_socket(false, until)) return false;
        }
    }

    bool send_raw(const BYTE* bytes, size_t size, Deadline until) {
        size_t offset = 0;
        while (offset < size) {
            if (!check_deadline(until)) return false;
            const int count = send(socket_, reinterpret_cast<const char*>(bytes + offset),
                static_cast<int>((std::min)(size - offset, size_t{16384})), 0);
            if (count > 0) { offset += count; continue; }
            if (count == 0) return fail("TLS peer closed during write");
            const int error = WSAGetLastError();
            if (error != WSAEWOULDBLOCK)
                return fail(detail::error_code("Send TLS data", error));
            if (!wait_socket(true, until)) return false;
        }
        return true;
    }

    bool retain_extra(const SecBuffer* buffers, size_t count) {
        size_t extra = 0;
        for (size_t i = 0; i < count; ++i)
            if (buffers[i].BufferType == SECBUFFER_EXTRA) extra = buffers[i].cbBuffer;
        if (extra > encrypted_.size()) return fail("Invalid TLS extra buffer");
        const size_t consumed = encrypted_.size() - extra;
        if (extra) std::memmove(encrypted_.data(), encrypted_.data() + consumed, extra);
        // The consumed record was decrypted in place; scrub it before it becomes spare capacity.
        if (consumed) SecureZeroMemory(encrypted_.data() + extra, consumed);
        encrypted_.resize(extra);
        return true;
    }

    bool handshake(bool server, const std::string& host, Deadline until) {
        bool first = true;
        for (;;) {
            if (!check_deadline(until)) return false;
            if ((server || !first) && encrypted_.empty() && !receive_more(until))
                return false;
            SecBuffer input[2]{};
            input[0] = {static_cast<ULONG>(encrypted_.size()), SECBUFFER_TOKEN,
                encrypted_.empty() ? nullptr : encrypted_.data()};
            input[1].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc in_desc{SECBUFFER_VERSION, 2, input};
            SecBuffer output{0, SECBUFFER_TOKEN, nullptr};
            SecBufferDesc out_desc{SECBUFFER_VERSION, 1, &output};
            ULONG attributes = 0;
            TimeStamp expiry{};
            SECURITY_STATUS status;
            CtxtHandle* existing = SecIsValidHandle(&context_) ? &context_ : nullptr;
            if (server) {
                constexpr ULONG flags = ASC_REQ_SEQUENCE_DETECT | ASC_REQ_REPLAY_DETECT |
                    ASC_REQ_CONFIDENTIALITY | ASC_REQ_EXTENDED_ERROR |
                    ASC_REQ_ALLOCATE_MEMORY | ASC_REQ_STREAM;
                status = AcceptSecurityContext(&credential_->handle, existing, &in_desc,
                    flags, SECURITY_NATIVE_DREP, &context_, &out_desc, &attributes, &expiry);
            } else {
                constexpr ULONG flags = ISC_REQ_SEQUENCE_DETECT | ISC_REQ_REPLAY_DETECT |
                    ISC_REQ_CONFIDENTIALITY | ISC_REQ_EXTENDED_ERROR | ISC_REQ_ALLOCATE_MEMORY |
                    ISC_REQ_STREAM | ISC_REQ_MANUAL_CRED_VALIDATION | ISC_REQ_USE_SUPPLIED_CREDS;
                status = InitializeSecurityContextA(&credential_->handle, existing,
                    const_cast<char*>(host.c_str()), flags, 0, SECURITY_NATIVE_DREP,
                    first ? nullptr : &in_desc, 0, &context_, &out_desc, &attributes, &expiry);
            }
            first = false;
            if (status == SEC_I_COMPLETE_NEEDED || status == SEC_I_COMPLETE_AND_CONTINUE) {
                const SECURITY_STATUS completed = CompleteAuthToken(&context_, &out_desc);
                if (completed != SEC_E_OK) status = completed;
                else status = status == SEC_I_COMPLETE_NEEDED ? SEC_E_OK : SEC_I_CONTINUE_NEEDED;
            }
            bool sent = true;
            if (output.pvBuffer) {
                if (output.cbBuffer > max_input) sent = fail("TLS output exceeds 256 KiB");
                else if (output.cbBuffer) sent = send_raw(
                    static_cast<const BYTE*>(output.pvBuffer), output.cbBuffer, until);
                FreeContextBuffer(output.pvBuffer);
            }
            if (!sent) return false;
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive_more(until)) return false;
                continue;
            }
            if (status != SEC_E_OK && status != SEC_I_CONTINUE_NEEDED)
                return fail(detail::error_code("TLS handshake", status));
            if (!retain_extra(input, 2)) return false;
            if (status == SEC_E_OK) {
                const ULONG required = server ? ASC_RET_CONFIDENTIALITY : ISC_RET_CONFIDENTIALITY;
                if ((attributes & required) == 0) return fail("TLS confidentiality was not negotiated");
                status = QueryContextAttributesA(&context_, SECPKG_ATTR_STREAM_SIZES, &sizes_);
                if (status != SEC_E_OK)
                    return fail(detail::error_code("Query TLS stream sizes", status));
                if (sizes_.cbMaximumMessage == 0 ||
                    static_cast<size_t>(sizes_.cbMaximumMessage) + sizes_.cbHeader + sizes_.cbTrailer > max_input)
                    return fail("Unsupported TLS record size");
                record_.reserve(static_cast<size_t>(sizes_.cbHeader) + sizes_.cbMaximumMessage + sizes_.cbTrailer);
                encrypted_.reserve((std::min)(max_input, size_t(16 * 1024)));
                plaintext_.reserve((std::min)(max_input, size_t(16 * 1024)));
                return true;
            }
        }
    }

    static bool parse_pin(const std::string& text, std::array<BYTE, 32>& pin) {
        if (text.size() != pin.size() * 2) return false;
        auto digit = [](char c) -> int {
            if (c >= '0' && c <= '9') return c - '0';
            if (c >= 'a' && c <= 'f') return c - 'a' + 10;
            if (c >= 'A' && c <= 'F') return c - 'A' + 10;
            return -1;
        };
        for (size_t i = 0; i < pin.size(); ++i) {
            const int high = digit(text[i * 2]), low = digit(text[i * 2 + 1]);
            if (high < 0 || low < 0) return false;
            pin[i] = static_cast<BYTE>((high << 4) | low);
        }
        return true;
    }

    bool verify_pin(const std::array<BYTE, 32>& pin) {
        PCCERT_CONTEXT remote = nullptr;
        const SECURITY_STATUS status = QueryContextAttributesA(
            &context_, SECPKG_ATTR_REMOTE_CERT_CONTEXT, &remote);
        if (status != SEC_E_OK || !remote)
            return fail(detail::error_code("Query TLS peer certificate", status));
        std::array<BYTE, 32> actual{};
        DWORD length = static_cast<DWORD>(actual.size());
        const bool date_valid = CertVerifyTimeValidity(nullptr, remote->pCertInfo) == 0;
        const BOOL hashed = CryptHashCertificate2(BCRYPT_SHA256_ALGORITHM, 0, nullptr,
            remote->pbCertEncoded, remote->cbCertEncoded, actual.data(), &length);
        const DWORD hash_error = GetLastError();
        CertFreeCertificateContext(remote);
        if (!date_valid) return fail("TLS peer certificate is expired or not yet valid");
        if (!hashed || length != actual.size())
            return fail(detail::error_code("Hash TLS peer certificate", hash_error));
        BYTE mismatch = 0;
        for (size_t i = 0; i < actual.size(); ++i) mismatch |= actual[i] ^ pin[i];
        if (mismatch) return fail("TLS peer certificate SHA-256 pin mismatch");
        return true;
    }

    bool decrypt_next(Deadline until) {
        if (!plaintext_.empty()) SecureZeroMemory(plaintext_.data(), plaintext_.size());
        plaintext_.clear();
        plain_offset_ = 0;
        for (;;) {
            if (!check_deadline(until)) return false;
            if (encrypted_.empty() && !receive_more(until)) return false;
            SecBuffer buffers[4]{};
            buffers[0] = {static_cast<ULONG>(encrypted_.size()), SECBUFFER_DATA, encrypted_.data()};
            for (size_t i = 1; i < 4; ++i) buffers[i].BufferType = SECBUFFER_EMPTY;
            SecBufferDesc message{SECBUFFER_VERSION, 4, buffers};
            const SECURITY_STATUS status = DecryptMessage(&context_, &message, 0, nullptr);
            if (status == SEC_E_INCOMPLETE_MESSAGE) {
                if (!receive_more(until)) return false;
                continue;
            }
            if (status == SEC_I_CONTEXT_EXPIRED) return fail("TLS peer sent close_notify");
            if (status == SEC_I_RENEGOTIATE) return fail("TLS renegotiation is not supported");
            if (status != SEC_E_OK)
                return fail(detail::error_code("Decrypt TLS record", status));
            for (const auto& buffer : buffers) {
                if (buffer.BufferType != SECBUFFER_DATA || buffer.cbBuffer == 0) continue;
                if (buffer.cbBuffer > max_input - plaintext_.size())
                    return fail("TLS plaintext exceeds 256 KiB");
                const auto* bytes = static_cast<const BYTE*>(buffer.pvBuffer);
                plaintext_.insert(plaintext_.end(), bytes, bytes + buffer.cbBuffer);
            }
            if (!retain_extra(buffers, 4)) return false;
            if (!plaintext_.empty()) return true;
        }
    }
};
} // namespace chh::tls
