// Pure C++17/x64 tests. No sockets, downloads or executable launches occur.
// cl /nologo /std:c++17 /EHsc /utf-8 /W4 /WX tests\explorer_command_tests.cpp
#include "../network/protocol.h"
#include "../sessions.h"
#include <iostream>
#include <stdexcept>

namespace {
using chh::server::ClientRole;
using chh::server::Sessions;
using chh::wire::Packet;
using chh::wire::Reader;
using chh::wire::Type;
using chh::wire::ExplorerStatus;
using QueueResult = Sessions::ExplorerQueueResult;
using Clock = std::chrono::steady_clock;
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip,
    ClientRole role, uintptr_t socket) {
    const auto binding = sessions.enter(account, ip, role, "SyntheticClient", socket);
    require(bool(binding), "fixture binding accepted");
    return binding;
}
void reject_queue(Sessions& sessions, uintptr_t socket, uint64_t token, QueueResult expected, const char* label) {
    const auto queued = sessions.queue_explorer(socket, token);
    require(queued.result == expected && queued.request_id == 0, label);
}
uint64_t queue(Sessions& sessions, uintptr_t socket, const Sessions::Binding& binding) {
    const auto queued = sessions.queue_explorer(socket, binding.token);
    require(queued.result == QueueResult::queued && queued.request_id != 0, "ready current primary receives nonzero request ID");
    return queued.request_id;
}
void snapshot_token(Sessions& sessions, uintptr_t socket, const Sessions::Binding& binding) {
    unsigned matches = 0;
    for (const auto& client : sessions.connections_snapshot().clients) {
        if (client.socket != socket) continue;
        ++matches;
        require(client.connection_token == binding.token && client.role == binding.role,
            "console snapshot exposes the matching connection token and role");
    }
    require(matches == 1, "socket appears once in console snapshot");
}
void routing_tests() {
    static_assert(sizeof(uintptr_t) == 8, "Use the server's x64 toolchain.");
    constexpr uintptr_t socket = static_cast<uintptr_t>(UINT64_C(0x100000111));
    Sessions sessions;
    const auto target = enter(sessions, "target", 100, ClientRole::primary, socket);
    const auto paired_game = enter(sessions, "target", 100, ClientRole::secondary, 101);
    const auto remote_game = enter(sessions, "target", 200, ClientRole::secondary, 102);
    const auto other = enter(sessions, "other", 300, ClientRole::primary, 103);
    snapshot_token(sessions, socket, target);
    snapshot_token(sessions, 101, paired_game);
    snapshot_token(sessions, 102, remote_game);
    reject_queue(sessions, socket, target.token, QueueResult::not_ready, "unannounced capability blocks launch queue");
    reject_queue(sessions, 999, target.token, QueueResult::not_found, "unknown socket rejected");
    reject_queue(sessions, socket, 0, QueueResult::not_found, "missing snapshot token rejected");
    require(!sessions.enable_explorer({}) && !sessions.enable_explorer(paired_game) && !sessions.enable_explorer(remote_game),
        "only a current primary can declare capability");
    require(sessions.enable_explorer(target) && sessions.enable_explorer(other), "primaries declare capability independently");
    reject_queue(sessions, 101, paired_game.token, QueueResult::not_found, "paired game cannot receive explorer launch");
    reject_queue(sessions, 102, remote_game.token, QueueResult::not_found, "other IP game of same account cannot receive launch");
    reject_queue(sessions, socket, other.token, QueueResult::not_found, "another live primary token cannot target this socket");
    reject_queue(sessions, static_cast<uint32_t>(socket), target.token, QueueResult::not_found, "64-bit socket cannot be targeted by truncated value");

    const auto request = queue(sessions, socket, target);
    reject_queue(sessions, socket, target.token, QueueResult::busy, "second click is busy while request awaits delivery");
    sessions.enable_explorer(target);
    reject_queue(sessions, socket, target.token, QueueResult::busy, "repeated capability announcement does not reset pending work");
    require(sessions.take_explorer_request(paired_game) == 0 && sessions.take_explorer_request(remote_game) == 0 &&
        sessions.take_explorer_request(other) == 0, "queue is isolated from games and other accounts");
    require(!sessions.explorer_result(target, request, false) && !sessions.explorer_result(target, request, true),
        "results are rejected before launch request has been delivered");
    require(sessions.take_explorer_request(target) == request, "target receives its queued request once");
    require(sessions.take_explorer_request(target) == 0, "request is not replayed on next poll");
    reject_queue(sessions, socket, target.token, QueueResult::busy, "request remains busy after delivery");
    require(!sessions.explorer_result(target, 0, true) && !sessions.explorer_result(target, request + 1, true),
        "zero and unrelated result IDs cannot finish request");
    require(!sessions.explorer_result(other, request, true) && !sessions.explorer_result(paired_game, request, true),
        "wrong primary and game result roles cannot finish request");
    require(sessions.explorer_result(target, request, false), "first downloading notification accepted");
    require(!sessions.explorer_result(target, request, false), "duplicate downloading notification rejected");
    reject_queue(sessions, socket, target.token, QueueResult::busy, "downloading status preserves inflight request");
    const auto independent = queue(sessions, 103, other);
    require(independent != request && sessions.take_explorer_request(other) == independent &&
        sessions.explorer_result(other, independent, true), "another primary can finish an independent simultaneous request");
    reject_queue(sessions, socket, target.token, QueueResult::busy, "other primary result cannot release target request");
    require(sessions.explorer_result(target, request, true), "matching final result completes request");
    require(!sessions.explorer_result(target, request, true), "duplicate final result rejected");
    const auto next = queue(sessions, socket, target);
    require(next != request, "next user request has a fresh ID");
    require(sessions.take_explorer_request(target) == next && sessions.explorer_result(target, next, true),
        "final result without downloading phase also releases request");
    std::cout << "PASS target isolation and one-request lifecycle\n";
}
void reconnection_tests() {
    Sessions sessions;
    auto current = enter(sessions, "moving", 10, ClientRole::primary, 500);
    const auto old_game = enter(sessions, "moving", 10, ClientRole::secondary, 501);
    require(sessions.enable_explorer(current), "initial moving client ready");
    const auto abandoned = queue(sessions, 500, current);
    const auto old = current;
    require(sessions.leave(old), "primary disconnect accepted");
    current = enter(sessions, "moving", 20, ClientRole::primary, 500);
    require(current.token != old.token, "socket reuse has a fresh primary token");
    snapshot_token(sessions, 500, current);
    reject_queue(sessions, 500, old.token, QueueResult::not_found, "captured token cannot target reused socket after migration");
    reject_queue(sessions, 500, current.token, QueueResult::not_ready, "reconnected primary must announce capability again");
    require(!sessions.enable_explorer(old), "stale binding cannot announce capability");
    require(sessions.take_explorer_request(current) == 0 && sessions.take_explorer_request(old) == 0 &&
        sessions.take_explorer_request(old_game) == 0, "abandoned request is not replayed to replacement or old game");
    require(!sessions.explorer_result(old, abandoned, true) && !sessions.explorer_result(current, abandoned, true),
        "old request cannot complete on stale or replacement binding");
    sessions.leave(old);
    require(sessions.current(current), "late old disconnect preserves new primary");
    require(sessions.enable_explorer(current), "replacement primary declares capability");
    const auto active = queue(sessions, 500, current);
    require(active != abandoned && sessions.take_explorer_request(current) == active, "replacement receives only its new request");
    require(!sessions.explorer_result(old, active, true), "stale primary cannot complete replacement request");
    reject_queue(sessions, 500, current.token, QueueResult::busy, "stale result leaves replacement request active");
    require(sessions.explorer_result(current, active, true), "replacement final result accepted");

    const auto invalidated = queue(sessions, 500, current);
    sessions.invalidate_account("moving");
    reject_queue(sessions, 500, current.token, QueueResult::not_found, "account invalidation removes queue target");
    require(!sessions.enable_explorer(current) && sessions.take_explorer_request(current) == 0 &&
        !sessions.explorer_result(current, invalidated, true), "invalidated binding loses capability and work");
    const auto new_same_ip = enter(sessions, "moving", 20, ClientRole::primary, 500);
    reject_queue(sessions, 500, current.token, QueueResult::not_found, "same-IP reconnect also rejects old socket token");
    reject_queue(sessions, 500, new_same_ip.token, QueueResult::not_ready, "invalidation does not preserve capability");
    require(sessions.take_explorer_request(new_same_ip) == 0, "invalidated request is not replayed");
    std::cout << "PASS reconnect, migration and stale-token rejection\n";
}
void timeout_tests() {
    Sessions sessions;
    const auto target = enter(sessions, "timeout", 1, ClientRole::primary, 600);
    const auto game = enter(sessions, "timeout", 1, ClientRole::secondary, 601);
    require(sessions.enable_explorer(target), "timeout fixture ready");
    const auto before = Clock::now();
    const auto request = queue(sessions, 600, target);
    const auto after = Clock::now();
    require(sessions.expire_explorer_request(target, before + std::chrono::minutes(6) - std::chrono::seconds(1)) == 0,
        "request remains active before six-minute deadline");
    require(sessions.expire_explorer_request(game, after + std::chrono::minutes(7)) == 0,
        "game binding cannot expire primary request");
    reject_queue(sessions, 600, target.token, QueueResult::busy, "nonowner expiry does not release request");
    require(sessions.expire_explorer_request(target, after + std::chrono::minutes(6) + std::chrono::seconds(1)) == request,
        "queued request expires after six minutes");
    require(sessions.take_explorer_request(target) == 0, "expired undelivered launch is removed");
    require(sessions.expire_explorer_request(target, after + std::chrono::minutes(8)) == 0,
        "expiry is reported once");
    require(!sessions.explorer_result(target, request, true), "late result cannot revive expired request");
    const auto next = queue(sessions, 600, target);
    require(next != request && sessions.take_explorer_request(target) == next, "explicit new request accepted after expiry");
    require(sessions.explorer_result(target, next, false), "delivered request enters downloading phase");
    require(sessions.expire_explorer_request(target, Clock::now() + std::chrono::minutes(7)) == next,
        "delivered downloading request also expires");
    const auto final = queue(sessions, 600, target);
    require(sessions.take_explorer_request(target) == final && sessions.explorer_result(target, final, true), "completed request fixture");
    require(sessions.expire_explorer_request(target, Clock::now() + std::chrono::minutes(7)) == 0,
        "completed request never expires again");
    std::cout << "PASS deterministic six-minute timeout handling\n";
}
void wire_tests() {
    require(static_cast<uint32_t>(Type::explorer_ready) == 132 && static_cast<uint32_t>(Type::explorer_launch) == 133 &&
        static_cast<uint32_t>(Type::explorer_result) == 134, "explorer message numbers remain unchanged");
    require(static_cast<uint32_t>(ExplorerStatus::downloading) == 1 && static_cast<uint32_t>(ExplorerStatus::launched) == 2 &&
        static_cast<uint32_t>(ExplorerStatus::failed) == 3 && static_cast<uint32_t>(ExplorerStatus::busy) == 4 &&
        static_cast<uint32_t>(ExplorerStatus::cancelled) == 5, "result status wire values are stable");
    require(chh::wire::explorer_message_limit == 384 && !chh::wire::valid_explorer_status(0) &&
        !chh::wire::valid_explorer_status(6) && !chh::wire::valid_explorer_status(UINT32_MAX),
        "message size and status validation boundaries match the wire contract");
    Packet ready(Type::explorer_ready);
    require(ready.valid() && ready.length() == 0 && Reader{ready}.done(), "ready frame has empty payload");
    require(Packet(Type::explorer_result).valid() && Packet(Type::game_movement_trace_set).valid() &&
        Packet(Type::game_instant_pick_set).valid() && !Packet(static_cast<Type>(182)).valid() && !Packet(static_cast<Type>(0)).valid(),
        "supported wire types are valid and unknown types are rejected");
    const uint64_t id = UINT64_C(0xfedcba9876543210);
    Packet launch(Type::explorer_launch);
    require(launch.add_u64(id) && launch.length() == 8, "launch frame carries one 64-bit request ID");
    Reader launch_reader{launch}; uint64_t decoded_id = 0;
    require(launch_reader.u64(decoded_id) && decoded_id == id && launch_reader.done(), "launch ID round-trips without truncation");
    auto short_launch = launch;
    chh::wire::write32(short_launch.bytes.data(), 7);
    Reader short_reader{short_launch};
    require(!short_reader.u64(decoded_id), "truncated request ID cannot be read");
    auto extra_launch = launch;
    require(extra_launch.add_number(1), "extra launch payload fixture");
    Reader extra_reader{extra_launch};
    require(extra_reader.u64(decoded_id) && !extra_reader.done(), "trailing launch fields are detectable");
    std::string max_message;
    for (unsigned i = 0; i < 96; ++i) max_message += u8"😀";
    for (const auto status : {ExplorerStatus::downloading, ExplorerStatus::launched, ExplorerStatus::failed,
        ExplorerStatus::busy, ExplorerStatus::cancelled}) {
        require(chh::wire::valid_explorer_status(static_cast<uint32_t>(status)), "declared explorer status passes protocol validation");
        Packet result(Type::explorer_result);
        require(result.add_u64(id) && result.add_number(static_cast<uint32_t>(status)) &&
            result.add_string(max_message, 384, true), "all result statuses encode with maximum UTF-8 message");
        require(result.valid() && result.length() == 398, "result payload fits fixed packet with exact field lengths");
        Reader reader{result}; uint32_t decoded_status = 0; std::string message;
        require(reader.u64(decoded_id) && reader.number(decoded_status) && reader.string(message, 384, true) &&
            reader.done() && decoded_id == id && decoded_status == static_cast<uint32_t>(status) && message == max_message,
            "result request status and UTF-8 message round-trip");
        chh::wire::write32(result.bytes.data(), result.length() - 1);
        Reader truncated{result};
        require(truncated.u64(decoded_id) && truncated.number(decoded_status) && !truncated.string(message, 384, true),
            "truncated UTF-8 result payload is rejected");
    }
    Packet message(Type::explorer_result);
    require(!message.add_string(std::string(385, 'x'), 384, true) && message.length() == 0, "385-byte message rejected without partial payload");
    require(!message.add_string(std::string("a\0b", 3), 384, true) && !message.add_string("line\nfeed", 384, true) &&
        !message.add_string(std::string("\xc0\xaf", 2), 384, true), "invalid text cannot enter result payload");
    require(message.add_u64(id) && message.add_number(3) && message.add_string("", 384, true) && message.length() == 14,
        "failed result with empty detail has a 14-byte payload");
    Reader empty_reader{message}; uint32_t status = 0; std::string text;
    require(empty_reader.u64(decoded_id) && empty_reader.number(status) && empty_reader.string(text, 384, true) &&
        empty_reader.done() && decoded_id == id && status == 3 && text.empty(), "empty result detail round-trips");
    chh::wire::write32(message.bytes.data(), static_cast<uint32_t>(chh::wire::payload_size + 1));
    require(!message.valid(), "oversized frame remains invalid at new protocol upper bound");
    std::cout << "PASS explorer wire types and payload boundaries\n";
}
} // namespace

int main() {
    try {
        routing_tests(); reconnection_tests(); timeout_tests(); wire_tests();
        std::cout << "RESULT " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
