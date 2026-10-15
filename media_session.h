#pragma once
#include "MySQL.h"
#include "chat_room.h"
#include "social_diagnostics.h"
#include "transfer.h"
#include "network/media_codec.h"
#include "network/avatar_codec.h"
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <thread>
#include <utility>
#include <vector>

namespace chh::server {
// Process-wide media limits. Each connection has at most one upload and one
// download. Normalizing a 16 MP image needs about 150 MiB, so at most two run at
// once. Transfer buffers (whole images, up to 15 MiB each) count against a byte
// budget of about eight full-size images per direction; the budgets are separate
// so slow or stalled uploads can never keep images from being downloaded.
// A refused request gets MediaResult::busy.
constexpr unsigned media_upload_limit=16, media_download_limit=16, media_decoder_limit=2;
constexpr size_t media_buffer_budget=128ull*1024*1024;
inline std::atomic<unsigned> media_uploads_active{0}, media_downloads_active{0}, media_decoders_active{0};
inline std::atomic<size_t> media_upload_bytes{0}, media_download_bytes{0};
// The console hint for a database that still has the pre-original-size limit.
inline ThrottledReport media_upgrade_report{"图片升级未完成：","请执行media_original_migration.sql"};
// Images being served from the database right now, process-wide. The collector
// postpones them (a download in progress is never collected).
inline std::mutex media_flight_mutex;
inline std::map<std::string,unsigned> media_flight;
inline bool media_downloading(const std::string& hash) {
    std::lock_guard<std::mutex> lock(media_flight_mutex);
    return media_flight.count(hash)!=0;
}
// One connection's entry in media_flight, held until its download ends.
class MediaFlight {
public:
    MediaFlight() = default;
    MediaFlight(const MediaFlight&) = delete;
    MediaFlight& operator=(const MediaFlight&) = delete;
    ~MediaFlight() { release(); }
    void hold(const std::string& hash) {
        release();
        std::lock_guard<std::mutex> lock(media_flight_mutex);
        ++media_flight[hash]; hash_=hash;
    }
    void release() {
        if (hash_.empty()) return;
        std::lock_guard<std::mutex> lock(media_flight_mutex);
        const auto found=media_flight.find(hash_);
        if (found!=media_flight.end() && --found->second==0) media_flight.erase(found);
        hash_.clear();
    }
private:
    std::string hash_;
};
class MediaSlot {
public:
    MediaSlot() = default;
    MediaSlot(const MediaSlot&) = delete;
    MediaSlot& operator=(const MediaSlot&) = delete;
    ~MediaSlot() { release(); }
    bool acquire(std::atomic<unsigned>& counter, unsigned maximum) {
        if (counter_) return false;
        auto active=counter.load();
        while (active<maximum) if (counter.compare_exchange_weak(active,active+1)) { counter_=&counter; return true; }
        return false;
    }
    // Waits up to `patience` for a slot, so a finished 15 MiB upload is not
    // thrown away just because two other normalizations are running.
    bool acquire(std::atomic<unsigned>& counter, unsigned maximum, std::chrono::milliseconds patience) {
        const auto until=std::chrono::steady_clock::now()+patience;
        while (!acquire(counter,maximum)) {
            if (counter_ || std::chrono::steady_clock::now()>=until) return false;
            std::this_thread::sleep_for(std::chrono::milliseconds(20));
        }
        return true;
    }
    void release() { if (counter_) { --*counter_; counter_=nullptr; } }
private:
    std::atomic<unsigned>* counter_=nullptr;
};
// One connection's share of a byte budget (media_upload_bytes or media_download_bytes).
class MediaBytes {
public:
    explicit MediaBytes(std::atomic<size_t>& counter) : counter_(counter) {}
    MediaBytes(const MediaBytes&) = delete;
    MediaBytes& operator=(const MediaBytes&) = delete;
    MediaBytes(MediaBytes&& other) noexcept : counter_(other.counter_), held_(std::exchange(other.held_,0)) {}
    MediaBytes& operator=(MediaBytes&&) = delete;
    ~MediaBytes() { release(); }
    bool reserve(size_t bytes) {
        release();
        auto used=counter_.load();
        while (bytes<=media_buffer_budget && used<=media_buffer_budget-bytes)
            if (counter_.compare_exchange_weak(used,used+bytes)) { held_=bytes; return true; }
        return false;
    }
    void shrink(size_t bytes) { if (bytes<held_) { counter_-=held_-bytes; held_=bytes; } }
    void release() { if (held_) { counter_-=held_; held_=0; } }
private:
    std::atomic<size_t>& counter_;
    size_t held_=0;
};

// Owned by one serve worker. Shared counters contain no socket or image data.
class MediaSession {
public:
    using Clock=std::chrono::steady_clock;
    using Code=wire::MediaResult;
    enum class Dispatch { unrelated, handled, failed, protocol_error };
    explicit MediaSession(std::string actor) : actor_(std::move(actor)) {}
    ~MediaSession() { clear_upload(); clear_download(); clear_staged(); }
    void reset_chat() { chat_.clear(); }
    void observe_chat(const ChatRoom::Message& message) {
        chat_[message.id]={message.image_hash,false,true};
        while (chat_.size()>wire::chat_history_limit) chat_.erase(chat_.begin());
    }
    void reset_private(const std::string& peer) {
        if (peer_==peer) return;
        peer_=peer; private_.clear(); private_order_.clear(); next_private_={};
    }
    void observe_private(const PrivateMessage& message, const std::string& peer) {
        reset_private(peer);
        if (message.recalled || message.text!="[图片]") {
            private_.erase(message.id);
            private_order_.erase(std::remove(private_order_.begin(), private_order_.end(), message.id), private_order_.end());
            return;
        }
        if (!private_.count(message.id)) private_order_.push_back(message.id);
        private_[message.id]={};
        while (private_order_.size()>100) { private_.erase(private_order_.front()); private_order_.pop_front(); }
    }
    bool pending() const { return download_.request()!=0 || metadata_pending_ || favorites_active_; }
    template<class Public, class Send> bool tick(const std::string& current_peer, uint64_t social_revision,
        Public lobby_visible, Send send) {
        if (!joined_) return true;
        const auto now=Clock::now();
        reset_private(current_peer);
        if (staged_.bytes && now>=staged_.until) clear_staged();
        if (upload_.expired(now)) {
            const auto request=upload_.request(); clear_upload(); if (!result(send,request,Code::invalid)) return false;
        }
        // A reader too slow for media_transfer_seconds gives the buffer back; the
        // client handles the result like any refused download and asks again later.
        if (download_.request() && now>=download_deadline_) {
            const auto request=download_.request(); clear_download(); if (!result(send,request,Code::busy)) return false;
        }
        if (now>=next_refresh_) {
            next_refresh_=now+std::chrono::seconds(1);
            if (!favorites_active_ && (!ready_ || favorites_revision_!=favorites_changes_ || refresh_requested_))
                if (!refresh(send)) return false;
        }
        if (!flush_favorites(send)) return false;
        if (ready_ && !favorites_active_ && !peer_.empty() && now>=next_private_) {
            next_private_=now+std::chrono::seconds(1);
            std::vector<uint64_t> ids;
            for (const auto& [id,entry]:private_) if (!entry.checked) ids.push_back(id);
            if (!ids.empty()) {
                std::vector<PrivateImage> images; std::string diagnostic;
                const auto code=private_images(actor_,peer_,ids,images,&diagnostic);
                if (code==Code::ok) {
                    for (auto id:ids) private_[id].checked=true;
                    for (const auto& image:images) private_[image.message_id].hash=image.hash;
                }
                // A vanished peer account or a pair that is no longer valid only
                // loses its image mappings; it is not a database outage.
                else if (code==Code::not_found || code==Code::not_friend || code==Code::invalid) { private_.clear(); private_order_.clear(); }
                else if (!unavailable(send,diagnostic)) return false;
            }
        }
        if (!flush_mappings(send)) return false;
        // A download from the lobby's memory needs no database access while a
        // recent lobby message references the image. Once none does (recall,
        // eviction), the account's own access decides, as for a database
        // download: a private image this account may fetch (the same file was
        // also sent privately, or favourited) goes on from the bytes already
        // held; anything else ends with that answer (not_found for an image
        // that exists only in the lobby's memory).
        if (download_.request() && download_memory_) {
            if (!lobby_visible(download_hash_)) {
                std::string diagnostic;
                const auto code=authorize_media(actor_,download_hash_,false,&diagnostic);
                if (code!=Code::ok) {
                    const auto request=download_.request(); clear_download();
                    if (code==Code::unavailable) report_(diagnostic);
                    if (!result(send,request,code)) return false;
                } else {
                    // From now on a database download: re-checked like one, and
                    // never collected while it runs.
                    download_memory_=false; download_lobby_visible_=false; flight_.hold(download_hash_);
                    download_social_revision_=social_revision; download_favorites_revision_=favorites_changes_;
                }
            }
        } else if (download_.request() && (download_social_revision_!=social_revision ||
            download_favorites_revision_!=favorites_changes_ ||
            (download_lobby_visible_ && !lobby_visible(download_hash_)))) {
            std::string diagnostic; const bool visible=lobby_visible(download_hash_);
            const auto code=authorize_media(actor_,download_hash_,visible,&diagnostic);
            if (code!=Code::ok) {
                const auto request=download_.request(); clear_download();
                if (code==Code::unavailable) report_(diagnostic);
                if (!result(send,request,code)) return false;
            } else {
                download_social_revision_=social_revision; download_favorites_revision_=favorites_changes_;
                download_lobby_visible_=visible;
            }
        }
        bool finished=false;
        // Up to 1 MiB or 20 ms per round: 15 MiB leave localhost in about 0.3 s.
        if (!download_.pump(wire::Type::media_data_chunk,wire::Type::media_data_end,send,finished,1024,std::chrono::milliseconds(20))) return false;
        if (finished) clear_download();
        return true;
    }

