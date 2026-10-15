// Standalone C++17 regression test. Database calls are stubs; no sockets,
// database library, credentials or background collector are used.
// cl /nologo /std:c++17 /EHsc /utf-8 tests\media_budget_tests.cpp /link oleaut32.lib
#include "../media_session.h"
#include <cstdlib>
#include <future>
#include <new>
#include <stdexcept>

namespace allocation_probe {
std::atomic<size_t> expected_size{0};
std::atomic<void*> target{nullptr};
std::atomic_bool deleted{false};
std::atomic<size_t> upload_at_delete{0}, download_at_delete{0};
std::atomic<unsigned> decoders_at_delete{0};
void reset(size_t size = 0) {
    expected_size = 0; target = nullptr; deleted = false;
    upload_at_delete = 0; download_at_delete = 0; decoders_at_delete = 0;
    expected_size = size;
}
}
// Observe the actual vector allocation being freed, not just its owner/counter.
void* operator new(size_t size) {
    void* memory = std::malloc(size ? size : 1);
    if (!memory) throw std::bad_alloc();
    const auto expected = allocation_probe::expected_size.load();
    // MSVC adds a small alignment header to vector allocations above 4 KiB.
    if (expected && size >= expected && size-expected < 64) {
        void* empty = nullptr;
        allocation_probe::target.compare_exchange_strong(empty, memory);
    }
    return memory;
}
void operator delete(void* memory) noexcept {
    const auto allocation = reinterpret_cast<uintptr_t>(memory);
    const auto watched = reinterpret_cast<uintptr_t>(allocation_probe::target.load());
    if (memory && watched >= allocation && watched-allocation < 64) {
        allocation_probe::upload_at_delete = chh::server::media_upload_bytes.load();
        allocation_probe::download_at_delete = chh::server::media_download_bytes.load();
        allocation_probe::decoders_at_delete = chh::server::media_decoders_active.load();
        allocation_probe::expected_size = 0;
        allocation_probe::target = nullptr;
        allocation_probe::deleted = true;
    }
    std::free(memory);
}
void* operator new[](size_t size) { return ::operator new(size); }
void operator delete[](void* memory) noexcept { ::operator delete(memory); }
void operator delete(void* memory, size_t) noexcept { ::operator delete(memory); }
void operator delete[](void* memory, size_t) noexcept { ::operator delete(memory); }

