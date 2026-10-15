#pragma once
#include "network/protocol.h"

namespace chh::server {
// Old authenticated names may be longer than newly editable profile names.
// The roster client applies valid_profile_name to this display field as well.
inline std::string_view roster_display_name(std::string_view name) {
    if (name.empty() || wire::valid_profile_name(name)) return name;

    size_t end = 0, characters = 0;
    while (end < name.size() && end < wire::profile_name_limit) {
        if ((static_cast<unsigned char>(name[end]) & 0xc0) != 0x80) {
            if (characters == wire::profile_name_characters) break;
            ++characters;
        }
        ++end;
    }
    // Do not retain part of a UTF-8 character at the byte limit.
    while (end && end < name.size() && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
    name = name.substr(0, end);
    // A shortened name can end on whitespace, and legacy names can contain
    // formatting that new profile names reject. Keep the longest valid prefix.
    while (!name.empty() && !wire::valid_profile_name(name)) {
        end = name.size() - 1;
        while (end && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
        name = name.substr(0, end);
    }
    return name.empty() ? std::string_view(u8"玩家") : name;
}
}
