#pragma once
#include "network/protocol.h"
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <memory>
#include <utility>
#include <vector>

// Chunked image transfer state shared by the avatar and media sessions.
// Each object belongs to one serve worker; nothing here is thread-safe.
namespace chh::server {
// Allows a burst of `capacity` requests, refilled at `rate` per second.
class TokenBucket {
public:
    using Clock = std::chrono::steady_clock;
    TokenBucket(double capacity, double rate) : tokens_(capacity), capacity_(capacity), rate_(rate) {}
    bool take() {
        const auto now = Clock::now();
        tokens_ = (std::min)(capacity_, tokens_ + std::chrono::duration<double>(now - last_).count() * rate_);
        last_ = now;
        if (tokens_ < 1.0) return false;
        tokens_ -= 1.0; return true;
    }
private:
    double tokens_, capacity_, rate_;
    Clock::time_point last_ = Clock::now();
};

// One upload assembled from in-order chunks within a deadline.
class ChunkedUpload {
public:
    using Clock = std::chrono::steady_clock;
    // Clients stream the chunks without pausing, so an upload with no progress
    // for this long is abandoned and gives back its reservation early instead of
    // holding it until the (much longer) overall deadline.
    static constexpr std::chrono::seconds idle_limit{30};
    // retired: tail of an upload that already received its final result.
    // unknown: no such upload. failed: bad offset/size or past the deadline;
    // the caller clears the upload and answers invalid.
    enum class Step { accepted, retired, unknown, failed };
    uint32_t request() const { return request_; }
    bool expired(Clock::time_point now) const { return request_ && (now >= deadline_ || now - last_ >= idle_limit); }
    const std::vector<uint8_t>& bytes() const { return bytes_; }
    void begin(uint32_t request, size_t total, Clock::time_point deadline) {
        if (request == retired_) retired_ = 0;
        request_ = request; total_ = total; deadline_ = deadline; last_ = Clock::now();
        bytes_.clear(); bytes_.reserve(total);
    }
    Step append(uint32_t request, uint32_t offset, const uint8_t* data, size_t count) {
        const auto owner = match(request);
        if (owner != Step::accepted) return owner;
        const auto now = Clock::now();
        if (expired(now) || offset != bytes_.size() || count > total_ - bytes_.size()) return Step::failed;
        bytes_.insert(bytes_.end(), data, data + count);
        last_ = now;
        return Step::accepted;
    }
    // accepted once every announced byte arrived in time.
    Step complete(uint32_t request) const {
        const auto owner = match(request);
        if (owner != Step::accepted) return owner;
        return expired(Clock::now()) || bytes_.size() != total_ ? Step::failed : Step::accepted;
    }
    // TCP preserves order: the next accepted begin starts a new transfer, and
    // the tail already in flight after a final result is ignored.
    void clear() {
        retired_ = request_; request_ = 0; total_ = 0;
        std::vector<uint8_t>().swap(bytes_);
    }
private:
    Step match(uint32_t request) const {
        if (request == request_) return Step::accepted;
        return request == retired_ ? Step::retired : Step::unknown;
    }
    uint32_t request_ = 0, retired_ = 0;
    size_t total_ = 0;
    std::vector<uint8_t> bytes_;
    Clock::time_point deadline_{}, last_{};
};

// One download streamed in bounded bursts so heartbeat and chat still run.
// The bytes are shared, so a lobby image is streamed from the in-memory copy
// without duplicating it per downloader.
class ChunkedDownload {
public:
    using Bytes = std::shared_ptr<const std::vector<uint8_t>>;
    uint32_t request() const { return request_; }
    void start(uint32_t request, std::vector<uint8_t> bytes) {
        start(request, std::make_shared<const std::vector<uint8_t>>(std::move(bytes)));
    }
    void start(uint32_t request, Bytes bytes) {
        request_ = request; offset_ = 0; bytes_ = bytes ? std::move(bytes) : empty();
    }
    void clear() { request_ = 0; offset_ = 0; bytes_ = empty(); }
    // Sends at least 16 (if left) and at most max_frames chunk frames per call,
    // stopping after 16 once `budget` has passed, then the end frame; `finished`
    // reports the end frame so the caller can release its download state. The
    // budget keeps a fast link fed without starving heartbeat and chat on a slow
    // one, where each send blocks until the peer drains its window.
    template<class Send> bool pump(wire::Type chunk_type, wire::Type end_type, Send& send, bool& finished,
        size_t max_frames = 16, std::chrono::steady_clock::duration budget = {}) {
        finished = false;
        const auto started = std::chrono::steady_clock::now();
        const auto& bytes = *bytes_;
        for (size_t frames = 0; request_ && frames < max_frames; ++frames) {
            if (frames >= 16 && std::chrono::steady_clock::now() - started >= budget) break;
            if (offset_ == bytes.size()) {
                wire::Packet end(end_type); end.add_number(request_);
                if (!send(end)) return false;
                finished = true; return true;
            }
            const size_t count = (std::min)(wire::avatar_chunk_limit, bytes.size() - offset_);
            wire::Packet chunk(chunk_type);
            if (!chunk.add_number(request_) || !chunk.add_number(static_cast<uint32_t>(offset_)) ||
                !chunk.add_bytes(bytes.data() + offset_, count) || !send(chunk)) return false;
            offset_ += count;
        }
        return true;
    }
private:
    static Bytes empty() {
        static const Bytes none = std::make_shared<const std::vector<uint8_t>>();
        return none;
    }
    uint32_t request_ = 0;
    size_t offset_ = 0;
    Bytes bytes_ = empty();
};

// Result frame shared by both features: request, code, optional hash.
template<class Code, class Send>
bool send_transfer_result(Send& send, wire::Type type, uint32_t request, Code code, const std::string& hash = {}) {
    wire::Packet packet(type);
    return packet.add_number(request) && packet.add_number(uint32_t(code)) && packet.add_string(hash, 64) && send(packet);
}
}