namespace {
using namespace chh::server;
using Code = chh::wire::MediaResult;
using Type = chh::wire::Type;
using Packet = chh::wire::Packet;
using Bytes = ChunkedDownload::Bytes;
unsigned checks = 0;
void require(bool condition, const char* message) {
    ++checks;
    if (!condition) throw std::runtime_error(message);
}
struct FakeDatabase {
    Code operation_result = Code::ok;
    std::function<void(const std::vector<uint8_t>&)> operation;
    std::vector<uint8_t> download;
} database;
void clean() {
    require(media_upload_bytes == 0 && media_download_bytes == 0, "all byte reservations released");
    require(media_uploads_active == 0 && media_downloads_active == 0 && media_decoders_active == 0,
        "all transfer/decoder slots released");
    require(media_flight.empty(), "download collector pin released");
    allocation_probe::reset(); database = {};
}
struct Client {
    std::unique_ptr<MediaSession> session = std::make_unique<MediaSession>("actor");
    ChatRateLimit chat_rate, private_rate;
    uint32_t sequence = 0;
    Code result = Code::invalid;
    std::string hash;
    std::function<bool(const Packet&)> response;
    std::function<Code(const std::string&, const Bytes&)> publish;
    bool send(const Packet& packet) {
        if (packet.type() == Type::media_result) {
            chh::wire::Reader reader{packet}; uint32_t request = 0, code = 0;
            require(reader.number(request) && reader.number(code) && reader.string(hash,64,true) && reader.done(),
                "well-formed media response");
            result = static_cast<Code>(code);
        }
        return !response || response(packet);
    }
    MediaSession::Dispatch dispatch(const Packet& packet) {
        return session->handle(packet,true,true,true,0,chat_rate,private_rate,
            [](const std::string&) { return false; },
            [&](const std::string& value, const Bytes& bytes) { return publish ? publish(value,bytes) : Code::unavailable; },
            [](const std::string&) { return Bytes{}; },
            [](uint32_t,const std::string&,const std::string&,uint64_t,uint32_t,bool) { return true; },
            [&](const Packet& reply) { return send(reply); });
    }
    void tick() {
        require(session->tick({},0,[](const std::string&) { return false; },
            [&](const Packet& reply) { return send(reply); }), "media tick succeeds");
    }
    Client() {
        require(dispatch(Packet(Type::media_join)) == MediaSession::Dispatch::handled, "join accepted");
        tick();
    }
    uint32_t begin(size_t size) {
        const auto request = ++sequence;
        Packet packet(Type::media_upload_begin); packet.add_number(request); packet.add_number(static_cast<uint32_t>(size));
        require(dispatch(packet) == MediaSession::Dispatch::handled, "upload begin handled");
        return request;
    }
    void chunks(uint32_t request, const std::vector<uint8_t>& encoded) {
        for (size_t offset = 0; offset < encoded.size(); offset += chh::wire::avatar_chunk_limit) {
            const auto count = (std::min)(size_t(chh::wire::avatar_chunk_limit), encoded.size()-offset);
            Packet packet(Type::media_upload_chunk); packet.add_number(request); packet.add_number(static_cast<uint32_t>(offset));
            packet.add_bytes(encoded.data()+offset,count);
            require(dispatch(packet) == MediaSession::Dispatch::handled, "upload chunk accepted");
        }
    }
    void commit(uint32_t request) {
        Packet packet(Type::media_upload_commit); packet.add_number(request);
        require(dispatch(packet) == MediaSession::Dispatch::handled, "upload commit handled");
    }
    void upload(const std::vector<uint8_t>& encoded) {
        const auto request = begin(encoded.size()); chunks(request,encoded); commit(request);
        require(result == Code::ok && chh::wire::valid_sha256_hex(hash), "upload staged");
    }
    Packet use(Type type, const std::string& image_hash) {
        Packet packet(type); packet.add_number(++sequence);
        if (type == Type::private_image_send) packet.add_string("peer",64);
        if (type == Type::favorite_action) packet.add_number(1);
        packet.add_string(image_hash,64);
        if (type == Type::private_image_send) packet.add_string(std::string(32,'1'),32);
        return packet;
    }
};
std::vector<uint8_t> gif() {
    return {'G','I','F','8','9','a',1,0,1,0,0x80,0,0,0,0,0,255,255,255,
        0x2c,0,0,0,0,1,0,1,0,0,2,2,0x44,0x01,0,0x3b};
}
std::vector<uint8_t> png() {
    chh::avatar::Image image; image.width = image.height = 64;
    image.pixels.resize(64*64*4);
    uint32_t random = 12345;
    for (auto& pixel : image.pixels) { random = random*1664525u+1013904223u; pixel = static_cast<uint8_t>(random >> 24); }
    std::vector<uint8_t> encoded; std::string error;
    require(chh::media::detail::encode_png(image,encoded,error), "PNG fixture encoded through WIC");
    require(chh::media::detail::canonical_layout(encoded,64,64), "PNG fixture has canonical layout");
    return encoded;
}
void move_test() {
    std::atomic<size_t> counter{0};
    std::unique_ptr<MediaBytes> owner;
    {
        MediaBytes source(counter); require(source.reserve(media_buffer_budget), "source fills budget");
        owner = std::make_unique<MediaBytes>(std::move(source));
        source.release(); require(counter == media_buffer_budget, "moved source cannot release owner's reservation");
        MediaBytes refused(counter); require(!refused.reserve(1), "moved reservation still excludes another upload");
    }
    require(counter == media_buffer_budget, "source destruction preserves moved reservation");
    owner.reset(); require(counter == 0, "moved owner releases exactly once");
}
void consumption_test(Type operation, Code answer, bool successful_send, const std::vector<uint8_t>& encoded) {
    clean();
    {
        Client client; client.upload(encoded); const auto held = media_upload_bytes.load();
        require(held >= encoded.size(), "staged capacity is charged");
        MediaBytes filler(media_upload_bytes);
        require(filler.reserve(media_buffer_budget-held), "remaining upload budget filled");
        const auto packet = client.use(operation,client.hash);
        std::promise<void> entered, resume; const auto gate = resume.get_future().share();
        database.operation_result = answer;
        database.operation = [&](const std::vector<uint8_t>& bytes) {
            allocation_probe::target = const_cast<uint8_t*>(bytes.data());
            entered.set_value(); gate.wait();
        };
        size_t during_response = 0;
        client.response = [&](const Packet& reply) {
            if (reply.type() == Type::media_result) during_response = media_upload_bytes.load();
            return successful_send;
        };
        MediaSession::Dispatch dispatch = MediaSession::Dispatch::unrelated;
        std::exception_ptr error;
        std::thread worker([&] { try { dispatch = client.dispatch(packet); } catch (...) { error = std::current_exception(); } });
        const bool reached = entered.get_future().wait_for(std::chrono::seconds(5)) == std::future_status::ready;
        MediaBytes competing(media_upload_bytes);
        const bool admitted = competing.reserve(1);
        const auto while_blocked = media_upload_bytes.load();
        competing.release(); resume.set_value(); worker.join();
        if (error) std::rethrow_exception(error);
        require(reached, "consumer reached blocked database stub");
        require(!admitted && while_blocked == media_buffer_budget, "blocked consumer keeps full budget reserved");
        require(during_response == media_buffer_budget, "reservation survives success/failure response callback");
        require(dispatch == (successful_send ? MediaSession::Dispatch::handled : MediaSession::Dispatch::failed), "send failure propagated");
        require(client.result == answer, "database result propagated");
        require(allocation_probe::deleted && allocation_probe::upload_at_delete == media_buffer_budget,
            "consumed vector freed before reservation returned");
        require(media_upload_bytes == media_buffer_budget-held, "consumer returns only its own reservation");
        database.operation = {};
    }
    clean();
}
void lobby_test(const std::vector<uint8_t>& encoded, bool accepted) {
    clean();
    {
        Client client; client.upload(encoded); const auto held = media_upload_bytes.load();
        const auto image_hash = client.hash;
        const auto mismatch = client.use(Type::chat_image_send,std::string(64,'f'));
        require(client.dispatch(mismatch) == MediaSession::Dispatch::handled && client.result == Code::forbidden,
            "unrelated hash refused");
        require(media_upload_bytes == held, "hash mismatch preserves staged reservation");
        ChatRoom lobby;
        client.publish = [&](const std::string& hash, const Bytes& bytes) {
            require(bytes && media_upload_bytes == held, "publish callback holds image and reservation");
            allocation_probe::target = const_cast<uint8_t*>(bytes->data());
            if (!accepted) return Code::unavailable;
            return lobby.publish("actor","name","[图片]",1,hash,false,bytes) ? Code::ok : Code::unavailable;
        };
        require(client.dispatch(client.use(Type::chat_image_send,image_hash)) == MediaSession::Dispatch::handled,
            "lobby image handled");
        require(media_upload_bytes == 0, "lobby success/failure returns upload reservation");
        if (accepted) {
            require(lobby.image_bytes() == encoded.size() && bool(lobby.image(image_hash)), "lobby owns image under its separate budget");
            require(!allocation_probe::deleted, "successful lobby handoff keeps vector alive");
        } else require(allocation_probe::deleted && allocation_probe::upload_at_delete == held,
            "failed lobby handoff frees vector before budget");
        allocation_probe::reset();
    }
    clean();
}
void normalization_failure_test() {
    clean(); const auto encoded = gif();
    chh::media::Normalized normalized; std::string error;
    require(chh::media::normalize(encoded,normalized,error), "GIF fixture normalizes");
    const auto capacity = normalized.bytes.capacity();
    require(capacity > encoded.size(), "GIF fixture expands for staged-budget failure");
    {
        Client client; MediaBytes filler(media_upload_bytes);
        require(filler.reserve(media_buffer_budget-encoded.size()), "only original upload fits");
        const auto request = client.begin(encoded.size()); client.chunks(request,encoded);
        allocation_probe::reset(capacity);
        bool cleaned_before_response = false;
        client.response = [&](const Packet& reply) {
            if (reply.type() == Type::media_result)
                cleaned_before_response = allocation_probe::deleted && media_decoders_active == 0;
            return true;
        };
        client.commit(request);
        require(client.result == Code::busy, "normalization reports staged-budget exhaustion");
        require(cleaned_before_response && allocation_probe::decoders_at_delete == 1,
            "temporary output freed under decoder slot before failed response");
        require(media_upload_bytes == media_buffer_budget-encoded.size(), "failed normalization releases upload only");
    }
    clean();
    {
        Client client; client.upload(encoded);
        require(media_upload_bytes == capacity, "staged GIF charges capacity, not just encoded length");
    }
    clean();
}
void destruction_test(const std::vector<uint8_t>& encoded) {
    clean();
    {
        Client client; allocation_probe::reset(12345); client.begin(12345);
        require(allocation_probe::target != nullptr, "upload allocation observed");
        client.session.reset();
        require(allocation_probe::deleted && allocation_probe::upload_at_delete == 12345,
            "session destroys unfinished upload before returning budget");
    }
    clean();
    {
        Client client; const auto request = client.begin(encoded.size()); client.chunks(request,encoded);
        allocation_probe::reset(encoded.size()); client.commit(request);
        require(allocation_probe::target != nullptr && !allocation_probe::deleted, "staged allocation observed");
        const auto held = media_upload_bytes.load(); client.session.reset();
        require(allocation_probe::deleted && allocation_probe::upload_at_delete == held,
            "session destroys unconsumed staged image before returning budget");
    }
    clean();
}
void download_test(bool valid, bool disconnect) {
    clean(); database.download.assign(4096,42);
    std::string hash(64,'a');
    if (valid) require(chh::avatar::sha256(database.download,hash), "download hash computed");
    {
        Client client; bool response_clean = false;
        client.response = [&](const Packet& reply) {
            if (reply.type() == Type::media_result)
                response_clean = allocation_probe::deleted && media_download_bytes == 0 && media_downloads_active == 0;
            return true;
        };
        require(client.dispatch(client.use(Type::media_get,hash)) == MediaSession::Dispatch::handled, "download request handled");
        if (!valid) {
            require(client.result == Code::unavailable && response_clean, "corrupt download freed before failed response");
            require(allocation_probe::download_at_delete == chh::wire::media_size_limit,
                "corrupt candidate freed before full-size reservation returned");
        } else {
            require(media_download_bytes == database.download.capacity(), "download charged to actual capacity");
            if (disconnect) client.session.reset(); else client.tick();
            require(allocation_probe::deleted && allocation_probe::download_at_delete == database.download.capacity(),
                "completed/disconnected download frees vector before reservation");
        }
    }
    clean();
}
}

