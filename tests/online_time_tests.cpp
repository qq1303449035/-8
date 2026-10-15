// Standalone C++17/x64 behavior tests. Fake clock and memory database only.
#include "../online_time.h"
#include "../network/protocol.h"
#include "../sessions.h"
#include <atomic>
#include <functional>
#include <iostream>
#include <stdexcept>
#include <thread>

namespace {
using chh::server::ClientRole;
using chh::server::OnlineTimeTracker;
using chh::server::Sessions;
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
OnlineTimeTracker::Time at(int64_t seconds) {
    return OnlineTimeTracker::Time{} + std::chrono::seconds(seconds);
}
struct Fixture {
    OnlineTimeTracker tracker;
    std::map<std::string, uint64_t> database;
    std::vector<std::pair<std::string, uint64_t>> writes;
    std::vector<std::string> reports;
    unsigned loads = 0, load_failures = 0, save_failures = 0;
    bool apply_failed_save = false, throw_load = false, throw_save = false;
    std::function<void()> during_save;
    void flush(int64_t now, bool force = false) {
        tracker.flush_due(at(now),
            [&](const std::string& account, uint64_t& base, std::string* diagnostic) {
                ++loads;
                if (throw_load) throw std::runtime_error("load exception");
                if (load_failures) {
                    --load_failures;
                    *diagnostic = "load unavailable";
                    return false;
                }
                base = database[account];
                return true;
            },
            [&](const std::string& account, uint64_t total, std::string* diagnostic) {
                writes.emplace_back(account, total);
                if (during_save) during_save();
                if (throw_save) throw std::runtime_error("save exception");
                if (save_failures) {
                    --save_failures;
                    if (apply_failed_save) database[account] = total;
                    *diagnostic = "save acknowledgement unavailable";
                    return false;
                }
                database[account] = total;
                return true;
            },
            [&](const std::string& diagnostic) { reports.push_back(diagnostic); }, force);
    }
    uint64_t value(const std::string& account = "user") const {
        const auto found = database.find(account);
        return found == database.end() ? 0 : found->second;
    }
};
void periodic_and_baseline_test() {
    Fixture f;
    f.database["user"] = 1000;
    require(f.tracker.start("user", 1, at(0)), "first start admitted");
    f.flush(59);
    require(f.loads == 0 && f.writes.empty(), "nothing persisted before 60 seconds");
    f.flush(60);
    require(f.value() == 1060, "first minute added to existing baseline");
    f.flush(119);
    require(f.writes.size() == 1, "checkpoint is not repeated before next minute");
    f.flush(120);
    require(f.value() == 1120, "second minute saved without double count");
    require(f.tracker.finish("user", 1, at(125)), "finish accepted");
    f.flush(125);
    require(f.value() == 1125, "125 seconds is 60 + 60 + 5");
    require(f.writes.size() == 3 && f.loads == 1, "one baseline read and three absolute writes");
    require(f.tracker.pending_accounts() == 0, "finished saved account removed");
    f.flush(1000, true);
    require(f.value() == 1125 && f.writes.size() == 3, "offline interval adds nothing");
    require(f.tracker.start("user", 2, at(1100)), "later login starts a fresh ledger");
    require(f.tracker.finish("user", 2, at(1110)), "later login finishes");
    f.flush(1110);
    require(f.value() == 1135 && f.loads == 2, "new login loads last persisted baseline");
}
void failed_load_test() {
    Fixture f;
    f.database["user"] = 700;
    f.load_failures = 1;
    f.tracker.start("user", 1, at(0));
    f.flush(60);
    require(f.value() == 700 && f.writes.empty(), "load failure never assumes zero baseline");
    require(f.reports.size() == 1 && f.tracker.pending_accounts() == 1, "load failure retained and reported");
    f.flush(64);
    require(f.loads == 1, "failed load uses retry delay");
    f.flush(65);
    require(f.value() == 765 && f.loads == 2, "load recovery includes all elapsed time");
    f.tracker.finish("user", 1, at(70));
    f.flush(70);
    require(f.value() == 770 && f.tracker.pending_accounts() == 0, "recovered session finishes cleanly");
}
void ambiguous_save_test() {
    Fixture f;
    f.database["user"] = 200;
    f.save_failures = 1;
    f.apply_failed_save = true;
    f.tracker.start("user", 1, at(0));
    f.tracker.finish("user", 1, at(17));
    f.flush(17);
    require(f.value() == 217 && f.tracker.pending_accounts() == 1, "unacknowledged applied save remains pending");
    f.flush(21);
    require(f.writes.size() == 1, "failed save waits five seconds");
    f.flush(22);
    require(f.value() == 217 && f.writes.size() == 2, "retry of applied save does not add twice");
    require(f.writes[0].second == f.writes[1].second && f.loads == 1, "retry retains original baseline and target");
    require(f.tracker.pending_accounts() == 0 && f.reports.size() == 1, "successful retry releases inactive account");
}
void reconnect_after_failure_test() {
    Fixture f;
    f.database["user"] = 100;
    f.save_failures = 2;
    f.apply_failed_save = true;
    f.tracker.start("user", 11, at(0));
    f.tracker.finish("user", 11, at(10));
    f.flush(10);
    f.flush(15);
    require(f.value() == 110 && f.tracker.pending_accounts() == 1, "failed disconnect save survives retries");
    require(f.tracker.start("user", 12, at(100)), "reconnect reuses unsaved ledger");
    require(!f.tracker.finish("user", 11, at(105)), "late old token cannot stop reconnected session");
    f.flush(120);
    require(f.value() == 130, "reconnect counts 10 + 20 seconds and excludes 90 offline seconds");
    f.tracker.finish("user", 12, at(125));
    f.flush(125);
    require(f.value() == 135 && f.loads == 1, "reconnect and ambiguous retries preserve one baseline");
}
void invalid_and_duplicate_tokens_test() {
    Fixture f;
    require(!f.tracker.start("", 1, at(0)), "empty account rejected");
    require(!f.tracker.start("user", 0, at(0)), "zero token rejected");
    require(!f.tracker.finish("user", 1, at(0)), "unknown finish rejected");
    require(f.tracker.pending_accounts() == 0, "invalid operations create no ledger");
    require(f.tracker.start("user", 1, at(0)), "valid session begins");
    require(!f.tracker.start("user", 1, at(20)), "duplicate same-token start rejected");
    require(!f.tracker.start("user", 2, at(30)), "duplicate different-token start rejected");
    require(!f.tracker.finish("user", 2, at(40)), "wrong-token finish rejected");
    require(!f.tracker.finish("user", 0, at(40)), "zero-token finish rejected");
    require(f.tracker.finish("user", 1, at(45)), "original token remains current");
    require(!f.tracker.finish("user", 1, at(90)), "duplicate finish rejected");
    f.flush(90);
    require(f.value() == 45, "duplicate actions neither reset clock nor add offline time");
}
void lifecycle_during_write_test() {
    Fixture f;
    f.tracker.start("user", 1, at(0));
    bool finished = false;
    f.during_save = [&] { finished = f.tracker.finish("user", 1, at(65)); };
    f.flush(60);
    f.during_save = {};
    require(finished && f.value() == 60, "disconnect can complete during database I/O");
    require(f.tracker.pending_accounts() == 1, "disconnect tail newer than write snapshot survives");
    f.flush(65);
    require(f.value() == 65 && f.tracker.pending_accounts() == 0, "disconnect tail is saved on next pass");

    f.tracker.start("user", 2, at(100));
    bool restarted = false;
    f.during_save = [&] {
        finished = f.tracker.finish("user", 2, at(163));
        restarted = f.tracker.start("user", 3, at(170));
    };
    f.flush(160);
    f.during_save = {};
    require(finished && restarted && f.value() == 125, "disconnect and reconnect both complete during write");
    require(f.tracker.pending_accounts() == 1, "in-flight save does not erase replacement session");
    require(!f.tracker.finish("user", 2, at(180)), "stale callback cannot finish replacement");
    f.tracker.finish("user", 3, at(190));
    f.flush(190);
    require(f.value() == 148, "in-flight replacement retains 63 + 20 new online seconds");
}
void exception_and_shutdown_test() {
    Fixture f;
    f.tracker.start("user", 1, at(0));
    f.throw_load = true;
    f.flush(60);
    require(f.reports.size() == 1 && f.tracker.pending_accounts() == 1, "load exception is reported and retained");
    f.throw_load = false;
    f.throw_save = true;
    f.flush(65);
    require(f.reports.size() == 2 && f.value() == 0, "save exception is reported without losing elapsed time");
    f.tracker.finish("user", 1, at(66));
    f.flush(66);
    require(f.reports.size() == 3 && f.tracker.pending_accounts() == 1, "failed final save retains an inactive account for retry");
    f.throw_save = false;
    f.flush(66, true);
    require(f.value() == 66 && f.tracker.pending_accounts() == 0, "shutdown force saves finished work despite retry schedule");

    Fixture active;
    active.tracker.start("user", 2, at(0));
    active.flush(7, true);
    require(active.value() == 7 && active.tracker.pending_accounts() == 1, "force saves active session before first periodic deadline");
    active.tracker.finish("user", 2, at(9));
    active.flush(9, true);
    require(active.value() == 9 && active.tracker.pending_accounts() == 0, "final shutdown saves remaining seconds exactly once");
}
void saturation_and_short_session_test() {
    Fixture f;
    f.database["user"] = UINT64_MAX - 5;
    f.tracker.start("user", 1, at(0));
    f.flush(60);
    require(f.value() == UINT64_MAX, "addition saturates at uint64 maximum");
    f.tracker.finish("user", 1, at(125));
    f.flush(125);
    require(f.value() == UINT64_MAX && f.writes.size() == 1, "saturated total never wraps or needs redundant writes");
    require(f.tracker.pending_accounts() == 0, "saturated inactive account retires");
    f.tracker.start("short", 2, at(0));
    f.tracker.finish("short", 2, at(0) + std::chrono::milliseconds(999));
    f.flush(1);
    require(f.value("short") == 0 && f.tracker.pending_accounts() == 0, "subsecond session rounds down and retires");
    require(f.loads == 1, "zero whole seconds avoid database operations");
}
void serialized_flush_test() {
    OnlineTimeTracker tracker;
    tracker.start("user", 1, at(0));
    std::atomic<unsigned> saves{0}, loads{0}, errors{0}, active{0};
    std::atomic<uint64_t> database{0};
    std::vector<std::thread> workers;
    for (unsigned i = 0; i < 8; ++i) workers.emplace_back([&] {
        tracker.flush_due(at(60),
            [&](const std::string&, uint64_t& base, std::string*) {
                ++loads;
                base = database.load();
                return true;
            },
            [&](const std::string&, uint64_t total, std::string*) {
                if (active.fetch_add(1) != 0) ++errors;
                ++saves;
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                database = total;
                --active;
                return true;
            },
            [&](const std::string&) { ++errors; }, true);
    });
    for (auto& worker : workers) worker.join();
    require(database == 60 && saves == 1 && loads == 1 && errors == 0,
        "concurrent forced flushes serialize and save the same checkpoint once");
}
void session_composition_test() {
    // Exercise the admission/cleanup composition with real Sessions. Socket
    // handlers and database connectivity are intentionally outside this test.
    Fixture f;
    Sessions sessions;
    auto enter = [&](const std::string& account, uint32_t ip, ClientRole role, int64_t now) {
        auto binding = sessions.enter(account, ip, role);
        if (binding && role == ClientRole::primary)
            require(f.tracker.start(account, binding.token, at(now)), "admitted primary starts timer");
        return binding;
    };
    auto leave = [&](const Sessions::Binding& binding, int64_t now) {
        const bool primary_left = sessions.leave(binding);
        if (primary_left)
            f.tracker.finish(binding.key.first, binding.token, at(now));
        return primary_left;
    };
    const auto game_only = enter("game-only", 50, ClientRole::secondary, 0);
    require(bool(game_only) && f.tracker.pending_accounts() == 0, "game-only connection starts no timer");
    const auto primary = enter("user", 100, ClientRole::primary, 0);
    const auto game = enter("user", 100, ClientRole::secondary, 1);
    require(bool(primary) && bool(game), "primary and paired game accepted");
    require(!enter("user", 100, ClientRole::primary, 20), "duplicate primary on same IP rejected");
    require(!enter("user", 200, ClientRole::primary, 30), "duplicate primary on another IP rejected");
    f.flush(60);
    require(f.value() == 60 && f.value("game-only") == 0, "only primary time contributes to account total");
    leave(game, 65);
    require(sessions.current(primary), "game disconnect leaves primary active");
    require(leave(primary, 70), "primary disconnect removes session");
    f.flush(70);
    require(f.value() == 70, "normal disconnect saves primary tail");
    const auto replacement = enter("user", 200, ClientRole::primary, 100);
    require(!leave(primary, 110), "late prior disconnect cannot remove new session");
    require(sessions.current(replacement), "replacement session survives old cleanup");
    f.flush(160);
    require(f.value() == 130, "replacement counts from admission and excludes offline gap");
    // Match the password handler: invalidation then settle the removed primary.
    if (sessions.invalidate_account("user"))
        f.tracker.finish(replacement.key.first, replacement.token, at(165));
    leave(replacement, 200);
    leave(game_only, 200);
    f.flush(200, true);
    require(f.value() == 135 && f.value("game-only") == 0, "invalidation and later cleanup settle once; games add nothing");
    require(f.tracker.pending_accounts() == 0 && sessions.snapshot().accounts == 0, "all sessions and ledgers finish cleanly");
}
}
int main() {
    try {
        periodic_and_baseline_test();
        failed_load_test();
        ambiguous_save_test();
        reconnect_after_failure_test();
        invalid_and_duplicate_tokens_test();
        lifecycle_during_write_test();
        exception_and_shutdown_test();
        saturation_and_short_session_test();
        serialized_flush_test();
        session_composition_test();
        std::cout << "PASS online time behavior tests: " << checks << " checks\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL online time behavior test: " << error.what() << '\n';
        return 1;
    }
}