    // lobby_bytes(hash): the lobby's in-memory copy while this session has
    // joined the lobby and a recent message references it, else null.
    // publish_chat(hash, bytes): ok, too_large (over the lobby memory cap) or unavailable.
    template<class Public, class Publish, class LobbyBytes, class Saved, class Send>
    Dispatch handle(const wire::Packet& packet, bool primary, bool chat_joined, bool social_joined,
        uint64_t social_revision, ChatRateLimit& chat_rate, ChatRateLimit& private_rate,
        Public lobby_visible, Publish publish_chat, LobbyBytes lobby_bytes, Saved saved_private, Send send) {
        const auto type=packet.type();
        if (uint32_t(type)<uint32_t(wire::Type::media_join) || uint32_t(type)>uint32_t(wire::Type::favorite_action)) return Dispatch::unrelated;
        if (!primary) return Dispatch::protocol_error;
        const auto delivered=[](bool good){ return good?Dispatch::handled:Dispatch::failed; };
        if (type==wire::Type::media_join || type==wire::Type::favorites_list) {
            if (packet.length() || (type==wire::Type::favorites_list && !joined_)) return Dispatch::protocol_error;
            joined_=true; refresh_requested_=true;
            if (type==wire::Type::media_join) {
                ready_=false; failure_sent_=false;
                for (auto& [id,entry]:chat_) { (void)id; entry.sent=false; }
                for (auto& [id,entry]:private_) { (void)id; entry.sent=false; }
            }
            return Dispatch::handled;
        }
        if (!joined_) return Dispatch::protocol_error;
        wire::Reader body{packet}; uint32_t request=0;
        if (!body.number(request) || !request) return Dispatch::protocol_error;
        if (type==wire::Type::media_upload_begin) {
            uint32_t total=0; if (!body.number(total) || !body.done()) return Dispatch::protocol_error;
            clear_staged();
            if (!total) return delivered(result(send,request,Code::invalid));
            if (total>wire::media_size_limit) return delivered(result(send,request,Code::too_large));
            if (!ready_) return delivered(result(send,request,Code::unavailable));
            const auto now=Clock::now();
            if (upload_.request() || now<next_upload_ || !upload_slot_.acquire(media_uploads_active,media_upload_limit))
                return delivered(result(send,request,Code::busy));
            if (!upload_bytes_.reserve(total)) { upload_slot_.release(); return delivered(result(send,request,Code::busy)); }
            next_upload_=now+std::chrono::seconds(2);
            upload_.begin(request,total,now+std::chrono::seconds(wire::media_transfer_seconds(total)));
            wire::Packet reply(wire::Type::media_upload_ready); reply.add_number(request); return delivered(send(reply));
        }
        if (type==wire::Type::media_upload_chunk) {
            uint32_t offset=0; if (!body.number(offset)) return Dispatch::protocol_error;
            const size_t count=packet.length()-body.offset;
            if (!count || count>wire::avatar_chunk_limit) return Dispatch::protocol_error;
            const auto step=upload_.append(request,offset,packet.bytes.data()+8+body.offset,count);
            if (step==ChunkedUpload::Step::accepted || step==ChunkedUpload::Step::retired) return Dispatch::handled;
            if (step==ChunkedUpload::Step::failed) clear_upload();
            return delivered(result(send,request,Code::invalid));
        }
        if (type==wire::Type::media_upload_commit) {
            if (!body.done()) return Dispatch::protocol_error;
            const auto step=upload_.complete(request);
            if (step==ChunkedUpload::Step::retired) return Dispatch::handled;
            if (step!=ChunkedUpload::Step::accepted) {
                if (step==ChunkedUpload::Step::failed) clear_upload();
                return delivered(result(send,request,Code::invalid));
            }
            // End normalization before sending a result: its temporary buffers
            // keep a decoder slot until destroyed or charged to the staged budget.
            const auto code=[&] {
                MediaSlot decoding;
                if (!decoding.acquire(media_decoders_active,media_decoder_limit,std::chrono::seconds(10))) {
                    clear_upload(); return Code::busy;
                }
                media::Normalized canonical; std::string diagnostic;
                if (!media::normalize(upload_.bytes(),canonical,diagnostic) || canonical.bytes.empty() ||
                    !wire::valid_sha256_hex(canonical.hash)) {
                    clear_upload(); return Code::invalid;
                }
                if (canonical.bytes.size()>wire::media_size_limit) { clear_upload(); return Code::too_large; }
                // The input is freed before transferring the decoder's output
                // to the staged budget; no database write happens here.
                clear_upload();
                if (!staged_bytes_.reserve(canonical.bytes.capacity())) return Code::busy;
                staged_.hash=canonical.hash;
                staged_.bytes=std::make_shared<const std::vector<uint8_t>>(std::move(canonical.bytes));
                staged_.until=Clock::now()+staged_lifetime;
                return Code::ok;
            }();
            return delivered(result(send,request,code,code==Code::ok?staged_.hash:std::string()));
        }
        if (type==wire::Type::media_get) {
            std::string hash; if (!body.string(hash,64) || !body.done()) return Dispatch::protocol_error;
            if (!wire::valid_sha256_hex(hash)) return delivered(result(send,request,Code::invalid));
            if (!ready_) return delivered(result(send,request,Code::unavailable));
            if (download_.request() || !download_tokens_.take() || !download_slot_.acquire(media_downloads_active,media_download_limit))
                return delivered(result(send,request,Code::busy));
            // A lobby image is streamed from the lobby's memory copy (shared, not
            // duplicated); it never touches the database.
            if (const ChunkedDownload::Bytes memory=lobby_bytes(hash)) {
                if (!download_bytes_.reserve(memory->capacity())) { download_slot_.release(); return delivered(result(send,request,Code::busy)); }
                wire::Packet begin(wire::Type::media_data_begin);
                if (!begin.add_number(request) || !begin.add_string(hash,64) || !begin.add_number(static_cast<uint32_t>(memory->size())) || !send(begin)) return Dispatch::failed;
                download_deadline_=Clock::now()+std::chrono::seconds(wire::media_transfer_seconds(memory->size()));
                download_.start(request,memory); download_hash_=hash; download_memory_=true;
                download_social_revision_=social_revision; download_favorites_revision_=favorites_changes_; download_lobby_visible_=true;
                return Dispatch::handled;
            }
            // The size is known only after the read: hold a full-size share
            // meanwhile, then keep only what the image needs.
            if (!download_bytes_.reserve(wire::media_size_limit)) { download_slot_.release(); return delivered(result(send,request,Code::busy)); }
            std::vector<uint8_t> candidate; std::string diagnostic,actual;
            const bool visible=lobby_visible(hash);
            flight_.hold(hash);
            auto code=load_media(actor_,hash,visible,candidate,&diagnostic);
            if (code==Code::ok && (!avatar::sha256(candidate,actual) || actual!=hash)) {
                code=Code::unavailable; diagnostic="图片内容与保存的标识不一致";
            }
            if (code!=Code::ok) {
                std::vector<uint8_t>().swap(candidate);
                flight_.release(); download_slot_.release(); download_bytes_.release(); if (code==Code::unavailable) report_(diagnostic);
                return delivered(result(send,request,code));
            }
            download_bytes_.shrink(candidate.capacity());
            wire::Packet begin(wire::Type::media_data_begin);
            if (!begin.add_number(request) || !begin.add_string(hash,64) || !begin.add_number(static_cast<uint32_t>(candidate.size())) || !send(begin)) return Dispatch::failed;
            download_deadline_=Clock::now()+std::chrono::seconds(wire::media_transfer_seconds(candidate.size()));
            download_.start(request,std::move(candidate)); download_hash_=hash;
            download_social_revision_=social_revision; download_favorites_revision_=favorites_changes_; download_lobby_visible_=visible;
            return Dispatch::handled;
        }
        if (type==wire::Type::chat_image_send) {
            std::string hash; if (!chat_joined || !body.string(hash,64) || !body.done()) return Dispatch::protocol_error;
            if (!wire::valid_sha256_hex(hash)) return delivered(result(send,request,Code::invalid));
            if (!ready_) return delivered(result(send,request,Code::unavailable));
            const auto now=Clock::now(); if (!chat_rate.ready(now)) return delivered(result(send,request,Code::busy));
            // A fresh upload goes into lobby memory and never into the database;
            // an image still in memory is posted again from there; anything
            // else (a favourite used as a sticker) is served from the database.
            std::string diagnostic; auto code=Code::ok;
            auto staged=take_staged(hash);
            ChunkedDownload::Bytes bytes=staged.bytes;
            if (!bytes && !lobby_bytes(hash)) code=authorize_media(actor_,hash,lobby_visible(hash),&diagnostic);
            if (code==Code::ok) {
                code=publish_chat(hash,bytes);
                if (code==Code::ok) chat_rate.accepted(now);
            }
            if (code==Code::unavailable) report_(diagnostic);
            return delivered(result(send,request,code,code==Code::ok?hash:std::string()));
        }
        if (type==wire::Type::private_image_send) {
            std::string peer,hash,nonce;
            if (!social_joined || !body.string(peer,64) || !body.string(hash,64) || !body.string(nonce,32) || !body.done()) return Dispatch::protocol_error;
            if (!wire::valid_sha256_hex(hash) || !wire::valid_nonce_hex(nonce))
                return delivered(result(send,request,Code::invalid));
            if (!ready_) return delivered(result(send,request,Code::unavailable));
            std::string diagnostic; uint64_t id=0; uint32_t timestamp=0; bool inserted=false; const auto now=Clock::now();
            // A private image enters the database only here: the staged upload,
            // or a lobby image forwarded from memory. The upload row keeps it
            // for the grace period until the message below holds its right.
            auto staged=take_staged(hash);
            ChunkedDownload::Bytes bytes=staged.bytes;
            if (!bytes && lobby_visible(hash)) bytes=lobby_bytes(hash);
            if (bytes) {
                const auto stored=store_media(actor_,hash,*bytes,&diagnostic);
                if (stored!=Code::ok) {
                    if (stored==Code::unavailable) report_(diagnostic);
                    if (stored==Code::upgrade_pending) media_upgrade_report(diagnostic);
                    return delivered(result(send,request,stored));
                }
            }
            const auto code=send_private_image(actor_,peer,hash,nonce,lobby_visible(hash),private_rate.ready(now),id,timestamp,inserted,&diagnostic);
            if (code==Code::ok && inserted) private_rate.accepted(now);
            // The caller may write private_stored, which must precede the result.
            if (code==Code::ok && !saved_private(request,peer,nonce,id,timestamp,inserted)) return Dispatch::failed;
            if (code==Code::unavailable) report_(diagnostic);
            return delivered(result(send,request,code,code==Code::ok?hash:std::string()));
        }
        if (type==wire::Type::favorite_action) {
            uint32_t operation=0; std::string hash;
            if (!body.number(operation) || !body.string(hash,64) || !body.done()) return Dispatch::protocol_error;
            if ((operation!=1 && operation!=2) || !wire::valid_sha256_hex(hash)) return delivered(result(send,request,Code::invalid));
            if (!ready_) return delivered(result(send,request,Code::unavailable));
            const auto now=Clock::now(); if (now<next_favorite_) return delivered(result(send,request,Code::busy));
            next_favorite_=now+std::chrono::milliseconds(500);
            std::string diagnostic;
            // Favouriting an image the server holds only in memory (a staged
            // upload, a lobby image) copies it into the database with the favourite.
            auto staged=take_staged(hash);
            ChunkedDownload::Bytes bytes=staged.bytes;
            if (operation!=1) bytes=nullptr;
            else if (!bytes) bytes=lobby_bytes(hash);
            const auto code=change_media_favorite(actor_,hash,operation==1,lobby_visible(hash),&diagnostic,bytes.get());
            // Favorites only grant access to their owner, and an account has a
            // single primary session, so only this session must re-check.
            if (code==Code::ok) { ++favorites_changes_; refresh_requested_=true; next_refresh_={}; }
            if (code==Code::ok && operation==2) media_collector_candidate(hash);
            if (code==Code::unavailable) report_(diagnostic);
            if (code==Code::upgrade_pending) media_upgrade_report(diagnostic);
            return delivered(result(send,request,code,code==Code::ok?hash:std::string()));
        }
        return Dispatch::protocol_error;
    }
private:
    struct Mapping { std::string hash; bool sent=false,checked=false; };
    template<class Send> static bool result(Send& send,uint32_t request,Code code,const std::string& hash={}) {
        return send_transfer_result(send,wire::Type::media_result,request,code,hash);
    }
    // An uploaded image waits here for the request that uses it (at most once).
    static constexpr std::chrono::seconds staged_lifetime{120};
    struct Staged { std::string hash; ChunkedDownload::Bytes bytes; Clock::time_point until{}; };
    struct StagedUse {
        // Destruction releases the image first, then its reservation. A successful
        // lobby publish has meanwhile transferred it to the lobby's own budget.
        MediaBytes budget;
        ChunkedDownload::Bytes bytes;
        StagedUse() : budget(media_upload_bytes) {}
        StagedUse(MediaBytes&& held, ChunkedDownload::Bytes image) noexcept :
            budget(std::move(held)),bytes(std::move(image)) {}
    };
    StagedUse take_staged(const std::string& hash) {
        if (!staged_.bytes || staged_.hash!=hash) return {};
        StagedUse used{std::move(staged_bytes_),std::move(staged_.bytes)};
        clear_staged(); return used;
    }
    void clear_staged() { staged_={}; staged_bytes_.release(); }
    static void media_collector_candidate(const std::string& hash);
    void clear_upload() { upload_.clear(); upload_slot_.release(); upload_bytes_.release(); }
    void clear_download() {
        download_.clear(); download_hash_.clear(); download_memory_=false; flight_.release();
        download_slot_.release(); download_bytes_.release();
    }
    template<class Send> bool unavailable(Send& send,const std::string& diagnostic) {
        report_(diagnostic); ready_=false; next_refresh_=Clock::now()+std::chrono::seconds(5);
        if (failure_sent_) return true;
        failure_sent_=true; return result(send,0,Code::unavailable);
    }
    template<class Send> bool refresh(Send& send) {
        std::vector<std::string> favorites; std::string diagnostic;
        const auto revision=favorites_changes_;
        if (list_media_favorites(actor_,favorites,&diagnostic)!=Code::ok) return unavailable(send,diagnostic);
        if (!ready_ && !send(wire::Packet(wire::Type::media_ready))) return false;
        favorites_=std::move(favorites); favorites_offset_=0; favorites_active_=true; favorites_begin_sent_=false;
        ready_=true; failure_sent_=false; refresh_requested_=false; favorites_revision_=revision; return true;
    }
    template<class Send> bool flush_favorites(Send& send) {
        if (!favorites_active_) return true;
        size_t frames=0;
        if (!favorites_begin_sent_) {
            wire::Packet begin(wire::Type::favorites_begin); begin.add_number(static_cast<uint32_t>(favorites_.size()));
            if (!send(begin)) return false;
            favorites_begin_sent_=true; ++frames;
        }
        while (favorites_offset_<favorites_.size() && frames<16) {
            wire::Packet entry(wire::Type::favorite_entry);
            if (!entry.add_string(favorites_[favorites_offset_],64) || !send(entry)) return false;
            ++favorites_offset_; ++frames;
        }
        if (favorites_offset_==favorites_.size() && frames<16) {
            if (!send(wire::Packet(wire::Type::favorites_end))) return false;
            favorites_active_=false; favorites_.clear();
        }
        return true;
    }
    template<class Send> bool flush_mappings(Send& send) {
        metadata_pending_=false; if (!ready_) return true;
        size_t count=0;
        for (auto& [id,entry]:chat_) {
            if (entry.hash.empty() || entry.sent) continue;
            if (count>=16) { metadata_pending_=true; continue; }
            wire::Packet packet(wire::Type::chat_image);
            if (!packet.add_u64(id) || !packet.add_string(entry.hash,64) || !send(packet)) return false;
            entry.sent=true; ++count;
        }
        for (auto& [id,entry]:private_) {
            if (!entry.checked || entry.hash.empty() || entry.sent) continue;
            if (count>=16) { metadata_pending_=true; continue; }
            wire::Packet packet(wire::Type::private_image);
            if (!packet.add_u64(id) || !packet.add_string(peer_,64) || !packet.add_string(entry.hash,64) || !send(packet)) return false;
            entry.sent=true; ++count;
        }
        return true;
    }
    std::string actor_,peer_,download_hash_;
    bool joined_=false,ready_=false,refresh_requested_=false,failure_sent_=false,metadata_pending_=false;
    bool favorites_active_=false,favorites_begin_sent_=false;
    bool download_lobby_visible_=false,download_memory_=false;
    // Counts this session's own favorite changes; favorites never affect another account.
    uint64_t favorites_changes_=0;
    uint64_t favorites_revision_=UINT64_MAX,download_social_revision_=0,download_favorites_revision_=0;
    std::map<uint64_t,Mapping> chat_,private_; std::deque<uint64_t> private_order_;
    std::vector<std::string> favorites_; size_t favorites_offset_=0;
    ChunkedUpload upload_;
    ChunkedDownload download_;
    Staged staged_;
    MediaFlight flight_;
    MediaSlot upload_slot_,download_slot_;
    MediaBytes upload_bytes_{media_upload_bytes},download_bytes_{media_download_bytes},staged_bytes_{media_upload_bytes};
    TokenBucket download_tokens_{8.0,4.0};
    ThrottledReport report_{"图片功能暂时不可用：","数据库操作未完成"};
    Clock::time_point next_refresh_{},next_private_{},next_upload_{},next_favorite_{},download_deadline_{};
};

// Deletes chat images nothing retains any more (private_media_migration.sql):
// no pending right, no undelivered or legacy message link, no favourite, no
// upload inside the grace period. Candidates come from every committed
// reference removal; a slow sweep over media_assets8 finds the rest (and what
// existed before this server started). One thread; it never holds its own
// mutex during a database call and never takes an account lock.
class MediaCollector {
public:
    using Clock=std::chrono::steady_clock;
    using Retained=std::function<bool(const std::string&)>;
    static constexpr size_t queue_limit=4096,round_limit=50,page_size=500;
    MediaCollector() = default;
    MediaCollector(const MediaCollector&) = delete;
    MediaCollector& operator=(const MediaCollector&) = delete;
    ~MediaCollector() { stop(); }
    // retained(hash): process state that keeps an asset (a recent lobby message
    // references it, or it is being downloaded from the database).
    void start(Retained retained) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (thread_.joinable()) return;
        retained_=std::move(retained); stopping_.store(false);
        next_sweep_=Clock::now()+std::chrono::seconds(15); next_check_={};
        thread_=std::thread([this]{ run(); });
    }
    void stop() {
        { std::lock_guard<std::mutex> lock(mutex_); stopping_.store(true); }
        wake_.notify_all();
        if (thread_.joinable()) thread_.join();
    }
    void candidate(const std::string& hash) {
        if (!wire::valid_sha256_hex(hash)) return;
        { std::lock_guard<std::mutex> lock(mutex_); add(hash); }
        wake_.notify_all();
    }
    template<class Range> void candidates(const Range& hashes) {
        bool any=false;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            for (const auto& hash:hashes) if (wire::valid_sha256_hex(hash)) { add(hash); any=true; }
        }
        if (any) wake_.notify_all();
    }
