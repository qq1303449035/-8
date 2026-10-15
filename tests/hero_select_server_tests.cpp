// The launcher extracts the real dispatch and forwarding branches. Only TLS
// writes are replaced; these tests do not start a server or connect to a DB.
#include "../network/protocol.h"
#include "../sessions.h"
#include <iostream>
#include <stdexcept>
#include <functional>

namespace chh::server {
unsigned hero_load_calls = 0;
bool hero_load_available = true;
std::vector<uint32_t> hero_load_ids{1, 2, 69, 308, 540, 1025, 65535};
std::string hero_load_actor;
std::function<void()> during_hero_load;
bool load_available_heroes(const std::string& actor, std::vector<uint32_t>& ids, std::string*) {
    ++hero_load_calls; hero_load_actor = actor;
    if (during_hero_load) during_hero_load();
    ids = hero_load_ids;
    return hero_load_available;
}
}

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
using Clock = std::chrono::steady_clock;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "hero_select_senders.inc"

struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    struct { std::string account_id; } profile;
    std::vector<uint32_t> available_hero_ids;
    Clock::time_point available_heroes_expires{};
    chh::tls::Stream stream;
    uint64_t sent_hero_select_command_revision = UINT64_MAX;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding}, profile{binding.key.first} {
        require(online.current(binding), "fixture worker has a current binding");
    }
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "hero_select_handler.inc"
        }
    }
    void poll() {
        do {
#include "hero_select_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip, Role role) {
    auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters a new session");
    return result;
}
Packet request(uint32_t hero_id) {
    Packet packet(Type::game_hero_select_set);
    require(packet.add_number(hero_id), "fixture encodes hero ID");
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
void routing_and_reconnect() {
    Sessions sessions;
    auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    auto own_game = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, own_game);
    Worker other_ip(sessions, enter(sessions, "owner", 20, Role::secondary));
    Worker other_account(sessions, enter(sessions, "other", 10, Role::secondary));
    game.poll();
    require(game.stream.sent.empty(), "no unsolicited command before first initialization");
    ui.receive({request(0)});
    const auto initial = sessions.hero_select_command_snapshot(own_game);
    require(initial.initialized && initial.revision != 0 && initial.hero_id == 0, "first clear value initializes command");
    require(!sessions.hero_select_command_snapshot(primary).initialized, "primary cannot read game command");
    game.poll();
    require(game.stream.sent.size() == 1, "paired game receives first clear value");
    frame(game.stream.sent.back(), Type::game_hero_select_set, 0);
    ui.receive({request(0)});
    game.poll();
    require(game.stream.sent.size() == 1 && sessions.hero_select_command_snapshot(own_game).revision == initial.revision,
        "duplicate settings and polls do not resend");
    ui.receive({request(1)});
    const auto selected = sessions.hero_select_command_snapshot(own_game);
    require(selected.hero_id == 1 && selected.revision > initial.revision, "selecting a hero advances revision");
    game.stream.writable = false;
    game.poll();
    require(game.sent_hero_select_command_revision == initial.revision, "failed write does not mark command sent");
    game.stream.writable = true;
    game.poll();
    require(game.stream.sent.size() == 2, "changed value forwarded exactly once");
    frame(game.stream.sent.back(), Type::game_hero_select_set, 1);
    other_ip.poll(); other_account.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty(), "account and IP isolate hero selection commands");
    require(ui.stream.sent.empty(), "successful primary requests generate no error");
    require(sessions.update_game(own_game, 42, 1, "Player"), "game status update accepted");
    require(sessions.hero_select_command_snapshot(own_game).revision == selected.revision, "status changes preserve hero selection");

    sessions.leave(own_game);
    ui.receive({request(0), request(1)});
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement);
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect replays only latest setting once");
    frame(reconnected.stream.sent.back(), Type::game_hero_select_set, 1);
    require(!sessions.hero_select_command_snapshot(own_game).initialized, "old game token loses snapshot access");
    game.stream.sent.clear(); game.poll();
    require(game.stream.sent.empty(), "stale game receives no replacement command");
    sessions.leave(own_game);
    require(sessions.current(replacement), "late old disconnect preserves replacement game");

    sessions.leave(primary);
    primary = enter(sessions, "owner", 10, Role::primary);
    ui.receive({request(0)});
    error(ui);
    Worker new_ui(sessions, primary);
    new_ui.receive({request(0)});
    reconnected.poll();
    require(reconnected.stream.sent.size() == 2, "replacement primary can change existing game");
    frame(reconnected.stream.sent.back(), Type::game_hero_select_set, 0);
    sessions.leave(primary); sessions.leave(replacement);
    const auto fresh = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.hero_select_command_snapshot(fresh).initialized, "both endpoints leaving discards cached setting");

    primary = enter(sessions, "owner", 10, Role::primary);
    require(sessions.set_game_hero_select(primary, 1), "fixture queues setting before revocation");
    require(sessions.invalidate_account("owner"), "account invalidation removes active primary");
    require(!sessions.hero_select_command_snapshot(fresh).initialized &&
        !sessions.set_game_hero_select(primary, 0), "invalidated bindings lose read and write access");
    require(sessions.current(other_account.member.binding), "revocation preserves other accounts");
    primary = enter(sessions, "owner", 10, Role::primary);
    const auto fresh_game = enter(sessions, "owner", 10, Role::secondary);
    require(!sessions.hero_select_command_snapshot(fresh_game).initialized, "new account session inherits no revoked setting");
}
void all_ids_and_independence() {
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    Worker ui(sessions, primary);
    ui.receive({request(65535)});
    require(ui.stream.sent.empty(), "primary can select before any game connects");
    const auto secondary = enter(sessions, "owner", 10, Role::secondary);
    Worker game(sessions, secondary);
    game.poll();
    require(game.stream.sent.size() == 1, "first game receives queued selection");
    frame(game.stream.sent.back(), Type::game_hero_select_set, 65535);
    const auto encoded = request(0x12345678u);
    require(encoded.bytes[8] == 0x12 && encoded.bytes[9] == 0x34 &&
        encoded.bytes[10] == 0x56 && encoded.bytes[11] == 0x78, "hero ID payload uses big-endian encoding");
    require(sessions.set_game_eye_timer(primary, 1) && sessions.set_game_jungle_timer(primary, 1),
        "initialize independent feature cards");
    const auto eye = sessions.eye_timer_command_snapshot(secondary);
    const auto jungle = sessions.jungle_timer_command_snapshot(secondary);
    const uint32_t values[]{0u, 1u, 2u, 540u, 1025u, 65535u, 0u};
    uint64_t previous = sessions.hero_select_command_snapshot(secondary).revision;
    size_t sent_count = game.stream.sent.size();
    for (const auto hero_id : values) {
        ui.receive({request(hero_id)});
        game.poll();
        require(game.stream.sent.size() == ++sent_count, "authorized hero ID forwards exactly once");
        frame(game.stream.sent.back(), Type::game_hero_select_set, hero_id);
        const auto selected = sessions.hero_select_command_snapshot(secondary);
        require(selected.initialized && selected.hero_id == hero_id && selected.revision > previous,
            "authorized hero ID is retained without boolean conversion");
        previous = selected.revision;
        ui.receive({request(hero_id)});
        game.poll();
        require(game.stream.sent.size() == sent_count && sessions.hero_select_command_snapshot(secondary).revision == previous,
            "duplicate hero ID does not advance revision or resend");
    }
    require(ui.stream.sent.empty(), "authorized hero IDs accepted");
    require(sessions.eye_timer_command_snapshot(secondary).enabled == 1 &&
        sessions.eye_timer_command_snapshot(secondary).revision == eye.revision &&
        sessions.jungle_timer_command_snapshot(secondary).enabled == 1 &&
        sessions.jungle_timer_command_snapshot(secondary).revision == jungle.revision,
        "hero selection leaves feature values and revisions unchanged");
    require(sessions.set_game_eye_timer(primary, 0) && sessions.set_game_jungle_timer(primary, 0),
        "feature cards can change independently");
    require(sessions.hero_select_command_snapshot(secondary).hero_id == 0 &&
        sessions.hero_select_command_snapshot(secondary).revision == previous,
        "feature cards do not change hero selection or its revision");
    game.poll();
    require(game.stream.sent.size() == sent_count, "feature changes do not resend selection");
    sessions.leave(secondary);
    ui.receive({request(65535)});
    Worker reconnected(sessions, enter(sessions, "owner", 10, Role::secondary));
    reconnected.poll(); reconnected.poll();
    require(reconnected.stream.sent.size() == 1, "reconnect receives latest authorized hero ID once");
    frame(reconnected.stream.sent.back(), Type::game_hero_select_set, 65535);
}
void malformed_and_roles() {
    static_assert(uint32_t(Type::game_hero_select_set) == 126);
    require(request(0).valid() && !Packet(static_cast<Type>(0)).valid() &&
        !Packet(static_cast<Type>(UINT32_MAX)).valid(), "hero selection type is supported and unknown types are rejected");
    for (const size_t length : {0u, 1u, 2u, 3u, 5u, 6u, 7u, 8u, 12u, 16u, uint32_t(chh::wire::payload_size)}) {
        Packet packet(Type::game_hero_select_set);
        const uint8_t bytes[chh::wire::payload_size]{};
        require(packet.add_bytes(bytes, length), "fixture creates malformed payload length");
        Sessions sessions;
        const auto primary = enter(sessions, "owner", 10, Role::primary);
        const auto game = enter(sessions, "owner", 10, Role::secondary);
        Worker ui(sessions, primary);
        ui.receive({request(540)});
        const auto revision = sessions.hero_select_command_snapshot(game).revision;
        ui.receive({packet, request(0)});
        error(ui);
        const auto selected = sessions.hero_select_command_snapshot(game);
        require(selected.hero_id == 540 && selected.revision == revision,
            "malformed request preserves selection and exits dispatch loop");
    }
    Sessions sessions;
    const auto primary = enter(sessions, "owner", 10, Role::primary);
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    Worker endpoint(sessions, game);
    endpoint.receive({request(UINT32_MAX)});
    error(endpoint);
    require(!sessions.hero_select_command_snapshot(game).initialized, "secondary cannot initialize selection");
    require(!sessions.set_game_hero_select({}, 0) && !sessions.set_game_hero_select(game, 0),
        "session API rejects empty binding and game role even when clearing selection");
    require(sessions.set_game_hero_select(primary, 0), "initial clear selection is accepted");
    sessions.leave(game);
    Worker replacement(sessions, enter(sessions, "owner", 10, Role::secondary));
    replacement.poll();
    require(replacement.stream.sent.size() == 1, "explicit zero survives game connection reset");
    frame(replacement.stream.sent.back(), Type::game_hero_select_set, 0);
}
void authorization_and_expiry() {
    using namespace chh::server;
    Sessions sessions;
    Worker ui(sessions, enter(sessions, "owner", 10, Role::primary));
    const auto game = enter(sessions, "owner", 10, Role::secondary);
    hero_load_calls = 0;
    ui.receive({request(1)});
    require(hero_load_calls == 1 && hero_load_actor == "owner", "first select resolves authenticated actor");
    const auto initial = sessions.hero_select_command_snapshot(game);
    ui.receive({request(3), request(65536), request(UINT32_MAX)});
    require(hero_load_calls == 1 && ui.stream.sent.empty() && sessions.hero_select_command_snapshot(game).revision == initial.revision,
        "unavailable and out-of-range heroes do not disconnect or forward");
    ui.receive({request(2)});
    require(hero_load_calls == 1 && sessions.hero_select_command_snapshot(game).hero_id == 2, "valid 30-second cache avoids database reload");
    ui.available_heroes_expires = Clock::now() - std::chrono::seconds(1);
    hero_load_ids = {1};
    ui.receive({request(2)});
    require(hero_load_calls == 2 && ui.available_hero_ids == hero_load_ids, "expired cache refreshes without retaining removed IDs");
    require(sessions.hero_select_command_snapshot(game).hero_id == 0, "expired authorization clears saved revoked selection");
    Worker endpoint(sessions, game); endpoint.poll();
    frame(endpoint.stream.sent.back(), Type::game_hero_select_set, 0);
    sessions.leave(game);
    const auto replacement = enter(sessions, "owner", 10, Role::secondary);
    Worker reconnected(sessions, replacement); reconnected.poll();
    frame(reconnected.stream.sent.back(), Type::game_hero_select_set, 0);
    ui.receive({request(1), request(2)});
    require(sessions.hero_select_command_snapshot(replacement).hero_id == 1, "rejecting unowned request preserves different authorized selection");
    const auto selected = sessions.hero_select_command_snapshot(replacement);
    ui.available_heroes_expires = {};
    hero_load_available = false;
    ui.receive({request(1), request(1)});
    require(hero_load_calls == 3 && ui.available_hero_ids.empty() && sessions.hero_select_command_snapshot(replacement).hero_id == 0 &&
        sessions.hero_select_command_snapshot(replacement).revision > selected.revision, "database failure clears stale selection and bounds failed retries");
    ui.receive({request(0)});
    require(hero_load_calls == 3 && sessions.hero_select_command_snapshot(replacement).hero_id == 0, "clear always works during database outage");
    hero_load_available = true;
    ui.available_heroes_expires = {};
    during_hero_load = [&] { sessions.invalidate_account("owner"); };
    ui.receive({request(1)});
    require(ui.stream.sent.size() == 1 && ui.available_hero_ids.empty(), "revocation during database lookup never publishes access");
    frame(ui.stream.sent.back(), Type::error, uint32_t(chh::wire::Error::credentials_changed));
    during_hero_load = {};
    Worker fresh(sessions, enter(sessions, "owner", 10, Role::primary));
    require(fresh.available_hero_ids.empty() && fresh.available_heroes_expires == Clock::time_point{}, "new connection never inherits authorization cache");
}
}
int main() {
    try {
        routing_and_reconnect();
        all_ids_and_independence();
        malformed_and_roles();
        authorization_and_expiry();
        std::cout << "PASS: " << checks << " hero selection server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
