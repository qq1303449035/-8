#pragma once
#include "MySQL.h"
#include "chat_room.h"
#include "social_diagnostics.h"
#include "transfer.h"
#include "network/avatar_codec.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <iostream>
#include <map>
#include <set>
#include <vector>

namespace chh::server {
inline std::atomic<uint64_t> avatar_revision{0};

// One instance belongs to one serve worker. Send callbacks must use its TLS stream.
class AvatarSession {
public:
    using Clock = std::chrono::steady_clock;
    using Code = wire::AvatarResult;
    enum class Dispatch { unrelated, handled, failed, protocol_error };
    explicit AvatarSession(std::string actor) : actor_(std::move(actor)) {}

    void reset_chat() { chat_.clear(); sent_chat_.clear(); next_refresh_ = {}; }
    void observe_chat(const ChatRoom::Message& message) {
        chat_[message.id] = message.account_id;
        while (chat_.size() > wire::chat_history_limit) chat_.erase(chat_.begin());
        for (auto it = sent_chat_.begin(); it != sent_chat_.end();)
            if (!chat_.count(it->first)) it = sent_chat_.erase(it); else ++it;
    }
    bool pending() const { return download_.request() != 0 || metadata_pending_; }

    template<class Send> bool tick(const std::vector<FriendRecord>& friends, Send send) {
        if (!joined_) return true;
        const auto now = Clock::now();
        if (upload_.expired(now)) {
            const auto expired = upload_.request(); upload_.clear();
            if (!result(send, expired, Code::invalid)) return false;
        }
        if (now >= next_refresh_) {
            next_refresh_ = now + std::chrono::seconds(1);
            if (!refresh_metadata(friends, send)) return false;
        }
        if (!flush_metadata(friends, send)) return false;
        bool finished = false;
        if (!download_.pump(wire::Type::avatar_data_chunk, wire::Type::avatar_data_end, send, finished)) return false;
        if (finished) download_.clear();
        return true;
    }

