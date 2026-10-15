#include "../roster_display.h"
#include <cstdlib>
#include <iostream>

namespace {
void require(bool condition, const char* label) {
    if (!condition) {
        std::cerr << "FAIL: " << label << '\n';
        std::exit(1);
    }
}
std::string repeat(std::string_view text, size_t count) {
    std::string result;
    for (size_t i = 0; i < count; ++i) result += text;
    return result;
}
void check(const std::string& original, std::string_view expected, const char* label) {
    const auto before = original;
    const auto display = chh::server::roster_display_name(original);
    require(display == expected, label);
    require(original == before, "stored name remains unchanged");
    require(display.size() <= chh::wire::profile_name_limit, "display byte limit");
    require(chh::wire::valid_text(display), "display has complete UTF-8 characters");
    require(original.empty() ? display.empty() : chh::wire::valid_profile_name(display),
        "display satisfies roster client validation");

    // Round-trip the identity/display fields through the real wire codec.
    chh::wire::Packet packet(chh::wire::Type::game_player_entry);
    const uint32_t linked = original.empty() ? 0u : 1u;
    const std::string account = linked ? "original-account" : "";
    const uint32_t hidden = linked;
    require(packet.add_number(linked) &&
        packet.add_string(display, chh::wire::profile_name_limit) &&
        packet.add_string(account, 64) && packet.add_number(hidden), "roster fields encode");
    chh::wire::Reader reader{packet};
    uint32_t decoded_linked = 2, decoded_hidden = 2;
    std::string decoded_name, decoded_account;
    require(reader.number(decoded_linked) &&
        reader.string(decoded_name, chh::wire::profile_name_limit, true) &&
        reader.string(decoded_account, 64, true) && reader.number(decoded_hidden) && reader.done(),
        "roster fields decode");
    require(decoded_linked == linked && decoded_name == display &&
        decoded_account == account && decoded_hidden == hidden, "identity flags and account preserved");
    require(!decoded_linked || chh::wire::valid_profile_name(decoded_name), "client accepts linked name");
}
}

int main() {
    check("", "", "empty unlinked name");
    check("Alice", "Alice", "short ASCII unchanged");
    check(u8"茶会玩家😀", u8"茶会玩家😀", "short UTF-8 unchanged");
    check(std::string(36, 'a'), std::string(36, 'a'), "36 ASCII characters unchanged");
    check(std::string(144, 'a'), std::string(36, 'a'), "144 ASCII bytes respect character limit");
    check(std::string(145, 'a'), std::string(36, 'a'), "145 ASCII bytes clipped");
    check(std::string(512, 'a'), std::string(36, 'a'), "maximum legacy name clipped");
    check(repeat(u8"茶", 36), repeat(u8"茶", 36), "36 three-byte characters unchanged");
    check(repeat(u8"茶", 49), repeat(u8"茶", 36), "49 three-byte characters clipped at boundary");
    check(repeat(u8"😀", 36), repeat(u8"😀", 36), "144-byte emoji name unchanged");
    check(repeat(u8"😀", 37), repeat(u8"😀", 36), "four-byte character boundary");
    check(repeat(u8"😀", 35) + u8"茶😀", repeat(u8"😀", 35) + u8"茶", "mixed three/four-byte boundary");
    check(std::string(35, 'a') + " x", std::string(35, 'a'), "shortening removes new trailing space");
    check(repeat(u8"茶", 35) + u8"　字", repeat(u8"茶", 35), "shortening removes full-width trailing space");
    check(u8"Alice\u200bBob", "Alice", "legacy format character keeps valid prefix");
    check(" leading", u8"玩家", "no valid prefix gets neutral display");
    check(" ", u8"玩家", "nonempty whitespace remains linked");
    check(u8"\u200b", u8"玩家", "invisible-only legacy name");
    std::cout << "PASS: 18 roster display cases and wire round-trips\n";
}
