// Types 179 (game_hero_delay_set), 180 (game_move_distance_set) and 181 (game_hero_keys_set) are retired: the
// hero-function run delay, walk-attack minimum move distance and hero keys now travel over the local LA<->DLL link
// (chh8/local_link.h), and the numbers are never reused. The launcher extracts the real packet gate, the dispatch
// branches that now sit where the 179-181 handlers were, and the matching forwarding blocks from appServer.cpp. Only
// TLS writes are replaced; these tests do not start a server or connect to a DB.
#include "../network/protocol.h"
#include "../sessions.h"
#include <initializer_list>
#include <iostream>
#include <stdexcept>
#include <type_traits>
#include <utility>
#include <vector>

namespace chh::tls {
struct Stream {
    bool writable = true;
    std::vector<wire::Packet> sent;
    bool write_all(const void* bytes, size_t size, int) {
        if (!writable || size != wire::packet_size) return false;
        wire::Packet packet;
        std::memcpy(packet.bytes.data(), bytes, size);
        sent.push_back(packet);
        return true;
    }
};
}
namespace {
using Sessions = chh::server::Sessions;
using Role = chh::server::ClientRole;
using Type = chh::wire::Type;
using Packet = chh::wire::Packet;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "retired_hero_types_senders.inc"

// ---------------------------------------------------------------------------
// Compile time: no retired name survives in the protocol or the session store.
// Each detector is checked against the live auto AFK relay first, so a broken
// detector cannot pass the negative checks by accident.
// ---------------------------------------------------------------------------
#define CHH_DETECT(trait, expression) \
    template<class T, class = void> struct trait : std::false_type {}; \
    template<class T> struct trait<T, std::void_t<decltype(expression)>> : std::true_type {};
#define CHH_DETECT_TYPE(trait, name) \
    template<class T, class = void> struct trait : std::false_type {}; \
    template<class T> struct trait<T, std::void_t<typename T::name>> : std::true_type {};
CHH_DETECT(has_auto_afk_type, T::game_auto_afk_set)
CHH_DETECT(has_hero_delay_type, T::game_hero_delay_set)
CHH_DETECT(has_move_distance_type, T::game_move_distance_set)
CHH_DETECT(has_hero_keys_type, T::game_hero_keys_set)
CHH_DETECT(has_set_auto_afk, &T::set_game_auto_afk)
CHH_DETECT(has_set_hero_delay, &T::set_game_hero_delay)
CHH_DETECT(has_set_move_distance, &T::set_game_move_distance)
CHH_DETECT(has_set_hero_keys, &T::set_game_hero_keys)
CHH_DETECT(has_auto_afk_snapshot, &T::auto_afk_command_snapshot)
CHH_DETECT(has_hero_delay_snapshot, &T::hero_delay_command_snapshot)
CHH_DETECT(has_move_distance_snapshot, &T::move_distance_command_snapshot)
CHH_DETECT(has_hero_keys_snapshot, &T::hero_keys_command_snapshot)
CHH_DETECT_TYPE(has_auto_afk_snapshot_type, AutoAfkSnapshot)
CHH_DETECT_TYPE(has_hero_delay_snapshot_type, HeroDelaySnapshot)
CHH_DETECT_TYPE(has_move_distance_snapshot_type, MoveDistanceSnapshot)
CHH_DETECT_TYPE(has_hero_keys_snapshot_type, HeroKeysSnapshot)
CHH_DETECT(has_auto_afk_enabled, std::declval<T&>().auto_afk_enabled)
CHH_DETECT(has_auto_afk_revision, std::declval<T&>().auto_afk_command_revision)
CHH_DETECT(has_hero_delay_ms, std::declval<T&>().hero_delay_ms)
CHH_DETECT(has_hero_delay_revision, std::declval<T&>().hero_delay_command_revision)
CHH_DETECT(has_move_distance_bits, std::declval<T&>().move_distance_bits)
CHH_DETECT(has_move_distance_revision, std::declval<T&>().move_distance_command_revision)
CHH_DETECT(has_hero_combo_key, std::declval<T&>().hero_combo_key)
CHH_DETECT(has_hero_toggle_key, std::declval<T&>().hero_toggle_key)
CHH_DETECT(has_hero_lane_key, std::declval<T&>().hero_lane_key)
CHH_DETECT(has_hero_keys_revision, std::declval<T&>().hero_keys_command_revision)
#undef CHH_DETECT_TYPE
#undef CHH_DETECT
static_assert(has_auto_afk_type<Type>::value && has_set_auto_afk<Sessions>::value &&
    has_auto_afk_snapshot<Sessions>::value && has_auto_afk_snapshot_type<Sessions>::value &&
    has_auto_afk_enabled<Sessions::Game>::value && has_auto_afk_revision<Sessions::Game>::value,
    "the detectors must see the live auto AFK relay");
static_assert(!has_hero_delay_type<Type>::value && !has_move_distance_type<Type>::value &&
    !has_hero_keys_type<Type>::value, "types 179-181 are gone from chh::wire::Type");
static_assert(!has_set_hero_delay<Sessions>::value && !has_set_move_distance<Sessions>::value &&
    !has_set_hero_keys<Sessions>::value, "Sessions has no setters for the retired settings");
static_assert(!has_hero_delay_snapshot<Sessions>::value && !has_move_distance_snapshot<Sessions>::value &&
    !has_hero_keys_snapshot<Sessions>::value && !has_hero_delay_snapshot_type<Sessions>::value &&
    !has_move_distance_snapshot_type<Sessions>::value && !has_hero_keys_snapshot_type<Sessions>::value,
    "Sessions has no snapshots for the retired settings");
static_assert(!has_hero_delay_ms<Sessions::Game>::value && !has_hero_delay_revision<Sessions::Game>::value &&
    !has_move_distance_bits<Sessions::Game>::value && !has_move_distance_revision<Sessions::Game>::value &&
    !has_hero_combo_key<Sessions::Game>::value && !has_hero_toggle_key<Sessions::Game>::value &&
    !has_hero_lane_key<Sessions::Game>::value && !has_hero_keys_revision<Sessions::Game>::value,
    "the paired game state keeps nothing for the retired settings");

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    unsigned dispatched = 0;  // frames that passed the packet gate
    unsigned unhandled = 0;   // frames that passed the gate but matched none of the extracted branches
    uint64_t sent_auto_afk_command_revision = UINT64_MAX;
    uint64_t sent_skin_block_command_revision = UINT64_MAX;
    uint64_t sent_attack_range_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "retired_hero_types_gate.inc"
            ++dispatched;
#include "retired_hero_types_handler.inc"
            } else {
                // Stand-in for the real final branch (media and avatar dispatch), which answers every type it does
                // not own with a protocol error.
                ++unhandled;
                send_error(stream, chh::wire::Error::protocol); break;
            }
        }
    }
    void poll() {
        do {
#include "retired_hero_types_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet make(uint32_t type, const std::vector<uint32_t>& values) {
    Packet packet(static_cast<Type>(type));
    for (const auto value : values) require(packet.add_number(value), "fixture encodes a uint32 payload");
    return packet;
}
Packet afk(uint32_t enabled) { return make(uint32_t(Type::game_auto_afk_set), {enabled}); }
void frame(const Packet& packet, Type type, uint32_t value) {
    require(packet.valid() && packet.type() == type && packet.length() == 4, "valid four-byte response frame");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX;
    require(reader.number(decoded) && reader.done() && decoded == value, "response contains exactly expected value");
}
void rejected_by_gate(const Worker& worker) {
    require(worker.stream.sent.size() == 1, "a retired type produces exactly one protocol error");
    frame(worker.stream.sent[0], Type::error, uint32_t(chh::wire::Error::protocol));
    require(worker.dispatched == 0 && worker.unhandled == 0,
        "the packet gate rejects a retired type before any dispatch branch sees it");
}

struct Retired {
    uint32_t type;
    std::vector<uint32_t> old_payload;  // what the removed relay accepted
};
// 179: 200 ms. 180: 0.5 m as float32 bits. 181: space, mouse side button 1, mouse side button 2.
const std::vector<Retired>& retired() {
    static const std::vector<Retired> types{{179, {200}}, {180, {0x3F000000u}}, {181, {0x20, 0x05, 0x06}}};
    return types;
}
// The old payload plus every other short payload length, so no encoding of a retired type is accepted.
std::vector<Packet> variants(const Retired& entry) {
    std::vector<Packet> packets{make(entry.type, entry.old_payload)};
    for (size_t count = 0; count <= 4; ++count) {
        if (count == entry.old_payload.size()) continue;
        packets.push_back(make(entry.type, std::vector<uint32_t>(count, 0x20)));
    }
    return packets;
}

void packet_boundary() {
    static_assert(uint32_t(Type::game_auto_afk_set) == 178);
    require(Packet(Type::game_auto_afk_set).valid() && afk(0).valid() && afk(1).valid(),
        "type 178 (auto AFK) is still the highest valid packet type");
    for (const auto& entry : retired()) {
        for (const auto& packet : variants(entry)) {
            require(uint32_t(packet.type()) == entry.type && !packet.valid(), "types 179-181 are invalid packets");
            chh::wire::Reader reader{packet};
            uint32_t value = 0;
            require(!reader.number(value), "no payload can be read from a retired type");
        }
        // The exact frame the old LA sent: big-endian length 4 or 12, then the type, then the values.
        Packet raw;
        const uint8_t header[8]{0, 0, 0, uint8_t(entry.old_payload.size() * 4), 0, 0, 0, uint8_t(entry.type)};
        std::memcpy(raw.bytes.data(), header, sizeof header);
        require(raw.type() == static_cast<Type>(entry.type) && !raw.valid(), "an old-format frame is invalid");
    }
    require(!Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(), "other unknown types stay invalid");
}

void primary_rejects_retired_types() {
    for (const auto& entry : retired()) {
        for (const auto& packet : variants(entry)) {
            Sessions sessions;
            const auto primary = enter(sessions, "owner", 10, Role::primary);
            const auto own_game = enter(sessions, "owner", 10, Role::secondary);
            Worker ui(sessions, primary);
            Worker game(sessions, own_game);
            // A valid auto AFK frame right behind the retired one must not be processed: the loop has ended.
            ui.receive({packet, afk(1)});
            rejected_by_gate(ui);
            require(!sessions.auto_afk_command_snapshot(own_game).initialized &&
                !sessions.skin_block_command_snapshot(own_game).initialized &&
                !sessions.attack_range_command_snapshot(own_game).initialized,
                "a retired type caches nothing and ends the primary's dispatch loop");
            game.poll(); game.poll();
            require(game.stream.sent.empty(), "nothing is forwarded to the paired game");
        }
    }
}

void retired_types_leave_existing_state_alone() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, own_game);
    ui.receive({afk(1)});
    require(ui.stream.sent.empty() && ui.dispatched == 1 && ui.unhandled == 0, "type 178 passes the gate and is handled");
    const auto before = sessions.auto_afk_command_snapshot(own_game);
    game.poll();
    require(game.stream.sent.size() == 1, "auto AFK is forwarded once");
    const uint8_t expected[12]{0, 0, 0, 4, 0, 0, 0, 178, 0, 0, 0, 1};
    require(std::memcmp(game.stream.sent.back().bytes.data(), expected, sizeof expected) == 0,
        "the forwarded type 178 frame is unchanged");
    for (const auto& entry : retired()) {
        Worker attacker(sessions, primary);
        attacker.receive({make(entry.type, entry.old_payload)});
        rejected_by_gate(attacker);
        const auto after = sessions.auto_afk_command_snapshot(own_game);
        require(after.revision == before.revision && after.enabled == 1, "a retired type does not touch other settings");
        game.poll();
        require(game.stream.sent.size() == 1, "a retired type forwards nothing");
    }
    ui.receive({afk(0)});
    game.poll();
    require(game.stream.sent.size() == 2, "type 178 keeps working after retired frames");
    frame(game.stream.sent.back(), Type::game_auto_afk_set, 0);
}

void game_endpoint_rejects_retired_types() {
    for (const auto& entry : retired()) {
        Sessions sessions;
        enter(sessions, "owner", 10, Role::primary);
        Worker endpoint(sessions, enter(sessions, "owner", 10, Role::secondary));
        endpoint.receive({make(entry.type, entry.old_payload)});
        rejected_by_gate(endpoint);
    }
}
}
int main() {
    try {
        packet_boundary();
        primary_rejects_retired_types();
        retired_types_leave_existing_state_alone();
        game_endpoint_rejects_retired_types();
        std::cout << "PASS: " << checks << " retired hero types (179-181) server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
