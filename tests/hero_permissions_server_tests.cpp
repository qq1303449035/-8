// The launcher extracts the production dispatch branch and packet senders.
// Only database I/O and the TLS stream are replaced; no server or DB is started.
#include "../network/protocol.h"
#include "../sessions.h"
#include "../MySQL.h"
#include <algorithm>
#include <functional>
#include <iostream>
#include <stdexcept>

namespace chh::tls {
struct Stream {
    std::vector<wire::Packet> sent;
    size_t fail_after = SIZE_MAX;
    bool write_all(const void* bytes, size_t size, int) {
        if (size != wire::packet_size || sent.size() >= fail_after) return false;
        wire::Packet packet;
        std::memcpy(packet.bytes.data(), bytes, size);
        sent.push_back(packet);
        return true;
    }
};
}
namespace {
struct Database {
    bool available = true;
    unsigned calls = 0;
    unsigned hero_calls = 0;
    unsigned catalog_calls = 0;
    std::string account;
    std::vector<uint32_t> ids{307, 1307, UINT32_MAX};
    std::vector<uint32_t> heroes{1, 69, 308};
    std::vector<chh::server::HeroCatalogEntry> catalog{{1,"免费英雄",true},{69,"自定义左二少",true},{308,"木更改名",false}};
    std::function<void()> during_load;
} database;
unsigned checks = 0;
void require(bool condition, const char* label) {
    ++checks;
    if (!condition) throw std::runtime_error(label);
}
}
namespace chh::server {
bool load_hero_permissions(const std::string& account, std::vector<uint32_t>& ids, std::string*) noexcept {
    ++database.calls;
    database.account = account;
    if (database.during_load) database.during_load();
    ids = database.ids;
    return database.available;
}
bool load_available_heroes(const std::string& account, std::vector<uint32_t>& ids, std::string*) noexcept {
    ++database.calls; ++database.hero_calls;
    database.account = account;
    if (database.during_load) database.during_load();
    ids = database.heroes;
    return database.available;
}
bool load_hero_catalog(const std::string& account, std::vector<HeroCatalogEntry>& rows, std::string*) noexcept {
    ++database.calls; ++database.catalog_calls;
    database.account = account;
    if (database.during_load) database.during_load();
    rows = database.catalog;
    return database.available;
}
}
namespace {
using Clock = std::chrono::steady_clock;
using Role = chh::server::ClientRole;
using Type = chh::wire::Type;
using Error = chh::wire::Error;
using Result = chh::wire::HeroPermissionsResult;
using Packet = chh::wire::Packet;
#include "hero_permissions_senders.inc"

struct Worker {
    chh::server::Sessions online;
    chh::server::Sessions::Binding primary_binding;
    struct { chh::server::Sessions::Binding binding; } member;
    struct { std::string account_id = "owner"; } profile;
    chh::tls::Stream stream;
    Clock::time_point next_hero_permissions_request{};
    std::vector<uint32_t> available_hero_ids;
    Clock::time_point available_heroes_expires{};
    explicit Worker(Role role = Role::secondary) {
        database = {};
        const auto primary = online.enter(profile.account_id, 123, Role::primary, "Owner");
        primary_binding = primary;
        require(bool(primary) && online.primary_online(profile.account_id, 123), "fixture authenticated primary");
        member.binding = role == Role::primary ? primary : online.enter(profile.account_id, 123, role, "Owner");
        require(online.current(member.binding), "fixture current login binding");
    }
    void run(const std::vector<Packet>& packets) {
        // Matches the live loop's break/continue behavior around the extracted branch.
        for (const auto& packet : packets) {
            const auto still_primary = [&] {
                if (online.current_primary(member.binding)) return true;
                send_error(stream, Error::credentials_changed);
                return false;
            };
            (void)still_primary; // Allows this harness to reject the pre-fix implementation.
#include "hero_permissions_handler.inc"
        }
    }
};
Packet request(uint32_t id = 7) {
    Packet packet(Type::hero_permissions_request);
    require(packet.add_number(id), "fixture request encodes");
    return packet;
}
Packet heroes_request(uint32_t id = 7) {
    auto packet = request(id);
    require(packet.add_number(chh::wire::hero_permissions_scope_heroes), "fixture heroes scope encodes");
    return packet;
}
Packet catalog_request(uint32_t id = 7) {
    auto packet = request(id);
    require(packet.add_number(chh::wire::hero_permissions_scope_catalog), "fixture catalog scope encodes");
    return packet;
}
void frame(const Packet& packet, Type type, std::initializer_list<uint32_t> numbers) {
    require(packet.valid() && packet.type() == type, "expected valid response type");
    chh::wire::Reader reader{packet};
    for (auto expected : numbers) {
        uint32_t actual = 0;
        require(reader.number(actual) && actual == expected, "expected response payload");
    }
    require(reader.done(), "no trailing response payload");
}
void error(const Worker& worker, Error code, unsigned calls) {
    require(database.calls == calls && worker.stream.sent.size() == 1, "error without permission snapshot");
    frame(worker.stream.sent[0], Type::error, {uint32_t(code)});
}
void successful_roles() {
    for (Role role : {Role::primary, Role::secondary}) {
        Worker worker(role);
        worker.run({request()});
        require(database.calls == 1 && database.account == "owner", "load only authenticated account for both roles");
        require(worker.stream.sent.size() == 3, "one complete permissions snapshot");
        frame(worker.stream.sent[0], Type::hero_permissions_begin, {7, 3});
        frame(worker.stream.sent[1], Type::hero_permissions_chunk, {7, 0, 3, 307, 1307, UINT32_MAX});
        frame(worker.stream.sent[2], Type::hero_permissions_end, {7});
    }
    Worker empty;
    database.ids.clear();
    empty.run({request()});
    require(empty.stream.sent.size() == 2, "empty permissions complete without chunks");
    frame(empty.stream.sent[0], Type::hero_permissions_begin, {7, 0});
    frame(empty.stream.sent[1], Type::hero_permissions_end, {7});
}
void rejected_requests() {
    std::vector<Packet> invalid{request(0), Packet(Type::hero_permissions_request), request()};
    require(invalid.back().add_string("victim", 64), "fixture forged account payload");
    for (const auto& packet : invalid) {
        Worker worker;
        worker.run({packet});
        error(worker, Error::protocol, 0);
    }
    Worker limited;
    limited.next_hero_permissions_request = Clock::now() + std::chrono::seconds(1);
    limited.run({request()});
    require(database.calls == 0 && limited.stream.sent.size() == 1, "rate limit prevents database access");
    frame(limited.stream.sent[0], Type::hero_permissions_result, {7, uint32_t(Result::rate_limited)});

    Worker failed;
    database.available = false;
    failed.run({request()});
    require(database.calls == 1 && failed.stream.sent.size() == 1, "database failure publishes no snapshot");
    frame(failed.stream.sent[0], Type::hero_permissions_result, {7, uint32_t(Result::unavailable)});
}
void session_revocation() {
    for (Role role : {Role::primary, Role::secondary}) {
        Worker before(role);
        before.online.invalidate_account("owner");
        before.run({request()});
        error(before, Error::credentials_changed, 0);

        Worker during(role);
        database.during_load = [&] { during.online.invalidate_account("owner"); };
        during.run({request()});
        error(during, Error::credentials_changed, 1);

        Worker replaced(role);
        database.during_load = [&] {
            replaced.online.leave(replaced.member.binding);
            require(bool(replaced.online.enter("owner", 123, role, "Replacement")), "fixture replacement binding");
        };
        replaced.run({request()});
        error(replaced, Error::credentials_changed, 1);
    }
    Worker forged;
    forged.member.binding.key.first = "victim";
    forged.run({request()});
    error(forged, Error::credentials_changed, 0);
}
void available_heroes() {
    Worker worker(Role::primary);
    worker.run({heroes_request()});
    require(database.calls == 1 && database.hero_calls == 1 && database.account == "owner", "hero scope resolves bound actor");
    require(worker.stream.sent.size() == 3 && worker.available_hero_ids == database.heroes, "hero snapshot published and connection cached");
    frame(worker.stream.sent[0], Type::hero_permissions_begin, {7, 3, 1});
    frame(worker.stream.sent[1], Type::hero_permissions_chunk, {7, 0, 3, 1, 69, 308});
    frame(worker.stream.sent[2], Type::hero_permissions_end, {7});
    worker.next_hero_permissions_request = {};
    worker.stream.sent.clear(); worker.run({request(9)});
    require(worker.available_hero_ids == database.heroes && database.hero_calls == 1, "legacy query cannot overwrite hero cache with permission IDs");
    worker.next_hero_permissions_request = {};
    worker.stream.sent.clear(); database.available = false;
    worker.run({heroes_request(8)});
    require(worker.available_hero_ids.empty() && worker.stream.sent.size() == 1, "failed refresh clears old allowlist");
    frame(worker.stream.sent[0], Type::hero_permissions_result, {8, uint32_t(Result::unavailable)});

    Worker secondary;
    secondary.run({heroes_request()}); error(secondary, Error::protocol, 0);
    for (uint32_t scope : {0u, 3u, UINT32_MAX}) {
        Worker invalid(Role::primary); auto packet = request();
        require(packet.add_number(scope), "fixture bad scope"); invalid.run({packet}); error(invalid, Error::protocol, 0);
    }
    Worker forged(Role::primary); auto packet = heroes_request();
    require(packet.add_string("victim", 64), "fixture forged hero actor"); forged.run({packet}); error(forged, Error::protocol, 0);
    Worker revoked(Role::primary);
    database.during_load = [&] { revoked.online.invalidate_account("owner"); };
    revoked.run({heroes_request()}); error(revoked, Error::credentials_changed, 1);
    require(revoked.available_hero_ids.empty(), "revoked result never enters allowlist");
    Worker refreshed(Role::primary);
    const auto game = refreshed.online.enter("owner", 123, Role::secondary, "Owner");
    require(refreshed.online.set_game_hero_select(refreshed.member.binding, 308), "fixture scope1 revoked selection");
    database.heroes = {1,69}; refreshed.run({heroes_request()});
    require(refreshed.online.hero_select_command_snapshot(game).hero_id == 0, "scope1 refresh also clears revoked saved selection");
    Worker interrupted(Role::primary);
    interrupted.stream.fail_after = 2;
    interrupted.run({heroes_request()});
    require(interrupted.stream.sent.size() == 2 && interrupted.available_hero_ids.empty(), "failed snapshot end never caches a partial grant");

    Worker large(Role::primary);
    database.heroes.clear();
    for (uint32_t id = 1; id <= chh::wire::hero_availability_id_limit; ++id) database.heroes.push_back(id);
    large.run({heroes_request()});
    frame(large.stream.sent.front(), Type::hero_permissions_begin, {7, 4096, 1});
    frame(large.stream.sent.back(), Type::hero_permissions_end, {7});
    size_t offset = 0;
    for (size_t i = 1; i + 1 < large.stream.sent.size(); ++i) {
        require(large.stream.sent[i].type() == Type::hero_permissions_chunk, "large scope uses existing chunks");
        chh::wire::Reader reader{large.stream.sent[i]}; uint32_t request_id = 0, start = 0, count = 0;
        require(reader.number(request_id) && reader.number(start) && reader.number(count) && request_id == 7 && start == offset,
            "large snapshot contiguous chunk indices");
        for (uint32_t n = 0; n < count; ++n) { uint32_t id = 0;
            require(reader.number(id) && id == ++offset, "large snapshot exact sorted IDs"); }
        require(reader.done(), "large snapshot exact chunk payload");
    }
    require(offset == 4096 && large.available_hero_ids.size() == 4096, "all 4096 heroes sent and cached");
    chh::tls::Stream rejected;
    require(!send_hero_permissions(rejected, 1, database.heroes) && rejected.sent.empty(), "legacy sender keeps 2048 bound");
}
void hero_catalog() {
    Worker worker(Role::primary);
    const auto game = worker.online.enter("owner", 123, Role::secondary, "Owner");
    require(worker.online.set_game_hero_select(worker.member.binding, 308), "fixture previously selected revoked hero");
    worker.run({catalog_request()});
    require(database.calls == 1 && database.catalog_calls == 1 && database.account == "owner", "catalog resolves only authenticated account");
    require(worker.stream.sent.size() == 5 && worker.available_hero_ids == std::vector<uint32_t>({1,69}), "full catalog includes unavailable heroes but caches only allowed IDs");
    frame(worker.stream.sent.front(), Type::hero_permissions_begin, {7,3,2});
    frame(worker.stream.sent.back(), Type::hero_permissions_end, {7});
    for (size_t index = 0; index < database.catalog.size(); ++index) {
        const auto& packet = worker.stream.sent[index + 1];
        require(packet.type() == Type::hero_permissions_chunk, "catalog uses chunk type");
        chh::wire::Reader reader{packet}; uint32_t req = 0, offset = 0, count = 0, id = 0, allowed = 0; std::string name;
        require(reader.number(req) && reader.number(offset) && reader.number(count) && reader.number(id) && reader.number(allowed) &&
            reader.string(name, chh::wire::hero_catalog_name_limit) && reader.done(), "catalog exact payload fields");
        const auto& expected = database.catalog[index];
        require(req == 7 && offset == index && count == 1 && id == expected.id && allowed == uint32_t(expected.available) && name == expected.name,
            "catalog custom name and permission are paired exactly");
    }
    require(worker.online.hero_select_command_snapshot(game).hero_id == 0, "catalog refresh clears revoked selection");
    require(worker.online.set_game_hero_select(worker.member.binding, 69), "fixture allowed selected hero");
    const auto revision = worker.online.hero_select_command_snapshot(game).revision;
    worker.next_hero_permissions_request = {}; worker.stream.sent.clear();
    worker.run({catalog_request()});
    require(worker.online.hero_select_command_snapshot(game).revision == revision, "catalog refresh preserves another still-authorized selection");
    worker.next_hero_permissions_request = {}; worker.stream.sent.clear(); database.available = false;
    worker.run({catalog_request()});
    require(worker.available_hero_ids.empty() && worker.online.hero_select_command_snapshot(game).hero_id == 0, "failed catalog refresh revokes previous grant and selection");
    frame(worker.stream.sent.front(), Type::hero_permissions_result, {7,uint32_t(Result::unavailable)});

    Worker empty(Role::primary); database.catalog.clear();
    empty.run({catalog_request()});
    require(empty.stream.sent.size() == 2 && empty.available_hero_ids.empty(), "empty catalog is complete");
    frame(empty.stream.sent.front(), Type::hero_permissions_begin, {7,0,2});
    frame(empty.stream.sent.back(), Type::hero_permissions_end, {7});
    Worker longest(Role::primary); database.catalog = {{1,"",true}};
    for (unsigned i = 0; i < 240; ++i) database.catalog[0].name += "\xf0\x9f\x98\x80";
    longest.run({catalog_request()});
    require(longest.stream.sent.size() == 3 && longest.stream.sent[1].length() == 982, "960-byte UTF8 name fits one complete record packet");
    Worker forged(Role::primary); auto packet = catalog_request();
    require(packet.add_string("victim", 64), "fixture forged catalog actor"); forged.run({packet}); error(forged, Error::protocol, 0);
    Worker revoked(Role::primary); database.during_load = [&] { revoked.online.invalidate_account("owner"); };
    revoked.run({catalog_request()}); error(revoked, Error::credentials_changed, 1);
    Worker interrupted(Role::primary); interrupted.stream.fail_after = 4;
    interrupted.run({catalog_request()});
    require(interrupted.stream.sent.size() == 4 && interrupted.available_hero_ids.empty(), "catalog cannot cache allowlist before end is sent");
}
void secondary_catalog() {
    Worker game;
    require(game.online.set_game_hero_select(game.primary_binding, 308), "fixture primary-selected hero denied in next directory");
    const auto selected = game.online.hero_select_command_snapshot(game.member.binding);
    game.available_hero_ids = {987};
    const auto expiry = Clock::now() + std::chrono::seconds(1);
    game.available_heroes_expires = expiry;
    game.run({catalog_request()});
    require(database.catalog_calls == 1 && database.account == "owner" && game.stream.sent.size() == 5,
        "authenticated secondary receives its own full catalog");
    frame(game.stream.sent.front(), Type::hero_permissions_begin, {7,3,2});
    frame(game.stream.sent.back(), Type::hero_permissions_end, {7});
    require(game.available_hero_ids == std::vector<uint32_t>({987}) && game.available_heroes_expires == expiry,
        "secondary directory never updates primary selection authorization cache");
    require(game.online.hero_select_command_snapshot(game.member.binding).hero_id == 308 &&
        game.online.hero_select_command_snapshot(game.member.binding).revision == selected.revision,
        "secondary directory cannot revoke or change primary selection");
    game.stream.sent.clear(); game.run({catalog_request(8)});
    require(database.calls == 1 && game.stream.sent.size() == 1, "secondary directory keeps 350ms query throttle");
    frame(game.stream.sent.front(), Type::hero_permissions_result, {8,uint32_t(Result::rate_limited)});
    game.stream.sent.clear(); game.next_hero_permissions_request = {}; database.available = false;
    game.run({catalog_request(9)});
    require(game.stream.sent.size() == 1 && game.available_hero_ids == std::vector<uint32_t>({987}) &&
        game.available_heroes_expires == expiry && game.online.hero_select_command_snapshot(game.member.binding).revision == selected.revision,
        "failed secondary directory never changes primary state or cache");
    frame(game.stream.sent.front(), Type::hero_permissions_result, {9,uint32_t(Result::unavailable)});

    Worker before;
    before.online.invalidate_account("owner"); before.run({catalog_request()}); error(before, Error::credentials_changed, 0);
    Worker during;
    database.during_load = [&] { during.online.invalidate_account("owner"); };
    during.run({catalog_request()}); error(during, Error::credentials_changed, 1);
    Worker replaced;
    database.during_load = [&] { replaced.online.leave(replaced.member.binding);
        replaced.online.enter("owner", 123, Role::secondary, "Replacement"); };
    replaced.run({catalog_request()}); error(replaced, Error::credentials_changed, 1);
    Worker forged; auto packet = catalog_request();
    require(packet.add_string("victim", 64), "fixture forged secondary directory account");
    forged.run({packet}); error(forged, Error::protocol, 0);
}
}
int main() {
    try {
        successful_roles();
        rejected_requests();
        session_revocation();
        available_heroes();
        hero_catalog();
        secondary_catalog();
        std::cout << "PASS: " << checks << " server permission checks (offline)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL after " << checks << " checks: " << error.what() << '\n';
        return 1;
    }
}
