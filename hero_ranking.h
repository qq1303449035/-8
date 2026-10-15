#pragma once
#include "MySQL.h"
#include <algorithm>
#include <array>
#include <chrono>
#include <memory>
#include <mutex>
#include <numeric>
#include <set>
#include <string>
#include <unordered_set>
#include <vector>

namespace chh::server {
struct HeroRankRow {
    uint32_t rank = 0, hero_id = 0;
    std::string name, account_id, permissions, avatar_hash;
    bool hide_account_id = false;
    uint64_t uses = 0, wins = 0, losses = 0, online_seconds = 0;
};
struct HeroRankPage {
    uint32_t mode = 0, sort = 0, page = 0, total = 0, matched = 0;
    uint64_t updated_at = 0;
    std::string query;
    std::vector<HeroRankRow> rows;
};
// Clamps a requested page to the last page of `matched` results (page 0 when
// there are none); [first, end) indexes the results shown on that page.
// Shared by the ranking pages and the game-note list.
struct PageWindow { uint32_t page = 0; size_t first = 0, end = 0; };
inline PageWindow page_window(size_t matched, uint32_t requested, size_t page_size) {
    const size_t last = matched ? (matched - 1) / page_size : 0;
    PageWindow window;
    window.page = static_cast<uint32_t>((std::min)(static_cast<size_t>(requested), last));
    window.first = static_cast<size_t>(window.page) * page_size;
    window.end = (std::min)(matched, window.first + page_size);
    return window;
}
struct HeroRankSnapshot {
    uint64_t updated_at = 0;
    std::vector<HeroUsage> heroes;
    std::vector<std::string> folded_names;
    std::array<std::vector<size_t>, 4> orders;
    static constexpr const char* invalid_diagnostic = "英雄排行榜数据超出范围或英雄编号重复";
    static constexpr const char* failure_diagnostic = "无法生成英雄排行榜快照";
};
inline uint64_t hero_uses(const HeroUsage& hero, uint32_t mode) { return mode == 1 ? hero.arena_uses : hero.brawl_uses; }
inline uint64_t hero_wins(const HeroUsage& hero, uint32_t mode) { return mode == 1 ? hero.arena_wins : hero.brawl_wins; }
inline uint64_t hero_losses(const HeroUsage& hero, uint32_t mode) { return mode == 1 ? hero.arena_losses : hero.brawl_losses; }
inline std::string hero_ascii_lower(std::string text) {
    for (auto& c : text) if (c >= 'A' && c <= 'Z') c = static_cast<char>(c + ('a' - 'A'));
    return text;
}

inline bool prepare_hero_ranks(HeroRankSnapshot& snapshot) {
    if (snapshot.heroes.size() > wire::hero_rank_limit) return false;
    std::unordered_set<uint32_t> ids;
    ids.reserve(snapshot.heroes.size());
    snapshot.folded_names.clear();
    snapshot.folded_names.reserve(snapshot.heroes.size());
    for (const auto& hero : snapshot.heroes) {
        if (hero.hero_id > INT32_MAX || hero.name.empty() || hero.name.size() > wire::hero_name_limit || !wire::valid_text(hero.name) ||
            !ids.insert(hero.hero_id).second || hero.arena_uses > INT32_MAX || hero.brawl_uses > INT32_MAX ||
            hero.arena_wins > INT32_MAX || hero.arena_losses > INT32_MAX ||
            hero.brawl_wins > INT32_MAX || hero.brawl_losses > INT32_MAX) return false;
        snapshot.folded_names.push_back(hero_ascii_lower(hero.name));
    }
    for (uint32_t mode = 1; mode <= 2; ++mode) for (uint32_t sort = 1; sort <= 2; ++sort) {
        auto& order = snapshot.orders[(mode - 1) * 2 + sort - 1];
        order.resize(snapshot.heroes.size()); std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            const auto& a = snapshot.heroes[left]; const auto& b = snapshot.heroes[right];
            if (sort == 2) {
                const auto aw = hero_wins(a, mode), bw = hero_wins(b, mode);
                const auto ag = aw + hero_losses(a, mode), bg = bw + hero_losses(b, mode);
                if ((ag != 0) != (bg != 0)) return ag != 0;
                // Every count is <= INT32_MAX, so either product is < 2^63.
                const auto cross_a = aw * bg, cross_b = bw * ag;
                if (cross_a != cross_b) return cross_a > cross_b;
            }
            const auto au = hero_uses(a, mode), bu = hero_uses(b, mode);
            return au != bu ? au > bu : a.hero_id < b.hero_id;
        });
    }
    return true;
}
inline HeroRankPage hero_rank_page(const HeroRankSnapshot& snapshot, uint32_t mode, uint32_t sort,
    uint32_t requested_page, std::string query) {
    HeroRankPage page; page.mode = mode; page.sort = sort; page.updated_at = snapshot.updated_at;
    page.total = static_cast<uint32_t>(snapshot.heroes.size());
    page.query = wire::normalize_hero_query(std::move(query));
    const auto folded = hero_ascii_lower(page.query);
    const bool numeric = !folded.empty() && folded.find_first_not_of("0123456789") == std::string::npos;
    uint64_t numeric_id = 0; bool numeric_overflow = false;
    if (numeric) for (char c : folded) {
        numeric_id = numeric_id * 10 + static_cast<unsigned>(c - '0');
        if (numeric_id > UINT32_MAX) { numeric_overflow = true; break; }
    }
    const auto& order = snapshot.orders[(mode - 1) * 2 + sort - 1];
    std::vector<size_t> matches;
    for (size_t position = 0; position < order.size(); ++position) {
        const auto& hero = snapshot.heroes[order[position]];
        if (folded.empty() || (numeric ? !numeric_overflow && hero.hero_id == numeric_id :
            (snapshot.folded_names.size() == snapshot.heroes.size()
                ? snapshot.folded_names[order[position]] : hero_ascii_lower(hero.name)).find(folded) != std::string::npos))
            matches.push_back(position);
    }
    page.matched = static_cast<uint32_t>(matches.size());
    const auto window = page_window(matches.size(), requested_page, wire::hero_rank_page_size);
    page.page = window.page;
    for (size_t row = window.first; row < window.end; ++row) {
        const auto position = matches[row]; const auto& hero = snapshot.heroes[order[position]];
        HeroRankRow entry;
        entry.rank = static_cast<uint32_t>(position + 1); entry.hero_id = hero.hero_id; entry.name = hero.name;
        entry.uses = hero_uses(hero, mode); entry.wins = hero_wins(hero, mode); entry.losses = hero_losses(hero, mode);
        page.rows.push_back(std::move(entry));
    }
    return page;
}

