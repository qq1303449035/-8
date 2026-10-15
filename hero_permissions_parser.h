#pragma once
#include "network/protocol.h"
#include <algorithm>
#include <utility>
#include <vector>

namespace chh::server {
// Separators of user8.special for the ban check, this parser and the ranking.
inline constexpr std::string_view permission_whitespace = " \t\r\n\f\v";

// Parse complete unsigned decimal tokens; neither 0 nor 99 grants other IDs.
inline bool parse_hero_permissions(std::string_view text, std::vector<uint32_t>& out) {
    out.clear();
    if (text.size() > wire::hero_permissions_text_limit) return false;
    std::vector<uint32_t> candidate;
    size_t begin = 0;
    while ((begin = text.find_first_not_of(permission_whitespace, begin)) != std::string_view::npos) {
        const size_t end = text.find_first_of(permission_whitespace, begin);
        const size_t limit = end == std::string_view::npos ? text.size() : end;
        if (limit - begin > 10 || candidate.size() >= wire::hero_permissions_id_limit) return false;
        uint32_t id = 0;
        for (size_t index = begin; index < limit; ++index) {
            const char digit = text[index];
            if (digit < '0' || digit > '9' || id > (UINT32_MAX - uint32_t(digit - '0')) / 10) return false;
            id = id * 10 + uint32_t(digit - '0');
        }
        candidate.push_back(id);
        begin = limit;
    }
    std::sort(candidate.begin(), candidate.end());
    candidate.erase(std::unique(candidate.begin(), candidate.end()), candidate.end());
    out = std::move(candidate);
    return true;
}
}
