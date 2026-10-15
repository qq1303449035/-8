#pragma once
#include "network/protocol.h"
#include <atomic>
#include <chrono>
#include <deque>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace chh::server {
class ChatRoom {
public:
    static constexpr size_t batch_limit = 16;
    // Canonical bytes of a lobby image. They live only in this process, beside
    // the ring, and never inside Message (after() copies messages).
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    static constexpr size_t default_image_limit = 256ull * 1024 * 1024;
    struct Message {
        uint64_t id = 0;
        uint32_t timestamp = 0;
        std::string account_id, name, text;
        std::string image_hash;
        bool hide_account_id = false;
        std::shared_ptr<std::atomic_bool> visibility;
        bool recalled = false;
    };
    struct Batch {
        bool reset = false;
        uint64_t cursor = 0;
        uint64_t recall_revision = 0;
        std::vector<Message> messages;
    };
    struct Recall {
        uint64_t revision = 0;
        uint64_t id = 0;
        uint32_t timestamp = 0;
    };
    struct Visibility {
        uint64_t revision = 0;
        bool reset = false;
        std::map<std::string, bool> authors;
    };

    // Set once before the server accepts connections (CHH_LOBBY_IMAGE_MEMORY_MB).
    void set_image_limit(size_t bytes) {
        std::lock_guard lock(mutex_); image_limit_ = bytes;
    }
    bool image_fits(size_t size) const {
        std::lock_guard lock(mutex_); return size <= image_limit_;
    }
    // The in-memory copy, present only while a ring message still references
    // the hash and the memory cap has not dropped it.
    Bytes image(const std::string& hash) const {
        std::lock_guard lock(mutex_);
        const auto found = images_.find(hash);
        return found == images_.end() ? Bytes{} : found->second.bytes;
    }
    size_t image_bytes() const {
        std::lock_guard lock(mutex_); return image_bytes_;
    }

    // image: the canonical bytes of a freshly uploaded lobby image. Without it
    // (a favourite posted as a sticker) the image is served from the database.
    bool publish(const std::string& account_id, const std::string& name,
        const std::string& text, uint32_t timestamp, const std::string& image_hash = {}, bool hidden = false,
        Bytes image = nullptr) {
        if (account_id.empty() || name.empty() || name.size() > 512 ||
            !wire::valid_text(name) || !wire::valid_chat_text(text) ||
            (!image_hash.empty() && (text != "[图片]" || !wire::valid_sha256_hex(image_hash))) ||
            (image && (image_hash.empty() || image->empty()))) return false;
        Message message{0, timestamp, account_id, name, text, image_hash, hidden};
        std::lock_guard lock(mutex_);
        if (last_id_ == (std::numeric_limits<uint64_t>::max)()) return false;
        if (image && image->size() > image_limit_) return false;
        for (auto it = authors_.begin(); it != authors_.end();)
            if (it->second.expired()) it = authors_.erase(it); else ++it;
        auto& author = authors_[account_id];
        message.visibility = author.lock();
        if (!message.visibility) { message.visibility = std::make_shared<std::atomic_bool>(hidden); author = message.visibility; }
        else message.visibility->store(hidden);
        message.id = last_id_ + 1;
        if (!image_hash.empty()) {
            const auto stored = images_.find(image_hash);
            if (stored != images_.end()) stored->second.last_message = message.id;
            else if (image) {
                // Oldest images go first; their messages stay and show the
                // client's cached copy or the expired placeholder.
                while (!images_.empty() && image_bytes_ + image->size() > image_limit_) {
                    auto oldest = images_.begin();
                    for (auto it = images_.begin(); it != images_.end(); ++it)
                        if (it->second.last_message < oldest->second.last_message) oldest = it;
                    image_bytes_ -= oldest->second.bytes->size();
                    images_.erase(oldest);
                }
                images_.emplace(image_hash, Stored{std::move(image), message.id});
                image_bytes_ += images_.find(image_hash)->second.bytes->size();
            }
        }
        messages_.push_back(std::move(message));
        ++last_id_;
        if (messages_.size() > wire::chat_history_limit) {
            const auto evicted = std::move(messages_.front().image_hash);
            messages_.pop_front();
            release_image(evicted);
        }
        return true;
    }

