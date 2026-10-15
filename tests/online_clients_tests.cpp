// Standalone C++17/x64 tests; no sockets are opened and no database is used.
// cl /nologo /std:c++17 /EHsc /utf-8 /W4 /WX tests\online_clients_tests.cpp
#include "../network/protocol.h"
#include "../sessions.h"
#include <algorithm>
#include <atomic>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using chh::server::ClientRole;
using chh::server::Sessions;
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
bool consistent(const Sessions::ConnectionsSnapshot& snapshot) {
    size_t primary = 0, secondary = 0;
    for (const auto& client : snapshot.clients) {
        if (client.role == ClientRole::primary) ++primary;
        else if (client.role == ClientRole::secondary) ++secondary;
        else return false;
    }
    return primary == snapshot.count.primary && secondary == snapshot.count.secondary &&
        snapshot.clients.size() == primary + secondary && snapshot.count.accounts <= snapshot.clients.size() &&
        (snapshot.clients.empty() == (snapshot.count.accounts == 0));
}
void counts(const Sessions& sessions, uint32_t accounts, uint32_t primary, uint32_t secondary) {
    const auto details = sessions.connections_snapshot();
    const auto ordinary = sessions.snapshot();
    require(consistent(details), "snapshot counts match its connection rows");
    require(details.count.accounts == accounts && details.count.primary == primary && details.count.secondary == secondary,
        "snapshot counts match expected account and endpoint totals");
    require(details.count.accounts == ordinary.accounts && details.count.primary == ordinary.primary &&
        details.count.secondary == ordinary.secondary && details.count.revision == ordinary.revision,
        "connection snapshot preserves existing snapshot count semantics");
}
void row(const Sessions& sessions, uintptr_t socket, ClientRole role, const std::string& name) {
    const auto snapshot = sessions.connections_snapshot();
    const auto found = std::find_if(snapshot.clients.begin(), snapshot.clients.end(),
        [&](const auto& item) { return item.socket == socket; });
    require(found != snapshot.clients.end(), "expected socket is listed");
    require(found->role == role && found->name == name, "socket role and name belong to the same connection");
    require(std::count_if(snapshot.clients.begin(), snapshot.clients.end(),
        [&](const auto& item) { return item.socket == socket; }) == 1, "connection is listed once");
}
void absent(const Sessions& sessions, uintptr_t socket) {
    const auto snapshot = sessions.connections_snapshot();
    require(std::none_of(snapshot.clients.begin(), snapshot.clients.end(),
        [&](const auto& item) { return item.socket == socket; }), "disconnected socket is absent");
}
Sessions::Binding enter(Sessions& sessions, const std::string& account, uint32_t ip,
    ClientRole role, const std::string& name, uintptr_t socket) {
    const auto binding = sessions.enter(account, ip, role, name, socket);
    require(bool(binding), "fixture connection accepted");
    return binding;
}
void lifecycle_tests() {
    static_assert(sizeof(uintptr_t) == 8, "Run this test with the server's x64 toolchain.");
    constexpr uintptr_t large_socket = static_cast<uintptr_t>(UINT64_C(0x100000123));
    Sessions sessions;
    counts(sessions, 0, 0, 0);
    require(sessions.connections_snapshot().clients.empty(), "empty session list has no rows");
    const auto primary = enter(sessions, "same-account", 100, ClientRole::primary, u8"客户端甲", 101);
    const auto game = enter(sessions, "same-account", 100, ClientRole::secondary, {}, large_socket);
    counts(sessions, 1, 1, 1);
    row(sessions, 101, ClientRole::primary, u8"客户端甲");
    row(sessions, large_socket, ClientRole::secondary, "");
    require(sessions.update_game(game, 123, 1, u8"游戏角色甲"), "first game name accepted");
    row(sessions, large_socket, ClientRole::secondary, u8"游戏角色甲");
    row(sessions, 101, ClientRole::primary, u8"客户端甲");

    const auto other_game = enter(sessions, "same-account", 200, ClientRole::secondary, {}, 202);
    require(sessions.update_game(other_game, 456, 3, u8"另一 IP 的角色"), "second IP game state accepted");
    const auto outsider = enter(sessions, "unrelated", 300, ClientRole::primary, "OtherClient", 303);
    counts(sessions, 2, 2, 2);
    row(sessions, large_socket, ClientRole::secondary, u8"游戏角色甲");
    row(sessions, 202, ClientRole::secondary, u8"另一 IP 的角色");
    row(sessions, 303, ClientRole::primary, "OtherClient");
    require(large_socket > UINT32_MAX, "large socket fixture exceeds 32-bit range");
    absent(sessions, static_cast<uint32_t>(large_socket));

    sessions.rename_account("same-account", u8"客户端已改名");
    require(sessions.update_game(game, 789, 2, u8"游戏角色已更新"), "game status refresh accepted");
    counts(sessions, 2, 2, 2);
    row(sessions, 101, ClientRole::primary, u8"客户端已改名");
    row(sessions, large_socket, ClientRole::secondary, u8"游戏角色已更新");
    row(sessions, 202, ClientRole::secondary, u8"另一 IP 的角色");

    require(sessions.leave(primary), "primary leaves while game endpoints remain");
    counts(sessions, 2, 1, 2);
    absent(sessions, 101);
    row(sessions, large_socket, ClientRole::secondary, u8"游戏角色已更新");
    row(sessions, 202, ClientRole::secondary, u8"另一 IP 的角色");
    sessions.leave(game);
    counts(sessions, 2, 1, 1);
    absent(sessions, large_socket);
    const auto replacement = enter(sessions, "same-account", 100, ClientRole::secondary, {}, large_socket + 1);
    counts(sessions, 2, 1, 2);
    row(sessions, large_socket + 1, ClientRole::secondary, "");
    absent(sessions, large_socket);
    sessions.leave(game);
    require(sessions.current(replacement), "late old disconnect preserves replacement binding");
    require(!sessions.update_game(game, 123, 1, "StaleName"), "stale binding cannot rename replacement");
    row(sessions, large_socket + 1, ClientRole::secondary, "");
    require(sessions.update_game(replacement, 999, 1, "FreshGame"), "replacement game name accepted");
    row(sessions, large_socket + 1, ClientRole::secondary, "FreshGame");

    const auto returning_primary = enter(sessions, "same-account", 100, ClientRole::primary, "ReturningClient", 404);
    counts(sessions, 2, 2, 2);
    row(sessions, 404, ClientRole::primary, "ReturningClient");
    sessions.invalidate_account("same-account");
    counts(sessions, 1, 1, 0);
    absent(sessions, 202);
    absent(sessions, large_socket + 1);
    absent(sessions, 404);
    require(!sessions.current(returning_primary) && !sessions.current(replacement) && !sessions.current(other_game),
        "invalidation removes every primary and game endpoint of the account");
    require(sessions.current(outsider), "invalidation preserves unrelated binding");
    row(sessions, 303, ClientRole::primary, "OtherClient");
    sessions.leave(outsider);
    counts(sessions, 0, 0, 0);

    const auto default_socket = sessions.enter("legacy-caller", 900, ClientRole::primary, "DefaultSocket");
    require(bool(default_socket), "existing enter call remains supported");
    row(sessions, 0, ClientRole::primary, "DefaultSocket");
    std::cout << "PASS connection names, sockets and lifecycle\n";
}
void concurrent_snapshot_test() {
    Sessions sessions;
    enter(sessions, "stable", 1, ClientRole::primary, "Stable", 1);
    std::atomic<bool> start{false}, done{false};
    std::atomic<unsigned> errors{0}, observed{0};
    std::thread reader([&] {
        while (!start.load()) std::this_thread::yield();
        do {
            const auto snapshot = sessions.connections_snapshot();
            if (!consistent(snapshot) || snapshot.count.accounts < 1 || snapshot.count.accounts > 2) ++errors;
            ++observed;
            std::this_thread::yield();
        } while (!done.load());
    });
    start = true;
    for (unsigned i = 0; i < 100; ++i) {
        const auto primary = sessions.enter("changing", 2, ClientRole::primary, "Changing", 10);
        const auto game = sessions.enter("changing", 2, ClientRole::secondary, {}, 20);
        if (!primary || !game || !sessions.update_game(game, i + 1, 1, "ChangingGame")) ++errors;
        std::this_thread::yield();
        sessions.leave(primary);
        std::this_thread::yield();
        sessions.leave(game);
    }
    done = true;
    reader.join();
    require(observed > 0 && errors == 0, "concurrent snapshots keep counts and rows internally consistent");
    counts(sessions, 1, 1, 0);
    std::cout << "PASS bounded concurrent snapshot consistency\n";
}
} // namespace

int main() {
    try {
        lifecycle_tests();
        concurrent_snapshot_test();
        std::cout << "RESULT " << checks << " checks passed\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
