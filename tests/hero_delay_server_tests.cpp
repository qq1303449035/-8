// The launcher extracts the real dispatch and forwarding branches. Only TLS
// writes are replaced; these tests do not start a server or connect to a DB.
#include "../network/protocol.h"
#include "../sessions.h"
#include <cmath>
#include <iostream>
#include <limits>
#include <stdexcept>
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
constexpr Type hero_type = Type::game_hero_delay_set;
constexpr Type distance_type = Type::game_move_distance_set;
constexpr Type keys_type = Type::game_hero_keys_set;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "hero_delay_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    uint64_t sent_move_distance_command_revision = UINT64_MAX;
    uint64_t sent_hero_keys_command_revision = UINT64_MAX;
    uint64_t sent_hero_delay_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "hero_delay_handler.inc"
        }
    }
    void poll() {
        do {
#include "hero_delay_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t value, Type type = hero_type) {
    Packet packet(type);
    require(packet.add_number(value), "fixture encodes one uint32 payload");
    return packet;
}
void frame(const Packet& packet, Type type, uint32_t value) {
    require(packet.valid() && packet.type() == type && packet.length() == 4, "valid four-byte response frame");
    chh::wire::Reader reader{packet};
    uint32_t decoded = UINT32_MAX;
    require(reader.number(decoded) && reader.done() && decoded == value, "response contains exactly expected value");
}
void error(const Worker& worker) {
    require(worker.stream.sent.size() == 1, "invalid request produces one protocol error");
    frame(worker.stream.sent[0], Type::error, uint32_t(chh::wire::Error::protocol));
}

// ---------------------------------------------------------------------------
// Type 179: hero-function run delay (uint32 milliseconds, 100..300).
// ---------------------------------------------------------------------------
constexpr uint32_t hero_default = chh::wire::game_hero_delay_default_ms;
Sessions::HeroDelaySnapshot hero_snapshot(Sessions& sessions, const Sessions::Binding& binding) {
    return sessions.hero_delay_command_snapshot(binding);
}
bool set_hero(Sessions& sessions, const Sessions::Binding& binding, uint32_t delay_ms) {
    return sessions.set_game_hero_delay(binding, delay_ms);
}
void hero_constants_and_wire_format() {
    static_assert(uint32_t(Type::game_hero_delay_set) == 179);
    static_assert(chh::wire::game_hero_delay_min_ms == 100 && chh::wire::game_hero_delay_max_ms == 300 &&
        chh::wire::game_hero_delay_default_ms == 100);
    require(chh::wire::valid_game_hero_delay(100) && chh::wire::valid_game_hero_delay(200) &&
        chh::wire::valid_game_hero_delay(300), "range bounds are inclusive");
    require(!chh::wire::valid_game_hero_delay(0) && !chh::wire::valid_game_hero_delay(99) &&
        !chh::wire::valid_game_hero_delay(301) && !chh::wire::valid_game_hero_delay(UINT32_MAX),
        "values outside 100..300 are invalid");
    require(Packet(Type::game_hero_delay_set).valid() && Packet(Type::game_auto_afk_set).valid() &&
        Packet(Type::game_move_distance_set).valid() && Packet(Type::game_hero_keys_set).valid() &&
        !Packet(static_cast<Type>(182)).valid() &&
        !Packet(static_cast<Type>(0)).valid() && !Packet(static_cast<Type>(UINT32_MAX)).valid(),
        "type 181 is the highest valid packet type and unknown types are rejected");

    // Forwarded frame: length=4, type 179, one big-endian uint32.
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({request(300)});
    game.poll();
    require(game.stream.sent.size() == 1, "set value forwarded");
    const auto& bytes = game.stream.sent.back().bytes;
    const uint8_t expected[12]{0, 0, 0, 4, 0, 0, 0, 179, 0, 0, 0x01, 0x2C};
    require(std::memcmp(bytes.data(), expected, sizeof expected) == 0,
        "frame header and payload are big-endian with exactly 4 payload bytes");
    bool trailing_zero = true;
    for (size_t i = sizeof expected; i < bytes.size(); ++i) trailing_zero = trailing_zero && bytes[i] == 0;
    require(trailing_zero, "no bytes beyond the 4-byte payload");
}
void hero_routing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto before = hero_snapshot(sessions, own_game);
    require(!before.initialized && before.revision == 0 && before.delay_ms == hero_default, "hero delay starts at its uninitialized default");
    game.poll();
    require(game.stream.sent.empty(), "no unsolicited command before first initialization");
    ui.receive({request(hero_default)});
    const auto initial = hero_snapshot(sessions, own_game);
    require(initial.initialized && initial.revision != 0 && initial.delay_ms == hero_default, "first default value initializes command");
    require(!hero_snapshot(sessions, primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first default value");
    frame(game.stream.sent.back(), hero_type, hero_default);
    ui.receive({request(hero_default)});
    game.poll();
    require(game.stream.sent.size() == 1 && hero_snapshot(sessions, own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(250)});
    const auto changed = hero_snapshot(sessions, own_game);
    require(changed.delay_ms == 250 && changed.revision > initial.revision, "changing delay advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_hero_delay_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), hero_type, 250);
    ui.receive({request(300)});
    game.poll();
    require(game.stream.sent.size() == 3, "maximum value accepted and forwarded");
    frame(game.stream.sent.back(), hero_type, 300);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate hero delay commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    const auto latest = hero_snapshot(sessions, own_game);
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(hero_snapshot(sessions, own_game).revision == latest.revision &&
        hero_snapshot(sessions, own_game).delay_ms == 300, "status changes preserve hero delay setting");

    sessions.leave(own_game);
    ui.receive({request(150), request(220)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), hero_type, 220);
    require(!hero_snapshot(sessions, own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    // Reconnect with no change in between still re-sends the cached value.
    sessions.leave(replacement);
    const auto second_replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker resent(sessions, second_replacement);
    resent.poll(); resent.poll();
    require(resent.stream.sent.size() == 1, "unchanged cached value re-sent to reconnected game once");
    frame(resent.stream.sent.back(), hero_type, 220);

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({request(hero_default)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({request(hero_default)});
    resent.poll();
    require(resent.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(resent.stream.sent.back(), hero_type, hero_default);
    sessions.leave(primary); sessions.leave(second_replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!hero_snapshot(sessions, fresh).initialized &&
        hero_snapshot(sessions, fresh).delay_ms == hero_default, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(set_hero(sessions, primary, 200), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!hero_snapshot(sessions, fresh).initialized &&
        !set_hero(sessions, primary, 150), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!hero_snapshot(sessions, fresh_game).initialized, "new account session inherits no revoked setting");
}
void hero_independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_auto_afk(primary, 1), "initialize independent auto AFK");
    const auto afk = sessions.auto_afk_command_snapshot(secondary);
    require(!hero_snapshot(sessions, secondary).initialized, "auto AFK does not initialize hero delay");
    game.poll();
    require(game.stream.sent.empty(), "auto AFK changes do not forward a hero delay command");
    ui.receive({request(180)});
    const auto delay_a = hero_snapshot(sessions, secondary);
    require(delay_a.initialized && delay_a.delay_ms == 180, "hero delay set while auto AFK is on");
    require(sessions.auto_afk_command_snapshot(secondary).enabled == 1 &&
        sessions.auto_afk_command_snapshot(secondary).revision == afk.revision,
        "hero delay does not change auto AFK setting or revision");
    game.poll();
    frame(game.stream.sent.back(), hero_type, 180);
    ui.receive({request(120)});
    const auto delay_b = hero_snapshot(sessions, secondary);
    require(delay_b.delay_ms == 120 && delay_b.revision > delay_a.revision, "hero delay changes independently");
    require(sessions.set_game_auto_afk(primary, 0) && sessions.set_game_attack_range(primary, 1),
        "other controls can be changed separately");
    require(hero_snapshot(sessions, secondary).revision == delay_b.revision &&
        hero_snapshot(sessions, secondary).delay_ms == 120,
        "other controls do not change hero delay setting or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only hero delay revisions control hero delay packet forwarding");
    frame(game.stream.sent.back(), hero_type, 120);
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    frame(replacement.stream.sent.back(), hero_type, 120);
    require(sessions.auto_afk_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent hero delay and auto AFK settings");
}
void hero_queued_before_game() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({request(275)});
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.poll(); game.poll();
    require(game.stream.sent.size() == 1, "pre-connection value reaches the first game exactly once");
    frame(game.stream.sent.back(), hero_type, 275);
    ui.receive({request(hero_default)});
    game.poll();
    require(game.stream.sent.size() == 2, "change after connection reaches the paired game");
    frame(game.stream.sent.back(), hero_type, hero_default);
}
void hero_malformed_and_roles() {
    std::vector<Packet> invalid{request(301), request(1000), request(UINT32_MAX), request(0), request(99)};
    for (size_t length = 0; length <= 8; ++length) {
        if (length == 4) continue;
        Packet packet(hero_type);
        // Leading bytes encode an in-range value so only the length is wrong.
        const uint8_t bytes[8]{0, 0, 0, 150, 0, 0, 0, 150};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(200)});
        const auto revision = hero_snapshot(sessions, game).revision;
        ui.receive({packet, request(300)});
        error(ui);
        const auto command = hero_snapshot(sessions, game);
        require(command.delay_ms == 200 && command.revision == revision,
            "malformed request keeps setting and exits dispatch loop");
    }
    // An invalid first value must not initialize the command either.
    for (const uint32_t bad : {301u, UINT32_MAX, 0u, 99u}) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        Worker endpoint(sessions, game);
        ui.receive({request(bad)});
        error(ui);
        require(!hero_snapshot(sessions, game).initialized &&
            hero_snapshot(sessions, game).delay_ms == hero_default, "rejected first value leaves default uninitialized");
        endpoint.poll();
        require(endpoint.stream.sent.empty(), "rejected value is never forwarded");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(200)});
    error(endpoint);
    require(!hero_snapshot(sessions, game).initialized, "secondary cannot initialize controls");
    require(!set_hero(sessions, game, 200), "session API rejects game endpoint writes");
    require(!set_hero(sessions, {}, 200) && !set_hero(sessions, primary, UINT32_MAX) &&
        !set_hero(sessions, primary, 301), "session API also rejects empty binding and out-of-range values");
    require(!hero_snapshot(sessions, game).initialized, "rejected API calls do not initialize");
    require(set_hero(sessions, primary, hero_default), "queue initial default for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "default initialization survives game connection reset");
    frame(late_game.stream.sent.back(), hero_type, hero_default);
}

// ---------------------------------------------------------------------------
// Type 180: walk-attack minimum move distance. The payload is the IEEE-754
// float32 bit pattern of a distance in metres (0.0..1.0) as a big-endian uint32.
// ---------------------------------------------------------------------------
uint32_t bits(float metres) { return chh::wire::game_move_distance_to_bits(metres); }
Packet distance(float metres) { return request(bits(metres), distance_type); }
Packet distance_raw(uint32_t raw_bits) { return request(raw_bits, distance_type); }
Sessions::MoveDistanceSnapshot distance_snapshot(Sessions& sessions, const Sessions::Binding& binding) {
    return sessions.move_distance_command_snapshot(binding);
}
bool set_distance(Sessions& sessions, const Sessions::Binding& binding, uint32_t raw_bits) {
    return sessions.set_game_move_distance(binding, raw_bits);
}
// Out-of-range, non-finite and NaN payloads. UINT32_MAX is a (negative) NaN.
std::vector<uint32_t> invalid_distance_bits() {
    const float inf = std::numeric_limits<float>::infinity();
    return {bits(-0.1f), bits(1.1f), bits(2.0f), bits(-1.0f), bits(inf), bits(-inf),
        bits(std::numeric_limits<float>::quiet_NaN()), UINT32_MAX, 0x7F800000u, 0xFF800000u,
        0x7FC00000u, 0x7F800001u};
}
void distance_constants_and_wire_format() {
    static_assert(uint32_t(Type::game_move_distance_set) == 180);
    static_assert(chh::wire::game_move_distance_min == 0.0f && chh::wire::game_move_distance_max == 1.0f &&
        chh::wire::game_move_distance_default == 0.0f && chh::wire::game_move_distance_step == 0.1f);
    require(Packet(distance_type).valid() && !Packet(static_cast<Type>(182)).valid(),
        "type 180 is valid and type 182 is invalid");
    require(bits(0.0f) == 0x00000000u && bits(0.1f) == 0x3DCCCCCDu && bits(0.5f) == 0x3F000000u &&
        bits(1.0f) == 0x3F800000u && bits(chh::wire::game_move_distance_default) == 0u,
        "distance encodes as its IEEE-754 float32 bit pattern");
    require(chh::wire::game_move_distance_from_bits(0x3F000000u) == 0.5f &&
        chh::wire::game_move_distance_from_bits(bits(0.1f)) == 0.1f &&
        chh::wire::game_move_distance_from_bits(0x3F800000u) == 1.0f, "bit pattern decodes back to the same float");
    for (const float ok : {0.0f, 0.1f, 0.5f, 1.0f})
        require(chh::wire::valid_game_move_distance(ok) && chh::wire::valid_game_move_distance_bits(bits(ok)),
            "0.0, 0.1, 0.5 and 1.0 m are valid distances");
    const float inf = std::numeric_limits<float>::infinity();
    for (const float bad : {-0.1f, 1.1f, 2.0f, inf, -inf, std::numeric_limits<float>::quiet_NaN()})
        require(!chh::wire::valid_game_move_distance(bad), "out-of-range and non-finite distances are invalid");
    for (const uint32_t bad : invalid_distance_bits())
        require(!chh::wire::valid_game_move_distance_bits(bad), "invalid bit patterns are rejected");

    // Forwarded frame: length=4, type 180, float32 bits of 0.5 m = 0x3F000000.
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({distance(0.5f)});
    game.poll();
    require(game.stream.sent.size() == 1, "move distance forwarded");
    const auto& bytes = game.stream.sent.back().bytes;
    const uint8_t expected[12]{0, 0, 0, 4, 0, 0, 0, 180, 0x3F, 0x00, 0x00, 0x00};
    require(std::memcmp(bytes.data(), expected, sizeof expected) == 0,
        "move distance frame is big-endian float32 bits with exactly 4 payload bytes");
    bool trailing_zero = true;
    for (size_t i = sizeof expected; i < bytes.size(); ++i) trailing_zero = trailing_zero && bytes[i] == 0;
    require(trailing_zero, "no bytes beyond the 4-byte move distance payload");
}
void distance_routing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto before = distance_snapshot(sessions, own_game);
    require(!before.initialized && before.revision == 0 && before.distance_bits == bits(0.0f),
        "move distance starts at uninitialized 0.0 m");
    game.poll(); game.poll();
    require(game.stream.sent.empty(), "default 0.0 m is not sent before the first set");

    ui.receive({distance(0.0f)});
    const auto initial = distance_snapshot(sessions, own_game);
    require(initial.initialized && initial.revision != 0 && initial.distance_bits == bits(0.0f),
        "first set of 0.0 m initializes the command");
    require(!distance_snapshot(sessions, primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "first set of 0.0 m is forwarded");
    frame(game.stream.sent.back(), distance_type, bits(0.0f));
    ui.receive({distance(0.0f), distance(0.0f)});
    game.poll();
    require(game.stream.sent.size() == 1 && distance_snapshot(sessions, own_game).revision == initial.revision,
        "identical bits are not resent");

    ui.receive({distance(0.1f)});
    const auto changed = distance_snapshot(sessions, own_game);
    require(changed.distance_bits == bits(0.1f) && changed.revision > initial.revision, "changed bits advance revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_move_distance_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll(); game.poll();
    require(game.stream.sent.size() == 2, "changed bits forwarded exactly once");
    frame(game.stream.sent.back(), distance_type, bits(0.1f));
    ui.receive({distance(0.5f)});
    game.poll();
    require(game.stream.sent.size() == 3, "0.5 m accepted and forwarded");
    frame(game.stream.sent.back(), distance_type, 0x3F000000u);
    ui.receive({distance(1.0f)});
    game.poll();
    require(game.stream.sent.size() == 4, "maximum 1.0 m accepted and forwarded");
    frame(game.stream.sent.back(), distance_type, bits(1.0f));
    ui.receive({distance(1.0f)});
    game.poll();
    require(game.stream.sent.size() == 4, "repeated maximum is not resent");
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate move distance commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    const auto latest = distance_snapshot(sessions, own_game);
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(distance_snapshot(sessions, own_game).revision == latest.revision &&
        distance_snapshot(sessions, own_game).distance_bits == bits(1.0f), "status changes preserve move distance setting");

    sessions.leave(own_game);
    ui.receive({distance(0.3f), distance(0.7f)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only the latest distance once");
    frame(reconnected.stream.sent.back(), distance_type, bits(0.7f));
    require(!distance_snapshot(sessions, own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    // Reconnect with no change in between still re-sends the cached bits.
    sessions.leave(replacement);
    const auto second_replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker resent(sessions, second_replacement);
    resent.poll(); resent.poll();
    require(resent.stream.sent.size() == 1, "unchanged cached distance re-sent to reconnected game once");
    frame(resent.stream.sent.back(), distance_type, bits(0.7f));

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({distance(0.0f)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({distance(0.0f)});
    resent.poll();
    require(resent.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(resent.stream.sent.back(), distance_type, bits(0.0f));
    sessions.leave(primary); sessions.leave(second_replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!distance_snapshot(sessions, fresh).initialized &&
        distance_snapshot(sessions, fresh).distance_bits == bits(0.0f), "both endpoints leaving discards cached distance");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(set_distance(sessions, primary, bits(0.5f)), "fixture queues distance before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!distance_snapshot(sessions, fresh).initialized &&
        !set_distance(sessions, primary, bits(0.1f)), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!distance_snapshot(sessions, fresh_game).initialized, "new account session inherits no revoked distance");
}
void distance_independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_auto_afk(primary, 1), "initialize independent auto AFK");
    const auto afk = sessions.auto_afk_command_snapshot(secondary);
    require(!distance_snapshot(sessions, secondary).initialized, "auto AFK does not initialize move distance");
    game.poll();
    require(game.stream.sent.empty(), "auto AFK changes do not forward a move distance command");
    ui.receive({distance(0.5f)});
    const auto distance_a = distance_snapshot(sessions, secondary);
    require(distance_a.initialized && distance_a.distance_bits == bits(0.5f), "move distance set while auto AFK is on");
    require(sessions.auto_afk_command_snapshot(secondary).enabled == 1 &&
        sessions.auto_afk_command_snapshot(secondary).revision == afk.revision,
        "move distance does not change auto AFK setting or revision");
    game.poll();
    frame(game.stream.sent.back(), distance_type, bits(0.5f));
    ui.receive({distance(0.2f)});
    const auto distance_b = distance_snapshot(sessions, secondary);
    require(distance_b.distance_bits == bits(0.2f) && distance_b.revision > distance_a.revision,
        "move distance changes independently");
    require(sessions.set_game_auto_afk(primary, 0) && sessions.set_game_attack_range(primary, 1),
        "other controls can be changed separately");
    require(distance_snapshot(sessions, secondary).revision == distance_b.revision &&
        distance_snapshot(sessions, secondary).distance_bits == bits(0.2f),
        "other controls do not change move distance setting or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only move distance revisions control move distance forwarding");
    frame(game.stream.sent.back(), distance_type, bits(0.2f));
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    frame(replacement.stream.sent.back(), distance_type, bits(0.2f));
    require(sessions.auto_afk_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent move distance and auto AFK settings");
}
void distance_queued_before_game() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({distance(0.5f)});
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.poll(); game.poll();
    require(game.stream.sent.size() == 1, "pre-connection distance reaches the first game exactly once");
    frame(game.stream.sent.back(), distance_type, bits(0.5f));
    ui.receive({distance(0.0f)});
    game.poll();
    require(game.stream.sent.size() == 2, "change back to 0.0 m after connection reaches the paired game");
    frame(game.stream.sent.back(), distance_type, bits(0.0f));
}
void distance_malformed_and_roles() {
    std::vector<Packet> invalid;
    for (const uint32_t bad : invalid_distance_bits()) invalid.push_back(distance_raw(bad));
    for (size_t length = 0; length <= 8; ++length) {
        if (length == 4) continue;
        Packet packet(distance_type);
        // Leading bytes encode a valid 0.5 m pattern so only the length is wrong.
        const uint8_t bytes[8]{0x3F, 0, 0, 0, 0x3F, 0, 0, 0};
        require(packet.add_bytes(bytes, length), "fixture creates malformed move distance payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        Worker endpoint(sessions, game);
        ui.receive({distance(0.5f)});
        endpoint.poll();
        const auto revision = distance_snapshot(sessions, game).revision;
        ui.receive({packet, distance(1.0f)});
        error(ui);
        const auto command = distance_snapshot(sessions, game);
        require(command.distance_bits == bits(0.5f) && command.revision == revision,
            "invalid move distance is a protocol error that keeps the cache and exits dispatch loop");
        endpoint.poll();
        require(endpoint.stream.sent.size() == 1, "rejected move distance forwards nothing new");
        frame(endpoint.stream.sent.back(), distance_type, bits(0.5f));
    }
    // An invalid first value must not initialize the command either.
    for (const uint32_t bad : invalid_distance_bits()) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        Worker endpoint(sessions, game);
        ui.receive({distance_raw(bad)});
        error(ui);
        require(!distance_snapshot(sessions, game).initialized &&
            distance_snapshot(sessions, game).distance_bits == bits(0.0f),
            "rejected first distance leaves default uninitialized");
        require(!set_distance(sessions, primary, bad), "session API rejects invalid bit pattern");
        require(!distance_snapshot(sessions, game).initialized, "rejected API call does not initialize");
        endpoint.poll();
        require(endpoint.stream.sent.empty(), "rejected distance is never forwarded");
    }
    // Reverse direction: the game endpoint may not set the distance.
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({distance(0.5f)});
    error(endpoint);
    require(!distance_snapshot(sessions, game).initialized, "secondary cannot initialize move distance");
    require(!set_distance(sessions, game, bits(0.5f)), "session API rejects game endpoint writes");
    require(!set_distance(sessions, {}, bits(0.5f)), "session API rejects empty binding");
    require(set_distance(sessions, primary, bits(0.3f)), "primary initializes move distance");
    const auto queued = distance_snapshot(sessions, game);
    Worker reverse(sessions, game);
    reverse.receive({distance(1.0f), distance(0.0f)});
    error(reverse);
    require(distance_snapshot(sessions, game).distance_bits == bits(0.3f) &&
        distance_snapshot(sessions, game).revision == queued.revision,
        "secondary change attempt keeps primary distance and revision");
    require(set_distance(sessions, primary, bits(0.0f)), "queue initial default for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "0.0 m initialization survives game connection reset");
    frame(late_game.stream.sent.back(), distance_type, bits(0.0f));
}

// ---------------------------------------------------------------------------
// Type 181: hero-function keys. Exactly three big-endian uint32 Windows
// virtual-key codes: the combo key (连招键), the toggle key (切换键), then the
// lane key (推线键). Each must be valid_game_hero_key and all three must be
// pairwise different. Defaults: Space / XBUTTON1 / XBUTTON2.
// ---------------------------------------------------------------------------
struct KeyTriple { uint32_t combo, toggle, lane; };
constexpr uint32_t combo_default = chh::wire::game_hero_combo_key_default;
constexpr uint32_t toggle_default = chh::wire::game_hero_toggle_key_default;
constexpr uint32_t lane_default = chh::wire::game_hero_lane_key_default;
Packet keys(uint32_t combo_key, uint32_t toggle_key, uint32_t lane_key) {
    Packet packet(keys_type);
    require(packet.add_number(combo_key) && packet.add_number(toggle_key) && packet.add_number(lane_key),
        "fixture encodes three uint32 key codes");
    return packet;
}
Packet keys(const KeyTriple& triple) { return keys(triple.combo, triple.toggle, triple.lane); }
void keys_frame(const Packet& packet, uint32_t combo_key, uint32_t toggle_key, uint32_t lane_key) {
    require(packet.valid() && packet.type() == keys_type && packet.length() == 12, "valid twelve-byte hero keys frame");
    chh::wire::Reader reader{packet};
    uint32_t combo = UINT32_MAX, toggle = UINT32_MAX, lane = UINT32_MAX;
    require(reader.number(combo) && reader.number(toggle) && reader.number(lane) && reader.done() &&
        combo == combo_key && toggle == toggle_key && lane == lane_key,
        "hero keys frame contains exactly the expected combo, toggle and lane keys");
}
// The whole fixed-size frame: 8-byte header, 12 payload bytes, then only zero padding.
void keys_exact_bytes(const Packet& packet, const uint8_t (&expected)[20], const char* message) {
    require(std::memcmp(packet.bytes.data(), expected, sizeof expected) == 0, message);
    bool trailing_zero = true;
    for (size_t i = sizeof expected; i < packet.bytes.size(); ++i) trailing_zero = trailing_zero && packet.bytes[i] == 0;
    require(trailing_zero, "no bytes beyond the 12-byte hero keys payload");
}
Sessions::HeroKeysSnapshot keys_snapshot(Sessions& sessions, const Sessions::Binding& binding) {
    return sessions.hero_keys_command_snapshot(binding);
}
bool set_keys(Sessions& sessions, const Sessions::Binding& binding, uint32_t combo_key, uint32_t toggle_key,
    uint32_t lane_key) {
    return sessions.set_game_hero_keys(binding, combo_key, toggle_key, lane_key);
}
bool keys_equal(const Sessions::HeroKeysSnapshot& snapshot, uint32_t combo_key, uint32_t toggle_key, uint32_t lane_key) {
    return snapshot.combo_key == combo_key && snapshot.toggle_key == toggle_key && snapshot.lane_key == lane_key;
}
// No key, left/right mouse buttons, VK_CANCEL, reserved 0x07 and anything above 0xFE.
std::vector<uint32_t> invalid_keys() {
    return {0x00u, 0x01u, 0x02u, 0x03u, 0x07u, 0xFFu, 0x100u, UINT32_MAX};
}
// Every pairwise-equal triple shape built from valid keys: combo == toggle, combo == lane,
// toggle == lane and all three equal.
std::vector<KeyTriple> equal_key_triples() {
    std::vector<KeyTriple> triples{
        {combo_default, combo_default, lane_default}, {combo_default, toggle_default, combo_default},
        {combo_default, toggle_default, toggle_default}, {lane_default, toggle_default, lane_default},
        {toggle_default, toggle_default, toggle_default}, {lane_default, lane_default, combo_default}};
    for (const uint32_t same : {0x04u, 0x05u, 0x06u, 0x08u, 0x20u, 0xFEu}) {
        const uint32_t other = 0x41;
        triples.push_back({same, same, other});
        triples.push_back({same, other, same});
        triples.push_back({other, same, same});
        triples.push_back({same, same, same});
    }
    return triples;
}
// One invalid key in each of the three positions (or all three), or a pairwise-equal triple.
std::vector<KeyTriple> invalid_key_triples() {
    std::vector<KeyTriple> triples;
    for (const uint32_t bad : invalid_keys()) {
        triples.push_back({bad, toggle_default, lane_default});
        triples.push_back({combo_default, bad, lane_default});
        triples.push_back({combo_default, toggle_default, bad});
        triples.push_back({bad, bad, bad});
    }
    for (const auto& triple : equal_key_triples()) triples.push_back(triple);
    return triples;
}
void keys_constants_and_wire_format() {
    static_assert(uint32_t(Type::game_hero_keys_set) == 181);
    static_assert(chh::wire::game_hero_combo_key_default == 0x20 && chh::wire::game_hero_toggle_key_default == 0x05 &&
        chh::wire::game_hero_lane_key_default == 0x06);
    require(Packet(keys_type).valid() && !Packet(static_cast<Type>(182)).valid(),
        "type 181 is the highest valid packet type and type 182 is invalid");
    for (const uint32_t ok : {0x04u, 0x05u, 0x06u, 0x08u, 0x09u, 0x20u, 0x41u, 0xA0u, 0xFDu, 0xFEu})
        require(chh::wire::valid_game_hero_key(ok), "middle/side mouse buttons and 0x08..0xFE are valid keys");
    for (const uint32_t bad : invalid_keys())
        require(!chh::wire::valid_game_hero_key(bad), "0x00-0x03, 0x07 and values above 0xFE are invalid keys");
    const KeyTriple valid[]{
        {combo_default, toggle_default, lane_default}, {combo_default, lane_default, toggle_default},
        {toggle_default, combo_default, lane_default}, {toggle_default, lane_default, combo_default},
        {lane_default, combo_default, toggle_default}, {lane_default, toggle_default, combo_default},
        {0x04, 0x05, 0x06}, {0x04, 0xFE, 0x08}, {0x06, 0x08, 0x04}, {0xFE, 0xFD, 0x08}};
    for (const auto& [combo_key, toggle_key, lane_key] : valid)
        require(chh::wire::valid_game_hero_keys(combo_key, toggle_key, lane_key),
            "three pairwise-different valid keys form a valid triple in any order");
    for (const auto& [combo_key, toggle_key, lane_key] : invalid_key_triples())
        require(!chh::wire::valid_game_hero_keys(combo_key, toggle_key, lane_key),
            "an invalid key in any position or any two equal keys form an invalid triple");

    // Forwarded frame: length=12, type 181, combo then toggle then lane key, all big-endian uint32.
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    ui.receive({keys(combo_default, toggle_default, lane_default)});
    game.poll();
    require(game.stream.sent.size() == 1, "first hero keys set forwarded");
    const uint8_t expected[20]{0, 0, 0, 12, 0, 0, 0, 181, 0, 0, 0, 0x20, 0, 0, 0, 0x05, 0, 0, 0, 0x06};
    keys_exact_bytes(game.stream.sent.back(), expected,
        "default hero keys frame is big-endian 32/5/6 with exactly 12 payload bytes");
    ui.receive({keys(0xFE, 0x04, 0x08)});
    game.poll();
    require(game.stream.sent.size() == 2, "changed hero keys forwarded");
    const uint8_t changed[20]{0, 0, 0, 12, 0, 0, 0, 181, 0, 0, 0, 0xFE, 0, 0, 0, 0x04, 0, 0, 0, 0x08};
    keys_exact_bytes(game.stream.sent.back(), changed, "hero keys keep the combo, toggle, lane payload order");
    ui.receive({keys(0x41, 0x42, 0x43)});
    game.poll();
    require(game.stream.sent.size() == 3, "third hero keys set forwarded");
    const uint8_t ordered[20]{0, 0, 0, 12, 0, 0, 0, 181, 0, 0, 0, 0x41, 0, 0, 0, 0x42, 0, 0, 0, 0x43};
    keys_exact_bytes(game.stream.sent.back(), ordered, "lane key is the third and last uint32 of the payload");
}
void keys_routing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    const auto before = keys_snapshot(sessions, own_game);
    require(!before.initialized && before.revision == 0 && keys_equal(before, 32, 5, 6),
        "hero keys start uninitialized at 32 (Space), 5 (lower side button) and 6 (upper side button)");
    game.poll(); game.poll();
    require(game.stream.sent.empty(), "default keys 32/5/6 are not sent before the first set");

    ui.receive({keys(32, 5, 6)});
    const auto initial = keys_snapshot(sessions, own_game);
    require(initial.initialized && initial.revision != 0 && keys_equal(initial, 32, 5, 6),
        "first set of the default keys initializes the command");
    require(!keys_snapshot(sessions, primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "first set of the default keys is forwarded");
    keys_frame(game.stream.sent.back(), 32, 5, 6);
    ui.receive({keys(32, 5, 6), keys(32, 5, 6)});
    game.poll();
    require(game.stream.sent.size() == 1 && keys_snapshot(sessions, own_game).revision == initial.revision,
        "identical key triple is not resent and keeps its revision");

    ui.receive({keys(0x46, 5, 6)});
    const auto changed = keys_snapshot(sessions, own_game);
    require(keys_equal(changed, 0x46, 5, 6) && changed.revision > initial.revision,
        "changing only the combo key advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_hero_keys_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll(); game.poll();
    require(game.stream.sent.size() == 2, "changed combo key forwarded exactly once");
    keys_frame(game.stream.sent.back(), 0x46, 5, 6);
    ui.receive({keys(0x46, 0x04, 6), keys(0x46, 0x04, 6)});
    const auto toggled = keys_snapshot(sessions, own_game);
    require(keys_equal(toggled, 0x46, 0x04, 6) && toggled.revision > changed.revision,
        "changing only the toggle key advances revision");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 3, "changing only the toggle key forwards the triple once");
    keys_frame(game.stream.sent.back(), 0x46, 0x04, 6);
    ui.receive({keys(0x46, 0x04, 0x51), keys(0x46, 0x04, 0x51)});
    const auto laned = keys_snapshot(sessions, own_game);
    require(keys_equal(laned, 0x46, 0x04, 0x51) && laned.revision > toggled.revision,
        "changing only the lane key advances revision");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 4, "changing only the lane key forwards the triple once");
    keys_frame(game.stream.sent.back(), 0x46, 0x04, 0x51);
    size_t forwarded = game.stream.sent.size();
    const KeyTriple edges[]{{0x04u, 0x06u, 0x05u}, {0x06u, 0x05u, 0x04u}, {0x08u, 0xFEu, 0x04u},
        {0xFEu, 0x08u, 0x06u}, {0x05u, 0x06u, 0x20u}, {0x06u, 0x20u, 0x05u}};
    for (const auto& [combo_key, toggle_key, lane_key] : edges) {
        ui.receive({keys(combo_key, toggle_key, lane_key)});
        game.poll(); game.poll();
        require(game.stream.sent.size() == ++forwarded, "edge key triple accepted and forwarded once");
        keys_frame(game.stream.sent.back(), combo_key, toggle_key, lane_key);
    }
    ui.receive({keys(0x06, 0x20, 0x05)});
    game.poll();
    require(game.stream.sent.size() == forwarded, "repeated rotated triple is not resent");
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate hero keys commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    const auto latest = keys_snapshot(sessions, own_game);
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(keys_snapshot(sessions, own_game).revision == latest.revision &&
        keys_equal(keys_snapshot(sessions, own_game), 0x06, 0x20, 0x05), "status changes preserve hero keys");

    sessions.leave(own_game);
    ui.receive({keys(0x51, 0x45, 0x52), keys(0x52, 0x04, 0x51)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only the latest key triple once");
    keys_frame(reconnected.stream.sent.back(), 0x52, 0x04, 0x51);
    require(!keys_snapshot(sessions, own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    // Reconnect with no change in between still re-sends the cached triple.
    sessions.leave(replacement);
    const auto second_replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker resent(sessions, second_replacement);
    resent.poll(); resent.poll();
    require(resent.stream.sent.size() == 1, "unchanged cached key triple re-sent to reconnected game once");
    keys_frame(resent.stream.sent.back(), 0x52, 0x04, 0x51);

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({keys(32, 5, 6)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({keys(32, 5, 6)});
    resent.poll();
    require(resent.stream.sent.size() == 2, "replacement primary can change existing game");
    keys_frame(resent.stream.sent.back(), 32, 5, 6);
    sessions.leave(primary); sessions.leave(second_replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!keys_snapshot(sessions, fresh).initialized && keys_equal(keys_snapshot(sessions, fresh), 32, 5, 6),
        "both endpoints leaving discards cached keys");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(set_keys(sessions, primary, 0x41, 0x42, 0x43), "fixture queues keys before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!keys_snapshot(sessions, fresh).initialized &&
        !set_keys(sessions, primary, 0x44, 0x45, 0x46), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!keys_snapshot(sessions, fresh_game).initialized, "new account session inherits no revoked keys");
}
void keys_independent_controls() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    require(sessions.set_game_auto_afk(primary, 1), "initialize independent auto AFK");
    const auto afk = sessions.auto_afk_command_snapshot(secondary);
    require(!keys_snapshot(sessions, secondary).initialized, "auto AFK does not initialize hero keys");
    game.poll();
    require(game.stream.sent.empty(), "auto AFK changes do not forward a hero keys command");
    ui.receive({keys(0x51, 0x06, 0x05)});
    const auto keys_a = keys_snapshot(sessions, secondary);
    require(keys_a.initialized && keys_equal(keys_a, 0x51, 0x06, 0x05), "hero keys set while auto AFK is on");
    require(sessions.auto_afk_command_snapshot(secondary).enabled == 1 &&
        sessions.auto_afk_command_snapshot(secondary).revision == afk.revision,
        "hero keys do not change auto AFK setting or revision");
    game.poll();
    keys_frame(game.stream.sent.back(), 0x51, 0x06, 0x05);
    ui.receive({keys(0x51, 0x04, 0x05)});
    const auto keys_b = keys_snapshot(sessions, secondary);
    require(keys_equal(keys_b, 0x51, 0x04, 0x05) && keys_b.revision > keys_a.revision, "hero keys change independently");
    require(sessions.set_game_auto_afk(primary, 0) && sessions.set_game_attack_range(primary, 1),
        "other controls can be changed separately");
    require(keys_snapshot(sessions, secondary).revision == keys_b.revision &&
        keys_equal(keys_snapshot(sessions, secondary), 0x51, 0x04, 0x05),
        "other controls do not change hero keys or revision");
    game.poll();
    require(game.stream.sent.size() == 2, "only hero keys revisions control hero keys forwarding");
    keys_frame(game.stream.sent.back(), 0x51, 0x04, 0x05);
    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    keys_frame(replacement.stream.sent.back(), 0x51, 0x04, 0x05);
    require(sessions.auto_afk_command_snapshot(replacement.member.binding).enabled == 0,
        "reconnect preserves independent hero keys and auto AFK settings");
}
void keys_queued_before_game() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({keys(0x51, 0x45, 0x52)});
    Worker game(sessions, enter(sessions, "owner", 10, Role::secondary));
    game.poll(); game.poll();
    require(game.stream.sent.size() == 1, "pre-connection keys reach the first game exactly once");
    keys_frame(game.stream.sent.back(), 0x51, 0x45, 0x52);
    ui.receive({keys(combo_default, toggle_default, lane_default)});
    game.poll();
    require(game.stream.sent.size() == 2, "change back to the defaults after connection reaches the paired game");
    keys_frame(game.stream.sent.back(), combo_default, toggle_default, lane_default);
}
void keys_malformed_and_roles() {
    std::vector<Packet> invalid;
    for (const auto& triple : invalid_key_triples()) invalid.push_back(keys(triple));
    for (size_t length = 0; length <= 16; ++length) {
        if (length == 12) continue;
        Packet packet(keys_type);
        // Leading bytes encode a valid 0x51/0x52/0x53 triple (and a valid 0x51/0x52 pair, the
        // retired 8-byte layout) that differs from the cached keys, so only the length is wrong.
        const uint8_t bytes[16]{0, 0, 0, 0x51, 0, 0, 0, 0x52, 0, 0, 0, 0x53, 0, 0, 0, 0x54};
        require(packet.add_bytes(bytes, length), "fixture creates malformed hero keys payload length");
        invalid.push_back(packet);
    }
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        Worker endpoint(sessions, game);
        ui.receive({keys(0x41, 0x42, 0x43)});
        endpoint.poll();
        const auto revision = keys_snapshot(sessions, game).revision;
        ui.receive({packet, keys(0x44, 0x45, 0x46)});
        error(ui);
        const auto command = keys_snapshot(sessions, game);
        require(keys_equal(command, 0x41, 0x42, 0x43) && command.revision == revision,
            "invalid hero keys are a protocol error that keeps the cache and exits dispatch loop");
        endpoint.poll();
        require(endpoint.stream.sent.size() == 1, "rejected hero keys forward nothing new");
        keys_frame(endpoint.stream.sent.back(), 0x41, 0x42, 0x43);
    }
    // An invalid first request must not initialize the command either.
    for (const auto& packet : invalid) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        Worker endpoint(sessions, game);
        ui.receive({packet});
        error(ui);
        require(!keys_snapshot(sessions, game).initialized && keys_equal(keys_snapshot(sessions, game), 32, 5, 6),
            "rejected first keys leave the defaults uninitialized");
        endpoint.poll();
        require(endpoint.stream.sent.empty(), "rejected keys are never forwarded");
    }
    for (const auto& [combo_key, toggle_key, lane_key] : invalid_key_triples()) {
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        require(!set_keys(sessions, primary, combo_key, toggle_key, lane_key),
            "session API rejects an invalid key in any position and any two equal keys");
        require(!keys_snapshot(sessions, game).initialized, "rejected API call does not initialize");
        require(set_keys(sessions, primary, 0x41, 0x42, 0x43), "primary initializes hero keys");
        const auto queued = keys_snapshot(sessions, game);
        require(!set_keys(sessions, primary, combo_key, toggle_key, lane_key) &&
            keys_equal(keys_snapshot(sessions, game), 0x41, 0x42, 0x43) &&
            keys_snapshot(sessions, game).revision == queued.revision,
            "rejected API call keeps the cached triple and revision");
    }
    // Reverse direction: the game endpoint may not set the keys.
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({keys(0x41, 0x42, 0x43)});
    error(endpoint);
    require(!keys_snapshot(sessions, game).initialized, "secondary cannot initialize hero keys");
    require(!set_keys(sessions, game, 0x41, 0x42, 0x43), "session API rejects game endpoint writes");
    require(!set_keys(sessions, {}, 0x41, 0x42, 0x43), "session API rejects empty binding");
    require(set_keys(sessions, primary, 0x51, 0x45, 0x52), "primary initializes hero keys");
    const auto queued = keys_snapshot(sessions, game);
    Worker reverse(sessions, game);
    reverse.receive({keys(0x41, 0x42, 0x43), keys(32, 5, 6)});
    error(reverse);
    require(keys_equal(keys_snapshot(sessions, game), 0x51, 0x45, 0x52) &&
        keys_snapshot(sessions, game).revision == queued.revision,
        "secondary change attempt keeps primary keys and revision");
    require(set_keys(sessions, primary, 32, 5, 6), "queue default keys for later game connection");
    sessions.leave(game);
    Worker late_game(sessions, enter(sessions, "owner", 10, Role::secondary));
    late_game.poll();
    require(late_game.stream.sent.size() == 1, "default keys initialization survives game connection reset");
    keys_frame(late_game.stream.sent.back(), 32, 5, 6);
}

// ---------------------------------------------------------------------------
// Types 179 and 180 share a pair but keep independent values and revisions.
// ---------------------------------------------------------------------------
void independent_delay_and_distance_channels() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    ui.receive({request(175, hero_type)});
    const auto run = sessions.hero_delay_command_snapshot(secondary);
    require(!sessions.move_distance_command_snapshot(secondary).initialized,
        "run delay does not initialize move distance");
    game.poll();
    require(game.stream.sent.size() == 1, "run delay alone forwards one command");
    frame(game.stream.sent.back(), hero_type, 175);

    ui.receive({distance(0.0f)});
    const auto move = sessions.move_distance_command_snapshot(secondary);
    require(move.initialized && move.distance_bits == bits(0.0f), "0.0 m move distance initializes independently");
    require(sessions.hero_delay_command_snapshot(secondary).revision == run.revision &&
        sessions.hero_delay_command_snapshot(secondary).delay_ms == 175,
        "move distance preserves run delay value and revision");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 2, "move distance does not resend run delay");
    frame(game.stream.sent.back(), distance_type, bits(0.0f));

    ui.receive({request(200, hero_type), request(200, hero_type)});
    require(sessions.move_distance_command_snapshot(secondary).revision == move.revision &&
        sessions.move_distance_command_snapshot(secondary).distance_bits == bits(0.0f),
        "run delay changes preserve move distance value and revision");
    game.poll();
    require(game.stream.sent.size() == 3, "run delay revision forwards independently");
    frame(game.stream.sent.back(), hero_type, 200);
    const auto updated_run = sessions.hero_delay_command_snapshot(secondary);
    ui.receive({distance(0.5f), distance(0.5f)});
    game.poll();
    require(game.stream.sent.size() == 4, "changed move distance forwards once");
    frame(game.stream.sent.back(), distance_type, bits(0.5f));
    require(sessions.hero_delay_command_snapshot(secondary).revision == updated_run.revision,
        "move distance leaves run delay revision unchanged");

    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 2, "reconnect restores both independent commands once");
    frame(replacement.stream.sent[0], distance_type, bits(0.5f));
    frame(replacement.stream.sent[1], hero_type, 200);
}
// Type 181 shares the pair with 179 and 180 but keeps its own values and revision.
void independent_keys_delay_and_distance_channels() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker ui(sessions, primary);
    Worker game(sessions, secondary);
    ui.receive({request(175, hero_type), distance(0.5f)});
    const auto run = sessions.hero_delay_command_snapshot(secondary);
    const auto move = sessions.move_distance_command_snapshot(secondary);
    require(!keys_snapshot(sessions, secondary).initialized && keys_equal(keys_snapshot(sessions, secondary), 32, 5, 6),
        "run delay and move distance do not initialize hero keys");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 2, "run delay and move distance forward without hero keys");
    frame(game.stream.sent[0], distance_type, bits(0.5f));
    frame(game.stream.sent[1], hero_type, 175);

    ui.receive({keys(32, 5, 6)});
    const auto first = keys_snapshot(sessions, secondary);
    require(first.initialized && keys_equal(first, 32, 5, 6), "default hero keys initialize independently");
    require(sessions.hero_delay_command_snapshot(secondary).revision == run.revision &&
        sessions.hero_delay_command_snapshot(secondary).delay_ms == 175 &&
        sessions.move_distance_command_snapshot(secondary).revision == move.revision &&
        sessions.move_distance_command_snapshot(secondary).distance_bits == bits(0.5f),
        "hero keys preserve run delay and move distance values and revisions");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 3, "hero keys do not resend run delay or move distance");
    keys_frame(game.stream.sent.back(), 32, 5, 6);

    ui.receive({request(250, hero_type), distance(1.0f)});
    require(keys_snapshot(sessions, secondary).revision == first.revision &&
        keys_equal(keys_snapshot(sessions, secondary), 32, 5, 6),
        "run delay and move distance changes preserve hero keys value and revision");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 5, "run delay and move distance forward without resending hero keys");
    frame(game.stream.sent[3], distance_type, bits(1.0f));
    frame(game.stream.sent[4], hero_type, 250);
    const auto updated_run = sessions.hero_delay_command_snapshot(secondary);
    const auto updated_move = sessions.move_distance_command_snapshot(secondary);
    ui.receive({keys(0x51, 0x05, 0x06), keys(0x51, 0x05, 0x06)});
    game.poll(); game.poll();
    require(game.stream.sent.size() == 6, "changed hero keys forward once");
    keys_frame(game.stream.sent.back(), 0x51, 0x05, 0x06);
    ui.receive({keys(0x51, 0x05, 0x52)});
    game.poll(); game.poll();
    require(game.stream.sent.size() == 7, "changed lane key forwards once");
    keys_frame(game.stream.sent.back(), 0x51, 0x05, 0x52);
    require(sessions.hero_delay_command_snapshot(secondary).revision == updated_run.revision &&
        sessions.move_distance_command_snapshot(secondary).revision == updated_move.revision,
        "hero keys leave run delay and move distance revisions unchanged");

    const auto updated_keys = keys_snapshot(sessions, secondary);
    Worker rejected(sessions, primary);
    rejected.receive({keys(0x51, 0x05, 0x51)});
    error(rejected);
    require(keys_snapshot(sessions, secondary).revision == updated_keys.revision &&
        keys_equal(keys_snapshot(sessions, secondary), 0x51, 0x05, 0x52) &&
        sessions.hero_delay_command_snapshot(secondary).revision == updated_run.revision &&
        sessions.move_distance_command_snapshot(secondary).revision == updated_move.revision,
        "rejected hero keys change none of the three channels");
    game.poll();
    require(game.stream.sent.size() == 7, "rejected hero keys forward nothing");

    sessions.leave(secondary);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll(); replacement.poll();
    require(replacement.stream.sent.size() == 3, "reconnect restores all three independent commands once");
    frame(replacement.stream.sent[0], distance_type, bits(1.0f));
    keys_frame(replacement.stream.sent[1], 0x51, 0x05, 0x52);
    frame(replacement.stream.sent[2], hero_type, 250);
}
}
int main() {
    try {
        hero_constants_and_wire_format();
        hero_routing_and_reconnect();
        hero_independent_controls();
        hero_queued_before_game();
        hero_malformed_and_roles();
        distance_constants_and_wire_format();
        distance_routing_and_reconnect();
        distance_independent_controls();
        distance_queued_before_game();
        distance_malformed_and_roles();
        keys_constants_and_wire_format();
        keys_routing_and_reconnect();
        keys_independent_controls();
        keys_queued_before_game();
        keys_malformed_and_roles();
        independent_delay_and_distance_channels();
        independent_keys_delay_and_distance_channels();
        std::cout << "PASS: " << checks << " hero delay, move distance and hero keys server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
