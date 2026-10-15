// Real production dispatch and forwarding, with an offline TLS sink only.
#include "../network/protocol.h"
#include "../sessions.h"
#include <iostream>
#include <stdexcept>

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
using namespace chh::wire;
using Sessions = chh::server::Sessions;
using Role = chh::server::ClientRole;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
#include "skin_action_senders.inc"
struct Worker {
    Sessions& online;
    struct { Sessions::Binding binding; } member;
    chh::tls::Stream stream;
    Worker(Sessions& sessions, Sessions::Binding binding) : online(sessions), member{binding} {}
    void receive(const std::vector<Packet>& packets) {
        for (const auto& packet : packets) {
#include "skin_action_handler.inc"
        }
    }
    void poll() {
        do {
#include "skin_action_forward.inc"
        } while (false);
    }
};
Sessions::Binding enter(Sessions& sessions, Role role, const char* account = "owner", uint32_t ip = 10) {
    const auto result = sessions.enter(account, ip, role);
    require(bool(result), "fixture enters account/IP pair");
    return result;
}
Packet request(uint32_t action, uint32_t skin_id, uint64_t target) {
    Packet packet(Type::game_skin_action);
    require(packet.add_number(action) && packet.add_number(skin_id) && packet.add_u64(target) &&
        packet.length() == 16, "primary action contains exactly 16 bytes");
    return packet;
}
void frame(const Packet& packet, uint32_t action, uint32_t skin_id) {
    Reader reader{packet}; uint32_t actual_action = 99, actual_skin = 999;
    require(packet.valid() && packet.type() == Type::game_skin_action && packet.length() == 8 &&
        reader.number(actual_action) && reader.number(actual_skin) && reader.done() &&
        action == actual_action && skin_id == actual_skin, "game receives exactly action and skin ID");
}
void protocol_error(Worker& worker, const Packet& packet) {
    worker.stream.sent.clear();
    worker.receive({packet});
    require(worker.stream.sent.size() == 1 && worker.stream.sent[0].type() == Type::error &&
        worker.stream.sent[0].number() == uint32_t(Error::protocol), "malformed or unauthorized action rejected");
}
void boundary_and_routing() {
    static_assert(uint32_t(Type::game_skin_action) == 175);
    require(Packet(Type::game_skin_action).valid() && !Packet(static_cast<Type>(182)).valid(), "protocol boundary correct");
    require(valid_game_skin_action(0, 0) && valid_game_skin_action(0, 255) &&
        valid_game_skin_action(1, 0) && valid_game_skin_action(2, 0), "all valid skin action boundaries accepted");
    Sessions sessions;
    Worker ui(sessions, enter(sessions, Role::primary));
    Worker game(sessions, enter(sessions, Role::secondary));
    Worker other_ip(sessions, enter(sessions, Role::secondary, "owner", 20));
    Worker other_account(sessions, enter(sessions, Role::secondary, "other"));
    const auto target = game.member.binding.token;
    ui.receive({request(0, 0, target), request(0, 255, target), request(1, 0, target), request(2, 0, target)});
    other_ip.poll(); other_account.poll(); ui.poll();
    require(other_ip.stream.sent.empty() && other_account.stream.sent.empty() && ui.stream.sent.empty(),
        "only currently paired secondary can consume actions");
    game.poll(); game.poll();
    require(game.stream.sent.size() == 4, "ordered actions consumed exactly once");
    frame(game.stream.sent[0], 0, 0); frame(game.stream.sent[1], 0, 255);
    frame(game.stream.sent[2], 1, 0); frame(game.stream.sent[3], 2, 0);
    for (const auto& invalid : std::vector<std::pair<uint32_t, uint32_t>>{{0, 256}, {0, UINT32_MAX}, {1, 1}, {2, 255}, {3, 0}, {UINT32_MAX, 0}}) {
        require(!valid_game_skin_action(invalid.first, invalid.second) &&
            !sessions.request_game_skin_action(ui.member.binding, invalid.first, invalid.second, target), "invalid action rejected by session API");
        protocol_error(ui, request(invalid.first, invalid.second, target));
    }
    protocol_error(ui, request(0, 0, 0));
    Packet short_frame(Type::game_skin_action); short_frame.add_number(0); short_frame.add_number(4);
    protocol_error(ui, short_frame);
    auto long_frame = request(0, 4, target); long_frame.add_number(0); protocol_error(ui, long_frame);
    protocol_error(game, request(0, 4, target));
    require(!sessions.request_game_skin_action({}, 0, 4, target) &&
        !sessions.request_game_skin_action(game.member.binding, 0, 4, target), "unauthenticated and secondary callers rejected");
    ui.stream.sent.clear(); ui.receive({request(0, 8, other_ip.member.binding.token), request(0, 8, other_account.member.binding.token)});
    require(ui.stream.sent.empty() && sessions.take_game_skin_actions(game.member.binding).empty(),
        "different account/IP target ignored without queueing or dropping primary");
    for (unsigned i = 0; i != 32; ++i) require(sessions.request_game_skin_action(ui.member.binding, 2, 0, target), "queue accepts bounded repeated actions");
    require(!sessions.request_game_skin_action(ui.member.binding, 2, 0, target), "queue overflow refused");
    game.stream.sent.clear(); game.poll(); game.poll();
    require(game.stream.sent.size() == 32, "all repeated shortcuts delivered once without merging");
}
void reconnect_and_failed_write() {
    Sessions sessions;
    Worker ui(sessions, enter(sessions, Role::primary));
    require(!sessions.request_game_skin_action(ui.member.binding, 0, 7, 10), "no game cannot queue future action");
    Worker game(sessions, enter(sessions, Role::secondary));
    game.poll(); require(game.stream.sent.empty(), "new game never inherits absent-game request");
    ui.receive({request(0, 12, game.member.binding.token)});
    game.stream.writable = false; game.poll(); game.stream.writable = true; game.poll();
    require(game.stream.sent.empty(), "failed one-shot forwarding never replays");
    ui.receive({request(1, 0, game.member.binding.token)});
    const auto stale_game = game.member.binding;
    sessions.leave(stale_game);
    require(!sessions.request_game_skin_action(ui.member.binding, 0, 2, stale_game.token), "disconnected target rejected");
    game.member.binding = enter(sessions, Role::secondary);
    ui.receive({request(0, 9, stale_game.token)}); game.poll();
    require(game.stream.sent.empty(), "replacement game cannot inherit pending or delayed old requests");
    ui.receive({request(2, 0, game.member.binding.token)});
    require(sessions.take_game_skin_actions(stale_game).empty(), "stale game cannot steal fresh action");
    game.poll(); frame(game.stream.sent.back(), 2, 0);
    ui.receive({request(0, 8, game.member.binding.token)});
    const auto stale_ui = ui.member.binding; sessions.leave(stale_ui);
    game.stream.sent.clear(); game.poll(); require(game.stream.sent.empty(), "primary disconnect clears pending actions");
    ui.member.binding = enter(sessions, Role::primary);
    require(!sessions.request_game_skin_action(stale_ui, 0, 8, game.member.binding.token), "stale primary cannot request actions");
    game.poll(); require(game.stream.sent.empty(), "primary reconnect does not replay actions");
    ui.receive({request(0, 8, game.member.binding.token)});
    sessions.leave(ui.member.binding); ui.member.binding = enter(sessions, Role::primary, "owner", 20);
    Worker moved_game(sessions, enter(sessions, Role::secondary, "owner", 20));
    game.poll(); moved_game.poll(); require(game.stream.sent.empty() && moved_game.stream.sent.empty(), "primary IP move clears old pairing actions");
    ui.receive({request(0, 11, moved_game.member.binding.token)}); moved_game.poll(); frame(moved_game.stream.sent.back(), 0, 11);
    require(sessions.invalidate_account("owner") && !sessions.request_game_skin_action(ui.member.binding, 0, 8, moved_game.member.binding.token),
        "account invalidation removes command access");
}
}
int main() {
    try {
        boundary_and_routing(); reconnect_and_failed_write();
        std::cout << "PASS: " << checks << " skin action server checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