    template<class Send> Dispatch handle(const wire::Packet& packet, bool primary, Send send) {
        const auto type = packet.type();
        if (uint32_t(type) < uint32_t(wire::Type::avatar_join) ||
            uint32_t(type) > uint32_t(wire::Type::avatar_upload_ready)) return Dispatch::unrelated;
        if (!primary) return Dispatch::protocol_error;
        const auto delivered = [](bool ok) { return ok ? Dispatch::handled : Dispatch::failed; };
        if (type == wire::Type::avatar_join) {
            if (packet.length()) return Dispatch::protocol_error;
            const auto now = Clock::now();
            if (now < next_join_) return delivered(result(send, 0, Code::busy));
            next_join_ = now + std::chrono::seconds(1);
            joined_ = true; next_refresh_ = {}; revision_ = UINT64_MAX;
            self_sent_ = false; sent_friends_.clear(); sent_chat_.clear(); failure_sent_ = false;
            return Dispatch::handled;
        }
        if (!joined_) return Dispatch::protocol_error;
        wire::Reader body{packet}; uint32_t request = 0;
        if (!body.number(request) || !request) return Dispatch::protocol_error;
        if (type == wire::Type::avatar_upload_begin) {
            uint32_t total = 0;
            if (!body.number(total) || !body.done()) return Dispatch::protocol_error;
            if (!total) return delivered(result(send, request, Code::invalid));
            if (total > wire::avatar_size_limit) return delivered(result(send, request, Code::too_large));
            const auto now = Clock::now();
            if (upload_.request() || now < next_upload_) return delivered(result(send, request, Code::busy));
            next_upload_ = now + std::chrono::seconds(10);
            upload_.begin(request, total, now + std::chrono::seconds(30));
            wire::Packet ready(wire::Type::avatar_upload_ready); ready.add_number(request);
            return delivered(send(ready));
        }
        if (type == wire::Type::avatar_upload_chunk) {
            uint32_t offset = 0;
            if (!body.number(offset)) return Dispatch::protocol_error;
            const size_t count = packet.length() - body.offset;
            if (!count || count > wire::avatar_chunk_limit) return Dispatch::protocol_error;
            const auto step = upload_.append(request, offset, packet.bytes.data() + 8 + body.offset, count);
            if (step == ChunkedUpload::Step::accepted || step == ChunkedUpload::Step::retired) return Dispatch::handled;
            if (step == ChunkedUpload::Step::failed) upload_.clear();
            return delivered(result(send, request, Code::invalid));
        }
        if (type == wire::Type::avatar_upload_commit) {
            if (!body.done()) return Dispatch::protocol_error;
            const auto step = upload_.complete(request);
            if (step == ChunkedUpload::Step::retired) return Dispatch::handled;
            if (step != ChunkedUpload::Step::accepted) {
                if (step == ChunkedUpload::Step::failed) upload_.clear();
                return delivered(result(send, request, Code::invalid));
            }
            std::vector<uint8_t> canonical; std::string diagnostic, hash;
            const bool normalized = avatar::normalize_png256(upload_.bytes(), canonical, diagnostic);
            upload_.clear();
            if (!normalized || canonical.empty()) return delivered(result(send, request, Code::invalid));
            if (canonical.size() > wire::avatar_size_limit) return delivered(result(send, request, Code::too_large));
            if (!avatar::sha256(canonical, hash) || !wire::valid_sha256_hex(hash)) return delivered(result(send, request, Code::unavailable));
            const auto code = store_avatar(actor_, canonical, hash, &diagnostic);
            if (code == Code::ok) { ++avatar_revision; next_refresh_ = {}; }
            else if (code == Code::unavailable) report_(diagnostic);
            return delivered(result(send, request, code, code == Code::ok ? hash : std::string()));
        }
        if (type == wire::Type::avatar_get) {
            std::string hash;
            if (!body.string(hash, 64) || !body.done()) return Dispatch::protocol_error;
            if (!wire::valid_sha256_hex(hash)) return delivered(result(send, request, Code::invalid));
            if (download_.request() || !download_tokens_.take()) return delivered(result(send, request, Code::busy));
            std::vector<uint8_t> candidate; std::string diagnostic;
            auto code = load_avatar(hash, candidate, &diagnostic);
            std::string actual;
            if (code == Code::ok && (!avatar::sha256(candidate, actual) || actual != hash)) {
                code = Code::unavailable; diagnostic = "头像内容与保存的标识不一致";
            }
            if (code != Code::ok) {
                if (code == Code::unavailable) report_(diagnostic);
                return delivered(result(send, request, code));
            }
            wire::Packet begin(wire::Type::avatar_data_begin);
            if (!begin.add_number(request) || !begin.add_string(hash, 64) ||
                !begin.add_number(static_cast<uint32_t>(candidate.size())) || !send(begin)) return Dispatch::failed;
            download_.start(request, std::move(candidate));
            return Dispatch::handled;
        }
        return Dispatch::protocol_error;
    }

private:
    template<class Send> static bool result(Send& send, uint32_t request, Code code, const std::string& hash = {}) {
        return send_transfer_result(send, wire::Type::avatar_result, request, code, hash);
    }
    template<class Send> bool refresh_metadata(const std::vector<FriendRecord>& friends, Send send) {
        std::set<std::string> scope{actor_};
        for (const auto& friend_record : friends)
            if (friend_record.relation == wire::FriendRelation::accepted) scope.insert(friend_record.id);
        for (const auto& [message, account] : chat_) { (void)message; scope.insert(account); }
        for (auto it = known_.begin(); it != known_.end();)
            if (!scope.count(it->first)) it = known_.erase(it); else ++it;
        const uint64_t revision = avatar_revision.load();
        std::vector<std::string> needed;
        for (const auto& account : scope)
            if (revision != revision_ || !known_.count(account)) needed.push_back(account);
        std::map<std::string, std::string> updates;
        for (size_t start = 0; start < needed.size(); start += wire::friend_limit) {
            const size_t end = (std::min)(needed.size(), start + wire::friend_limit);
            std::vector<std::string> batch(needed.begin() + start, needed.begin() + end);
            std::vector<AvatarMetadata> metadata; std::string diagnostic;
            if (avatar_metadata(batch, metadata, &diagnostic) != Code::ok) {
                report_(diagnostic); metadata_available_ = false; self_sent_ = false;
                next_refresh_ = Clock::now() + std::chrono::seconds(5);
                if (!failure_sent_) { failure_sent_ = true; return result(send, 0, Code::unavailable); }
                return true;
            }
            for (auto& entry : metadata) updates.emplace(std::move(entry.account_id), std::move(entry.hash));
        }
        for (auto& [account, hash] : updates) known_[account] = std::move(hash);
        revision_ = revision; metadata_available_ = true; failure_sent_ = false; return true;
    }
    template<class Send> bool flush_metadata(const std::vector<FriendRecord>& friends, Send send) {
        metadata_pending_ = false;
        if (!metadata_available_) return true;
        size_t frames = 0;
        const auto own = known_.find(actor_);
        if (own != known_.end() && (!self_sent_ || self_hash_ != own->second)) {
            wire::Packet packet(wire::Type::avatar_self);
            if (!packet.add_string(own->second, 64) || !send(packet)) return false;
            self_sent_ = true; self_hash_ = own->second; ++frames;
        }
        std::set<std::string> accepted;
        for (const auto& entry : friends) if (entry.relation == wire::FriendRelation::accepted) {
            accepted.insert(entry.id);
            const auto known = known_.find(entry.id), sent = sent_friends_.find(entry.id);
            if (known == known_.end() || (sent != sent_friends_.end() && sent->second == known->second)) continue;
            if (frames >= 16) { metadata_pending_ = true; continue; }
            wire::Packet packet(wire::Type::avatar_friend);
            if (!packet.add_string(entry.id, 64) || !packet.add_string(known->second, 64) || !send(packet)) return false;
            sent_friends_[entry.id] = known->second; ++frames;
        }
        for (auto it = sent_friends_.begin(); it != sent_friends_.end();)
            if (!accepted.count(it->first)) it = sent_friends_.erase(it); else ++it;
        for (const auto& [message, account] : chat_) {
            const auto known = known_.find(account); const auto sent = sent_chat_.find(message);
            if (known == known_.end() || (sent != sent_chat_.end() && sent->second == known->second)) continue;
            if (frames >= 16) { metadata_pending_ = true; continue; }
            wire::Packet packet(wire::Type::avatar_chat);
            if (!packet.add_u64(message) || !packet.add_string(known->second, 64) || !send(packet)) return false;
            sent_chat_[message] = known->second; ++frames;
        }
        return true;
    }
    std::string actor_, self_hash_;
    bool joined_ = false, self_sent_ = false, metadata_available_ = false, failure_sent_ = false;
    bool metadata_pending_ = false;
    uint64_t revision_ = UINT64_MAX;
    std::map<std::string, std::string> known_, sent_friends_;
    std::map<uint64_t, std::string> chat_, sent_chat_;
    Clock::time_point next_refresh_{}, next_join_{}, next_upload_{};
    ChunkedUpload upload_;
    ChunkedDownload download_;
    TokenBucket download_tokens_{20.0, 20.0};
    ThrottledReport report_{"头像功能暂时不可用：", "数据库操作未完成"};
};
}