// One loader serves all rank clients. Old immutable snapshots stay alive for
// complete replies, but a failed refresh never serves stale success. The
// loader fills Snapshot::*rows; prepare validates and orders the snapshot.
template<class Snapshot, auto rows, auto prepare> class RankingCache {
public:
    using Clock = std::chrono::steady_clock;
    template<class Load> std::shared_ptr<const Snapshot> get(Clock::time_point now,
        uint64_t unix_seconds, Load load, std::string* diagnostic = nullptr) {
        std::lock_guard lock(mutex_);
        if (now < next_refresh_) return available_ ? snapshot_ : nullptr;
        available_ = false; next_refresh_ = now + std::chrono::seconds(5);
        try {
            auto candidate = std::make_shared<Snapshot>();
            if (!load((*candidate).*rows, diagnostic)) return nullptr;
            if (!prepare(*candidate)) {
                if (diagnostic) *diagnostic = Snapshot::invalid_diagnostic;
                return nullptr;
            }
            candidate->updated_at = unix_seconds;
            snapshot_ = std::move(candidate); available_ = true;
            next_refresh_ = now + std::chrono::seconds(30);
            return snapshot_;
        } catch (...) {
            if (diagnostic) *diagnostic = Snapshot::failure_diagnostic;
            return nullptr;
        }
    }
private:
    std::mutex mutex_;
    Clock::time_point next_refresh_{};
    bool available_ = false;
    std::shared_ptr<const Snapshot> snapshot_;
};
using HeroRankingCache = RankingCache<HeroRankSnapshot, &HeroRankSnapshot::heroes, prepare_hero_ranks>;