    // A public message can only be recalled by its author. The row remains in
    // the ring so history stays ordered; clients render the recalled
    // placeholder instead.
    wire::ChatResult recall(const std::string& account_id, uint64_t id,
        Recall* event = nullptr) {
        std::lock_guard lock(mutex_);
        for (auto& message : messages_) {
            if (message.id != id) continue;
            if (message.account_id != account_id) return wire::ChatResult::recall_forbidden;
            if (message.recalled) return wire::ChatResult::recall_not_found;
            message.recalled = true;
            message.text = "[消息已撤回]";
            const auto hash = std::move(message.image_hash);
            message.image_hash.clear();
            release_image(hash);
            const Recall change{++recall_revision_, message.id, message.timestamp};
            recalls_.push_back(change);
            if (recalls_.size() > wire::chat_history_limit) recalls_.pop_front();
            if (event) *event = change;
            return wire::ChatResult::recalled;
        }
        return wire::ChatResult::recall_not_found;
    }

    // Shared visibility also covers an older history batch being sent while its
    // ring entries are evicted. Weak author entries are pruned on publication.
    // Every ring message carries the shared visibility flag (see publish), so
    // updating it covers all of this author's messages at once.
    void set_author_hidden(const std::string& account_id, bool hidden) {
        std::lock_guard lock(mutex_);
        const auto author = authors_.find(account_id);
        const auto state = author == authors_.end() ? nullptr : author->second.lock();
        if (state && state->load() == hidden) return;
        if (state) state->store(hidden);
        else {
            // No live messages: logins re-apply the stored preference, so skip
            // an event that would only repeat this author's latest recorded one.
            for (auto it = visibility_changes_.rbegin(); it != visibility_changes_.rend(); ++it) {
                if (it->author != account_id) continue;
                if (it->hidden == hidden) return;
                break;
            }
        }
        visibility_changes_.push_back({++visibility_revision_, account_id, hidden});
        if (visibility_changes_.size() > wire::chat_history_limit) visibility_changes_.pop_front();
    }
    bool author_hidden(const Message& message) const {
        return message.visibility ? message.visibility->load() : message.hide_account_id;
    }
    Visibility visibility_since(uint64_t revision) const {
        std::lock_guard lock(mutex_);
        Visibility result; result.revision = visibility_revision_;
        if (revision != visibility_revision_) {
            result.reset = revision != UINT64_MAX && !visibility_changes_.empty() && revision < visibility_changes_.front().revision - 1;
            for (const auto& message : messages_) result.authors[message.account_id] = author_hidden(message);
            for (const auto& change : visibility_changes_)
                if (revision == UINT64_MAX || change.revision > revision) result.authors[change.author] = change.hidden;
        }
        return result;
    }

    bool contains_image(const std::string& hash) const {
        std::lock_guard lock(mutex_);
        for (const auto& message : messages_) if (!message.image_hash.empty() && message.image_hash == hash) return true;
        return false;
    }

    Batch after(uint64_t cursor, bool reset = false) const {
        std::lock_guard lock(mutex_);
        Batch batch;
        batch.recall_revision = recall_revision_;
        batch.reset = reset || cursor > last_id_ ||
            (!messages_.empty() && cursor < messages_.front().id - 1);
        batch.cursor = batch.reset ? last_id_ : cursor;
        batch.messages.reserve(batch.reset ? messages_.size() : batch_limit);
        for (const auto& message : messages_) {
            if (!batch.reset && message.id <= cursor) continue;
            batch.messages.push_back(message);
            batch.cursor = message.id;
            if (!batch.reset && batch.messages.size() == batch_limit) break;
        }
        return batch;
    }

    uint64_t recall_revision() const {
        std::lock_guard lock(mutex_); return recall_revision_;
    }

