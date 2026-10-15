#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <bcrypt.h>
#include <array>
#include <cstdint>
#include <initializer_list>
#include <limits>
#include <string>
#include <string_view>
#pragma comment(lib, "bcrypt.lib")

namespace chh::login_proof {
constexpr size_t key_limit = 64, response_limit = 68;

// LP2 binds the full nonce and listed primary-login context inside verified TLS.
// This unkeyed digest is NOT a signature, MAC, or standalone authorization.
// The server must authenticate the account before producing a response.
// Both endpoints must use this format; legacy responses are never accepted.
struct Context {
    std::string_view account, machine, version, name, expiration;
};

namespace detail {
constexpr char hex[] = "0123456789abcdef";
constexpr std::string_view domain = "CHH8/login-proof/v2";
constexpr std::string_view prefix = "LP2:";

inline bool lowercase_hex(std::string_view text, size_t length) {
    if (text.size() != length) return false;
    for (const char c : text)
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'))) return false;
    return true;
}

inline bool valid_machine(std::string_view text) {
    if (text.empty() || text.size() > 16) return false;
    uint64_t number = 0;
    constexpr uint64_t maximum = uint64_t((std::numeric_limits<int64_t>::max)());
    for (const char c : text) {
        unsigned digit = 0;
        if (c >= '0' && c <= '9') digit = unsigned(c - '0');
        else if (c >= 'a' && c <= 'f') digit = unsigned(c - 'a') + 10;
        else if (c >= 'A' && c <= 'F') digit = unsigned(c - 'A') + 10;
        else return false;
        if (number > (maximum - digit) / 16) return false;
        number = number * 16 + digit;
    }
    return true;
}

inline bool bounded_text(std::string_view text, size_t limit) {
    return !text.empty() && text.size() <= limit && text.find('\0') == std::string_view::npos;
}

inline std::string encode_hex(const std::array<uint8_t, 32>& bytes) {
    std::string text(bytes.size() * 2, '0');
    for (size_t i = 0; i < bytes.size(); ++i) {
        text[i * 2] = hex[bytes[i] >> 4];
        text[i * 2 + 1] = hex[bytes[i] & 15];
    }
    return text;
}

inline void append_field(std::string& transcript, std::string_view field) {
    const auto length = static_cast<uint32_t>(field.size());
    for (unsigned shift : {24u, 16u, 8u, 0u})
        transcript.push_back(static_cast<char>((length >> shift) & 255));
    transcript.append(field.data(), field.size());
}

inline bool digest(std::string_view transcript, std::array<uint8_t, 32>& out) {
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    BCRYPT_HASH_HANDLE hash = nullptr;
    const bool good = BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
        BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) >= 0 &&
        BCryptHashData(hash, reinterpret_cast<PUCHAR>(const_cast<char*>(transcript.data())),
            static_cast<ULONG>(transcript.size()), 0) >= 0 &&
        BCryptFinishHash(hash, out.data(), static_cast<ULONG>(out.size()), 0) >= 0;
    if (hash) BCryptDestroyHash(hash);
    if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
    return good;
}
} // namespace detail

inline bool make_key(std::string& key) {
    key.clear();
    std::array<uint8_t, 32> random{};
    if (BCryptGenRandom(nullptr, random.data(), static_cast<ULONG>(random.size()),
        BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0) return false;
    key = detail::encode_hex(random);
    return true;
}

inline bool valid_request(std::string_view key, std::string_view machine) {
    return detail::lowercase_hex(key, key_limit) && detail::valid_machine(machine);
}

inline bool make_response(std::string_view key, const Context& context, std::string& out) {
    out.clear();
    if (!valid_request(key, context.machine) || !detail::bounded_text(context.account, 64) ||
        !detail::bounded_text(context.version, 32) || !detail::bounded_text(context.name, 512) ||
        !detail::bounded_text(context.expiration, 19)) return false;
    // Fixed domain followed by uint32 big-endian byte lengths and exact bytes.
    // Length framing prevents adjacent fields from being reinterpreted.
    std::string transcript(detail::domain);
    for (const auto field : {key, context.account, context.machine, context.version, context.name, context.expiration})
        detail::append_field(transcript, field);
    std::array<uint8_t, 32> hash{};
    if (!detail::digest(transcript, hash)) return false;
    out = std::string(detail::prefix) + detail::encode_hex(hash);
    return true;
}

inline bool verify_response(std::string_view key, const Context& context, std::string_view proof) {
    if (proof.size() != response_limit || proof.substr(0, detail::prefix.size()) != detail::prefix ||
        !detail::lowercase_hex(proof.substr(detail::prefix.size()), 64)) return false;
    std::string expected;
    if (!make_response(key, context, expected)) return false;
    unsigned difference = 0;
    for (size_t i = 0; i < expected.size(); ++i)
        difference |= static_cast<unsigned char>(expected[i]) ^ static_cast<unsigned char>(proof[i]);
    return difference == 0;
}
} // namespace chh::login_proof