struct PermissionRankSnapshot {
    uint64_t updated_at = 0;
    std::vector<PermissionRecord> records;
    std::array<std::vector<size_t>, 2> orders;
    static constexpr const char* invalid_diagnostic = "权限排行榜数据无效或超过4096条";
    static constexpr const char* failure_diagnostic = "无法生成权限排行榜快照";
};

inline bool prepare_permission_ranks(PermissionRankSnapshot& snapshot) {
    if (snapshot.records.size() > wire::hero_rank_limit) return false;
    std::unordered_set<std::string> ids;
    ids.reserve(snapshot.records.size());
    for (const auto& record : snapshot.records) {
        if (!wire::valid_text(record.account_id) || record.account_id.empty() ||
            record.account_id.size() > wire::permission_account_limit ||
            record.name.empty() || record.name.size() > wire::permission_name_limit ||
            !wire::valid_text(record.name) ||
            !wire::valid_permission_list(record.permissions) ||
            (!record.avatar_hash.empty() && !wire::valid_sha256_hex(record.avatar_hash)) ||
            !ids.insert(record.account_id).second) return false;
    }
    for (uint32_t sort = 1; sort <= 2; ++sort) {
        auto& order = snapshot.orders[sort - 1];
        order.resize(snapshot.records.size()); std::iota(order.begin(), order.end(), size_t(0));
        std::sort(order.begin(), order.end(), [&](size_t left, size_t right) {
            const auto& a = snapshot.records[left]; const auto& b = snapshot.records[right];
            if (sort == 1) {
                const auto ac = wire::permission_count(a.permissions), bc = wire::permission_count(b.permissions);
                if (ac != bc) return ac > bc;
            }
            return a.account_id < b.account_id;
        });
        // Each sort defines its own top 100; search only that published list.
        if (order.size() > wire::permission_rank_limit) order.resize(wire::permission_rank_limit);
    }
    return true;
}

inline HeroRankPage permission_rank_page(const PermissionRankSnapshot& snapshot, uint32_t sort,
    uint32_t requested_page, std::string query) {
    HeroRankPage page; page.mode = uint32_t(wire::HeroRankMode::permissions); page.sort = sort;
    page.updated_at = snapshot.updated_at;
    page.query = wire::normalize_hero_query(std::move(query));
    const auto folded = hero_ascii_lower(page.query);
    const auto& order = snapshot.orders[sort - 1];
    page.total = static_cast<uint32_t>(order.size());
    std::vector<size_t> matches;
    for (size_t position = 0; position < order.size(); ++position) {
        const auto& record = snapshot.records[order[position]];
        // A hidden ID must not be searchable; the name stays searchable, as
        // it is shown in every other view.
        if (folded.empty() || hero_ascii_lower(record.name).find(folded) != std::string::npos ||
            (!record.hide_account_id && hero_ascii_lower(record.account_id).find(folded) != std::string::npos))
            matches.push_back(position);
    }
    page.matched = static_cast<uint32_t>(matches.size());
    const auto window = page_window(matches.size(), requested_page, wire::hero_rank_page_size);
    page.page = window.page;
    for (size_t row = window.first; row < window.end; ++row) {
        const auto position = matches[row]; const auto& record = snapshot.records[order[position]];
        HeroRankRow entry;
        entry.rank = static_cast<uint32_t>(position + 1);
        entry.name = record.name; entry.account_id = record.account_id; entry.permissions = record.permissions;
        entry.avatar_hash = record.avatar_hash;
        entry.hide_account_id = record.hide_account_id;
        entry.uses = wire::permission_count(record.permissions);
        page.rows.push_back(std::move(entry));
    }
    return page;
}