    struct Recalls {
        // Events after the given revision already left the bounded ring; the
        // caller must resend history (which carries recalled flags) instead.
        bool reset = false;
        std::vector<Recall> changes;
    };
    Recalls recalls_since(uint64_t revision) const {
        std::lock_guard lock(mutex_);
        Recalls result;
        result.reset = !recalls_.empty() && revision + 1 < recalls_.front().revision;
        if (result.reset) return result;
        for (const auto& change : recalls_)
            if (change.revision > revision) result.changes.push_back(change);
        return result;
    }

private:
    struct Stored { Bytes bytes; uint64_t last_message = 0; };
    // Caller holds mutex_. Drops the bytes once no ring message references them.
    void release_image(const std::string& hash) {
        if (hash.empty()) return;
        const auto stored = images_.find(hash);
        if (stored == images_.end()) return;
        for (const auto& message : messages_) if (message.image_hash == hash) return;
        image_bytes_ -= stored->second.bytes->size();
        images_.erase(stored);
    }
    mutable std::mutex mutex_;
    std::deque<Message> messages_;
    std::map<std::string, Stored> images_;
    size_t image_bytes_ = 0, image_limit_ = default_image_limit;
    std::map<std::string, std::weak_ptr<std::atomic_bool>> authors_;
    struct VisibilityChange { uint64_t revision; std::string author; bool hidden; };
    std::deque<VisibilityChange> visibility_changes_;
    std::deque<Recall> recalls_;
    uint64_t last_id_ = 0;
    uint64_t visibility_revision_ = 0;
    uint64_t recall_revision_ = 0;
};

// Owned by one serve() worker; invalid/rejected requests do not consume a slot.
class ChatRateLimit {
public:
    using Clock = std::chrono::steady_clock;
    bool ready(Clock::time_point now) const {
        return !accepted_ || now - last_ >= std::chrono::seconds(1);
    }
    void accepted(Clock::time_point now) { last_ = now; accepted_ = true; }
private:
    bool accepted_ = false;
    Clock::time_point last_{};
};

// Validate framing separately from text so invalid content can receive an ACK
// without treating a well-framed request as a broken authenticated connection.
inline bool read_chat_request(const wire::Packet& packet, uint32_t& request_id,
    std::string& text) {
    if (!packet.valid() || packet.type() != wire::Type::chat_send || packet.length() < 6)
        return false;
    request_id = wire::read32(packet.bytes.data() + 8);
    const size_t size = (size_t(packet.bytes[12]) << 8) | packet.bytes[13];
    if (!request_id || size + 6 != packet.length()) return false;
    text.assign(reinterpret_cast<const char*>(packet.bytes.data() + 14), size);
    return true;
}

inline bool chat_message_packet(const ChatRoom::Message& message,
    const std::string& recipient_id, wire::Packet& packet, bool enhanced = false, bool preferences = false) {
    packet = wire::Packet(wire::Type::chat_message);
    // Identity comes from the authenticated publisher, never a chat request.
    std::string_view name = message.name;
    const bool include_author = enhanced && (preferences || !message.hide_account_id);
    // Optional fields are positional: account, visibility, recall. Initial
    // history precedes social_join; preserve both slots even without identity
    // disclosure when adding a recall flag to that compact layout.
    const bool include_account = include_author || preferences || message.recalled;
    const bool include_visibility = preferences || message.recalled;
    const std::string_view author = include_author ? std::string_view(message.account_id) : std::string_view{};
    if (enhanced) {
        if (name.size() > 512 || message.text.size() > wire::chat_text_limit ||
            message.account_id.empty() || message.account_id.size() > 64 || !wire::valid_text(name)) return false;
        // Keep the complete body and identity within the fixed frame. A long
        // display name may be shortened only at a UTF-8 character boundary.
        const size_t available = wire::payload_size - 24 - message.text.size() -
            (include_account ? 2 + author.size() : 0) - (include_visibility ? 4 : 0) - (message.recalled ? 4 : 0);
        if (name.size() > available) {
            size_t end = available;
            while (end && (static_cast<unsigned char>(name[end]) & 0xc0) == 0x80) --end;
            name = name.substr(0, end);
        }
    }
    return packet.add_u64(message.id) && packet.add_number(message.timestamp) &&
        packet.add_number(message.account_id == recipient_id ? 1u : 0u) &&
        packet.add_string(name, 512) &&
        packet.add_string(message.text, wire::chat_text_limit) &&
        (!include_account || packet.add_string(author, 64)) &&
        (!include_visibility || packet.add_number(message.hide_account_id ? 1u : 0u)) &&
        (!message.recalled || packet.add_number(1u));
}
}