private:
    // Beyond the cap a hash is dropped; the sweep finds it again.
    void add(const std::string& hash) {
        if (candidates_.size()<queue_limit) candidates_.insert(hash);
        woken_=true;
    }
    void later(std::vector<std::pair<Clock::time_point,std::string>>& postponed) {
        for (auto& [when,hash]:postponed) if (later_.size()<queue_limit) later_.emplace(when,std::move(hash));
        postponed.clear();
    }
    void run() {
        std::unique_lock<std::mutex> lock(mutex_);
        bool enabled=false;
        std::string sweep_after; bool sweeping=false;
        std::vector<std::pair<Clock::time_point,std::string>> postponed;
        while (!stopping_) {
            wake_.wait_for(lock,std::chrono::seconds(1),[this]{ return stopping_ || woken_; });
            if (stopping_) break;
            woken_=false;
            auto now=Clock::now();
            if (now>=next_check_) {
                next_check_=now+std::chrono::seconds(60);
                lock.unlock();
                std::string diagnostic;
                const auto state=media_collection_enabled(&diagnostic);
                if (state==MediaCollection::missing) missing_report_({});
                else if (state==MediaCollection::unavailable) failure_report_(diagnostic);
                lock.lock();
                // An unreadable switch keeps the previous state; missing or paused stops deleting.
                if (state!=MediaCollection::unavailable) enabled=state==MediaCollection::enabled;
                if (stopping_) break;
            }
            // Paused: candidates wait (up to the cap), nothing is deleted.
            if (!enabled) continue;
            std::vector<std::string> due;
            for (auto it=later_.begin(); it!=later_.end() && it->first<=now && due.size()<round_limit;) {
                due.push_back(std::move(it->second)); it=later_.erase(it);
            }
            while (!candidates_.empty() && due.size()<round_limit) {
                due.push_back(*candidates_.begin()); candidates_.erase(candidates_.begin());
            }
            // A backlog is worked off at most one round (50 images) per second.
            const auto retained=retained_;
            lock.unlock();
            for (const auto& hash:due) {
                if (stopping_.load()) break;
                now=Clock::now();
                if (retained && retained(hash)) { postponed.emplace_back(now+std::chrono::seconds(30),hash); continue; }
                uint32_t wait=0; std::string diagnostic;
                switch (collect_media(hash,wait,&diagnostic)) {
                case MediaCollect::deleted: case MediaCollect::gone: case MediaCollect::kept: break;
                case MediaCollect::young: postponed.emplace_back(now+std::chrono::seconds(wait+5),hash); break;
                case MediaCollect::busy: postponed.emplace_back(now+std::chrono::seconds(10),hash); break;
                case MediaCollect::unavailable:
                    failure_report_(diagnostic); postponed.emplace_back(now+std::chrono::seconds(60),hash); break;
                }
            }
            // One sweep page per round: 15 s after start, then every 10 minutes.
            MediaCollectionPage page; bool paged=false, page_ok=false; std::string diagnostic;
            if (sweeping || now>=next_sweep_) {
                paged=true;
                page_ok=media_collection_page(sweep_after,page_size,page,&diagnostic);
            }
            lock.lock();
            later(postponed);
            if (paged) {
                if (!page_ok) {
                    failure_report_(diagnostic); sweeping=false; sweep_after.clear();
                    next_sweep_=now+std::chrono::seconds(60);
                } else {
                    for (const auto& hash:page.collectable) if (candidates_.size()<queue_limit) candidates_.insert(hash);
                    sweep_after=page.last; sweeping=page.scanned>=page_size;
                    if (!sweeping) { sweep_after.clear(); next_sweep_=now+std::chrono::minutes(10); }
                }
            }
        }
    }
    std::mutex mutex_;
    std::condition_variable wake_;
    std::thread thread_;
    Retained retained_;
    std::atomic_bool stopping_{false};
    bool woken_=false;
    std::set<std::string> candidates_;
    std::multimap<Clock::time_point,std::string> later_;
    Clock::time_point next_sweep_{},next_check_{};
    ThrottledReport missing_report_{"图片清理未启用：","请执行private_media_migration.sql"};
    ThrottledReport failure_report_{"图片清理暂时不可用：","数据库操作未完成"};
};
inline MediaCollector media_collector;
inline void MediaSession::media_collector_candidate(const std::string& hash) { media_collector.candidate(hash); }
}