namespace chh::server {
wire::MediaResult list_media_favorites(const std::string&,std::vector<std::string>& out,std::string*) noexcept { out.clear(); return Code::ok; }
wire::MediaResult store_media(const std::string&,const std::string&,const std::vector<uint8_t>& bytes,std::string*) noexcept {
    if (database.operation) database.operation(bytes); return database.operation_result;
}
wire::MediaResult change_media_favorite(const std::string&,const std::string&,bool,bool,std::string*,const std::vector<uint8_t>* bytes) noexcept {
    if (database.operation && bytes) database.operation(*bytes); return database.operation_result;
}
wire::MediaResult authorize_media(const std::string&,const std::string&,bool,std::string*) noexcept { return Code::forbidden; }
wire::MediaResult load_media(const std::string&,const std::string&,bool,std::vector<uint8_t>& out,std::string*) noexcept {
    out = database.download; allocation_probe::target = out.data(); return Code::ok;
}
wire::MediaResult send_private_image(const std::string&,const std::string&,const std::string&,const std::string&,bool,bool,
    uint64_t& id,uint32_t& timestamp,bool& inserted,std::string*) noexcept { id=1; timestamp=1; inserted=true; return Code::ok; }
wire::MediaResult private_images(const std::string&,const std::string&,const std::vector<uint64_t>&,std::vector<PrivateImage>& out,std::string*) noexcept {
    out.clear(); return Code::ok;
}
}
int main() {
    try {
        const auto encoded = png();
        move_test();
        for (const auto operation : {Type::private_image_send,Type::favorite_action}) {
            consumption_test(operation,Code::ok,true,encoded);
            consumption_test(operation,Code::unavailable,true,encoded);
            consumption_test(operation,Code::ok,false,encoded);
        }
        lobby_test(encoded,true); lobby_test(encoded,false);
        normalization_failure_test(); destruction_test(encoded);
        download_test(false,false); download_test(true,false); download_test(true,true);
        std::cout << "PASS " << checks << " media budget assertions (database stubs; no network/database)\n";
        return 0;
    } catch (const std::exception& error) {
        std::cerr << "FAIL: " << error.what() << '\n'; return 1;
    }
}