using PermissionRankingCache =
    RankingCache<PermissionRankSnapshot, &PermissionRankSnapshot::records, prepare_permission_ranks>;

struct OnlineTimeRankSnapshot {
    uint64_t updated_at = 0;
    std::vector<OnlineTimeRecord> records;
    std::vector<size_t> order;
    static constexpr const char* invalid_diagnostic = "在线时间排行榜数据无效或超过100条";
    static constexpr const char* failure_diagnostic = "无法生成在线时间排行榜快照";
};

inline bool prepare_online_time_ranks(OnlineTimeRankSnapshot& snapshot) {
    if (snapshot.records.size() > wire::online_rank_limit) return false;
    std::unordered_set<std::string> ids;
    ids.reserve(snapshot.records.size());
    for (const auto& record : snapshot.records) {
        if (record.account_id.empty() || record.account_id.size() > wire::permission_account_limit ||
            !wire::valid_text(record.account_id) || record.name.empty() ||
            record.name.size() > wire::permission_name_limit || !wire::valid_text(record.name) ||
            (!record.avatar_hash.empty() && !wire::valid_sha256_hex(record.avatar_hash)) ||
            !record.online_seconds || !ids.insert(record.account_id).second) return false;
    }
    snapshot.order.resize(snapshot.records.size());
    std::iota(snapshot.order.begin(), snapshot.order.end(), size_t(0));
    std::sort(snapshot.order.begin(), snapshot.order.end(), [&](size_t left, size_t right) {
        const auto& a = snapshot.records[left]; const auto& b = snapshot.records[right];
        return a.online_seconds != b.online_seconds ? a.online_seconds > b.online_seconds :
            a.account_id < b.account_id;
    });
    return true;
}

inline HeroRankPage online_time_rank_page(const OnlineTimeRankSnapshot& snapshot,
    uint32_t requested_page, std::string query) {
    HeroRankPage page; page.mode = uint32_t(wire::HeroRankMode::online_time); page.sort = 1;
    page.updated_at = snapshot.updated_at; page.total = static_cast<uint32_t>(snapshot.records.size());
    page.query = wire::normalize_hero_query(std::move(query));
    const auto folded = hero_ascii_lower(page.query);
    std::vector<size_t> matches;
    for (size_t position = 0; position < snapshot.order.size(); ++position) {
        const auto& record = snapshot.records[snapshot.order[position]];
        if (folded.empty() || hero_ascii_lower(record.name).find(folded) != std::string::npos ||
            (!record.hide_account_id && hero_ascii_lower(record.account_id).find(folded) != std::string::npos))
            matches.push_back(position);
    }
    page.matched = static_cast<uint32_t>(matches.size());
    const auto window = page_window(matches.size(), requested_page, wire::hero_rank_page_size);
    page.page = window.page;
    for (size_t row = window.first; row < window.end; ++row) {
        const auto position = matches[row]; const auto& record = snapshot.records[snapshot.order[position]];
        HeroRankRow entry;
        entry.rank = static_cast<uint32_t>(position + 1); entry.name = record.name;
        // This public ranking does not need IDs for authenticated actions.
        // Remove hidden IDs from the response model as well as search results.
        if (!record.hide_account_id) entry.account_id = record.account_id;
        entry.avatar_hash = record.avatar_hash; entry.hide_account_id = record.hide_account_id;
        entry.online_seconds = record.online_seconds;
        page.rows.push_back(std::move(entry));
    }
    return page;
}

using OnlineTimeRankingCache =
    RankingCache<OnlineTimeRankSnapshot, &OnlineTimeRankSnapshot::records, prepare_online_time_ranks>;
}
