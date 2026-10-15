#pragma once
#include "avatar_codec.h"
#include <array>
#include <limits>
#include <new>
#include <utility>

// Call normalize/decode on a worker thread. No UI objects or global COM state
// are owned here. GIF normalization preserves validated compressed image data;
// it does not quantize colors or invent a new GIF encoder.
namespace chh::media {
// Static JPG/PNG keep their original resolution. Only a source over
// static_side_limit or static_pixel_limit is scaled down (to fit both), and only
// a PNG over static_byte_limit shrinks further. GIF limits are unchanged.
constexpr size_t source_byte_limit = 32 * 1024 * 1024; // JPG/PNG read from a file or the clipboard.
constexpr size_t static_byte_limit = 15 * 1024 * 1024, gif_byte_limit = 2 * 1024 * 1024;
constexpr size_t encoded_byte_limit = static_byte_limit; // Largest canonical file of either kind.
constexpr uint32_t static_side_limit = 8192, gif_side_limit = 1024, gif_frame_limit = 120;
constexpr uint64_t static_pixel_limit = 16ull * 1024 * 1024;
// The largest JPG/PNG source that is scaled down rather than refused (a 50 MP
// photo, a 12000 x 3000 panorama). WIC's scaler reads the source row by row, so
// memory follows the fitted output; the bound keeps the decode time in check.
constexpr uint32_t source_side_limit = 65535;
constexpr uint64_t source_pixel_limit = 64ull * 1024 * 1024;
constexpr uint64_t decoded_pixel_limit = 16ull * 1024 * 1024;
constexpr uint64_t rgba_budget = 64ull * 1024 * 1024;
static_assert(encoded_byte_limit == wire::media_size_limit, "a canonical image must fit one media transfer");
// Width and height scaled so the longest side becomes `longest`, rounded like
// every WIC scaler call in this file (floor, at least 1 px).
inline void scaled_size(uint32_t width, uint32_t height, uint32_t longest, uint32_t& out_width, uint32_t& out_height) {
    const uint64_t old_longest = (std::max)((std::max)(width, height), 1u);
    out_width = (std::max)(1u, static_cast<uint32_t>(uint64_t(width) * longest / old_longest));
    out_height = (std::max)(1u, static_cast<uint32_t>(uint64_t(height) * longest / old_longest));
}
// Longest side a width x height static image keeps: unchanged within both
// limits, otherwise the largest scaled size within both. Integer search, so the
// client and the server always pick the same size. 0 for an empty image.
inline uint32_t fitted_side(uint32_t width, uint32_t height,
    uint32_t max_side = static_side_limit, uint64_t max_pixels = static_pixel_limit) {
    if (!width || !height || !max_side || !max_pixels) return 0;
    const auto fits = [&](uint32_t side) {
        uint32_t fitted_width = 0, fitted_height = 0;
        scaled_size(width, height, side, fitted_width, fitted_height);
        return uint64_t(fitted_width) * fitted_height <= max_pixels;
    };
    uint32_t low = 1, high = (std::min)((std::max)(width, height), max_side);
    if (fits(high)) return high;
    while (high - low > 1) {
        const uint32_t middle = low + (high - low) / 2;
        if (fits(middle)) low = middle; else high = middle;
    }
    return low;
}
enum class Kind : uint32_t { png = 1, gif = 2 };
struct Info {
    Kind kind = Kind::png;
    uint32_t width = 0, height = 0, frame_count = 0;
    // 0 = infinite; positive values count complete playthroughs. No GIF loop
    // extension means one playthrough. NETSCAPE repeat N becomes N + 1.
    uint32_t loop_count = 1;
};
struct Frame {
    std::vector<uint8_t> pixels; // width * height * 4, straight RGBA, full canvas.
    uint32_t delay_ms = 0;      // Original GIF delay, including zero-delay frames.
};
struct Decoded { Info info; std::vector<Frame> frames; };
struct Normalized { Info info; std::vector<uint8_t> bytes; std::string hash; };
inline bool sha256(const std::vector<uint8_t>& bytes, std::string& hash) noexcept {
    return avatar::sha256(bytes, hash);
}
namespace detail {
using avatar::Image;
using avatar::ComPtr;
using avatar::detail::fail;
inline bool is_gif(const std::vector<uint8_t>& bytes) {
    return bytes.size() >= 6 && (!std::memcmp(bytes.data(), "GIF87a", 6) || !std::memcmp(bytes.data(), "GIF89a", 6));
}
inline uint16_t little16(const uint8_t* bytes) { return uint16_t(bytes[0]) | (uint16_t(bytes[1]) << 8); }
inline void add16(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value)); bytes.push_back(static_cast<uint8_t>(value >> 8));
}
// Read-only WIC source over caller-owned straight RGBA. Scaling and encoding read
// the vector directly instead of CreateBitmapFromMemory's full copy (64 MiB for a
// 16 MP image, which matters in the 32-bit client). The image must outlive every
// WIC object built on the source.
class PixelSource final : public IWICBitmapSource {
public:
    static bool create(const Image& image, ComPtr<IWICBitmapSource>& out) {
        if (!image.width || !image.height || image.width > 65535 || image.height > 65535 ||
            image.pixels.size() != uint64_t(image.width) * image.height * 4) return false;
        out.Attach(new (std::nothrow) PixelSource(image));
        return out != nullptr;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id != __uuidof(IUnknown) && id != __uuidof(IWICBitmapSource)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IWICBitmapSource*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE GetSize(UINT* width, UINT* height) override {
        if (!width || !height) return E_INVALIDARG;
        *width = image_.width; *height = image_.height; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetPixelFormat(WICPixelFormatGUID* format) override {
        if (!format) return E_INVALIDARG;
        *format = GUID_WICPixelFormat32bppRGBA; return S_OK;
    }
    // Like CreateBitmapFromMemory: no resolution, so the PNG encoder writes no
    // pHYs chunk and canonical bytes stay identical to earlier builds.
    HRESULT STDMETHODCALLTYPE GetResolution(double* x, double* y) override {
        if (!x || !y) return E_INVALIDARG;
        *x = *y = 0.0; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyPalette(IWICPalette*) override { return WINCODEC_ERR_PALETTEUNAVAILABLE; }
    HRESULT STDMETHODCALLTYPE CopyPixels(const WICRect* rect, UINT stride, UINT size, BYTE* buffer) override {
        const WICRect all{0, 0, static_cast<INT>(image_.width), static_cast<INT>(image_.height)};
        const WICRect& area = rect ? *rect : all;
        if (!buffer || area.X < 0 || area.Y < 0 || area.Width < 0 || area.Height < 0 ||
            uint64_t(area.X) + uint64_t(area.Width) > image_.width || uint64_t(area.Y) + uint64_t(area.Height) > image_.height)
            return E_INVALIDARG;
        if (!area.Width || !area.Height) return S_OK;
        const size_t row = size_t(area.Width) * 4;
        if (stride < row || (uint64_t(area.Height) - 1) * stride + row > size) return WINCODEC_ERR_INSUFFICIENTBUFFER;
        for (INT y = 0; y < area.Height; ++y)
            std::memcpy(buffer + size_t(y) * stride, image_.pixels.data() + ((size_t(area.Y) + y) * image_.width + area.X) * 4, row);
        return S_OK;
    }
private:
    explicit PixelSource(const Image& image) : image_(image) {}
    ~PixelSource() = default;
    const Image& image_;
    LONG references_ = 1;
};
// Memory IStream that refuses to grow past `cap` bytes: the PNG encoder of an
// oversized image fails at the cap instead of buffering its whole result, and
// `overflow` tells that apart from a real encoder failure.
class CappedStream final : public IStream {
public:
    explicit CappedStream(size_t cap) : cap_(cap) {}
    std::vector<uint8_t> bytes;
    bool overflow = false;
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** out) override {
        if (!out) return E_POINTER;
        if (id != __uuidof(IUnknown) && id != __uuidof(ISequentialStream) && id != __uuidof(IStream)) { *out = nullptr; return E_NOINTERFACE; }
        *out = static_cast<IStream*>(this); AddRef(); return S_OK;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return static_cast<ULONG>(InterlockedIncrement(&references_)); }
    ULONG STDMETHODCALLTYPE Release() override {
        const LONG count = InterlockedDecrement(&references_);
        if (!count) delete this;
        return static_cast<ULONG>(count);
    }
    HRESULT STDMETHODCALLTYPE Read(void* buffer, ULONG size, ULONG* read) override {
        const size_t count = position_ < bytes.size() ? (std::min)(size_t(size), bytes.size() - position_) : 0;
        if (count && !buffer) return STG_E_INVALIDPOINTER;
        if (count) std::memcpy(buffer, bytes.data() + position_, count);
        position_ += count; if (read) *read = static_cast<ULONG>(count);
        return count == size ? S_OK : S_FALSE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* buffer, ULONG size, ULONG* written) override {
        if (written) *written = 0;
        if (size && !buffer) return STG_E_INVALIDPOINTER;
        if (position_ > cap_ || size > cap_ - position_) { overflow = true; return STG_E_MEDIUMFULL; }
        const size_t end = position_ + size;
        try {
            if (end > bytes.capacity()) bytes.reserve((std::min)(cap_, (std::max)(end, bytes.capacity() * 2)));
            if (end > bytes.size()) bytes.resize(end);
        } catch (...) { return E_OUTOFMEMORY; }
        if (size) std::memcpy(bytes.data() + position_, buffer, size);
        position_ = end; if (written) *written = size; return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(LARGE_INTEGER move, DWORD origin, ULARGE_INTEGER* result) override {
        int64_t base = 0;
        if (origin == STREAM_SEEK_CUR) base = static_cast<int64_t>(position_);
        else if (origin == STREAM_SEEK_END) base = static_cast<int64_t>(bytes.size());
        else if (origin != STREAM_SEEK_SET) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart < -base) return STG_E_INVALIDFUNCTION;
        if (move.QuadPart > static_cast<int64_t>(cap_) - base) { overflow = true; return STG_E_MEDIUMFULL; }
        position_ = static_cast<size_t>(base + move.QuadPart);
        if (result) result->QuadPart = position_;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetSize(ULARGE_INTEGER size) override {
        if (size.QuadPart > cap_) { overflow = true; return STG_E_MEDIUMFULL; }
        try { bytes.resize(static_cast<size_t>(size.QuadPart)); } catch (...) { return E_OUTOFMEMORY; }
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE CopyTo(IStream*, ULARGE_INTEGER, ULARGE_INTEGER*, ULARGE_INTEGER*) override { return E_NOTIMPL; }
    HRESULT STDMETHODCALLTYPE Commit(DWORD) override { return S_OK; }
    HRESULT STDMETHODCALLTYPE Revert() override { return S_OK; }
    HRESULT STDMETHODCALLTYPE LockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE UnlockRegion(ULARGE_INTEGER, ULARGE_INTEGER, DWORD) override { return STG_E_INVALIDFUNCTION; }
    HRESULT STDMETHODCALLTYPE Stat(STATSTG* stat, DWORD) override {
        if (!stat) return STG_E_INVALIDPOINTER;
        *stat = {}; stat->type = STGTY_STREAM; stat->cbSize.QuadPart = bytes.size(); stat->grfMode = STGM_READWRITE;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Clone(IStream** out) override { if (out) *out = nullptr; return E_NOTIMPL; }
private:
    ~CappedStream() = default;
    size_t cap_, position_ = 0;
    LONG references_ = 1;
};
inline bool read_static(const std::vector<uint8_t>& encoded, Image& out, bool canonical, std::string& error) {
    if (encoded.empty() || encoded.size() > (canonical ? static_byte_limit : source_byte_limit))
        return fail(error, "图片文件过大，源图片最多32MiB，发送文件最多15MiB");
    const bool png = encoded.size() >= 8 && encoded[0] == 137 && encoded[1] == 80;
    if ((png && !avatar::detail::complete_png(encoded)) || (!png && (canonical || encoded.size() < 4 ||
        encoded[0] != 0xff || encoded[1] != 0xd8 || encoded[encoded.size()-2] != 0xff || encoded.back() != 0xd9)))
        return fail(error, "请选择完整的静态JPG/PNG或GIF图片");
    avatar::detail::ComScope com; ComPtr<IWICImagingFactory> imaging;
    if (!com.valid() || !avatar::detail::factory(imaging, error)) return fail(error, "无法初始化图片组件");
    ComPtr<IWICStream> stream; ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(imaging->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size()))) ||
        FAILED(imaging->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return fail(error, "图片内容损坏或无法解码");
    GUID format{}; UINT count = 0;
    if (FAILED(decoder->GetContainerFormat(&format)) || FAILED(decoder->GetFrameCount(&count)) || count != 1 ||
        (format != GUID_ContainerFormatPng && (canonical || format != GUID_ContainerFormatJpeg)))
        return fail(error, "仅支持静态JPG/PNG和GIF");
    ComPtr<IWICBitmapFrameDecode> frame; UINT width = 0, height = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) || !width || !height)
        return fail(error, "图片尺寸超出允许范围");
    if (canonical && (!avatar::detail::valid_dimensions(width, height) || width > static_side_limit ||
        height > static_side_limit || uint64_t(width) * height > static_pixel_limit))
        return fail(error, "图片尺寸超出允许范围");
    if (!canonical && (width > source_side_limit || height > source_side_limit || uint64_t(width) * height > source_pixel_limit))
        return fail(error, "图片尺寸过大：每边最多65535像素，总像素最多64 MP");
    unsigned orientation = 1;
    if (format == GUID_ContainerFormatJpeg) {
        ComPtr<IWICMetadataQueryReader> metadata; PROPVARIANT value{};
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata)) && SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value))) {
            if (value.vt == VT_UI2) orientation = value.uiVal;
            else if (value.vt == VT_UI4) orientation = value.ulVal;
        }
        PropVariantClear(&value);
    }
    ComPtr<IWICBitmapSource> source = frame; ComPtr<IWICBitmapScaler> scaler;
    const UINT longest = (std::max)(width, height), side = fitted_side(width, height);
    if (side < longest) {
        UINT target_width = 0, target_height = 0; scaled_size(width, height, side, target_width, target_height);
        if (FAILED(imaging->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(), target_width, target_height, WICBitmapInterpolationModeFant)))
            return fail(error, "无法缩小图片");
        source = scaler;
    }
    if (!avatar::detail::pixels(imaging.Get(), source.Get(), out, error)) return false;
    avatar::detail::orient(out, orientation); return true;
}
// Any size up to 65535 px per side (a multi-monitor capture); the result must
// satisfy chh::avatar's limits.
inline bool shrink(const Image& original, uint32_t longest, Image& out, std::string& error) {
    avatar::detail::ComScope com; ComPtr<IWICImagingFactory> imaging;
    if (!com.valid() || !avatar::detail::factory(imaging, error)) return fail(error, "无法初始化图片组件");
    UINT width = 0, height = 0; scaled_size(original.width, original.height, longest, width, height);
    ComPtr<IWICBitmapSource> bitmap; ComPtr<IWICBitmapScaler> scaler;
    if (!PixelSource::create(original, bitmap) || FAILED(imaging->CreateBitmapScaler(&scaler)) ||
        FAILED(scaler->Initialize(bitmap.Get(), width, height, WICBitmapInterpolationModeFant)))
        return fail(error, "无法缩小图片");
    return avatar::detail::pixels(imaging.Get(), scaler.Get(), out, error);
}
enum class Png { ok, too_large, failed };
// Encodes a valid image (chh::avatar limits) as PNG of at most max_bytes; `out`
// changes only on ok. too_large also costs at most max_bytes of memory and stops
// the encoder there, so the shrink loop never pays for a whole oversized encode.
inline Png write_png(const Image& image, size_t max_bytes, std::vector<uint8_t>& out, std::string& error) {
    if (!avatar::detail::valid_image(image)) { fail(error, "规范图片尺寸无效"); return Png::failed; }
    avatar::detail::ComScope com; ComPtr<IWICImagingFactory> imaging;
    if (!com.valid() || !avatar::detail::factory(imaging, error)) { fail(error, "无法初始化图片组件"); return Png::failed; }
    ComPtr<IWICBitmapSource> pixels, source; ComPtr<CappedStream> stream; ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame; ComPtr<IWICFormatConverter> convert;
    stream.Attach(new (std::nothrow) CappedStream(max_bytes));
    if (!stream || !PixelSource::create(image, pixels) || FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) || FAILED(encoder->CreateNewFrame(&frame, nullptr)) ||
        FAILED(frame->Initialize(nullptr)) || FAILED(frame->SetSize(image.width, image.height))) { fail(error, "无法编码PNG图片"); return Png::failed; }
    GUID format = GUID_WICPixelFormat32bppRGBA; source = pixels;
    if (FAILED(frame->SetPixelFormat(&format))) { fail(error, "无法编码PNG图片"); return Png::failed; }
    if (format != GUID_WICPixelFormat32bppRGBA) {
        if (FAILED(imaging->CreateFormatConverter(&convert)) ||
            FAILED(convert->Initialize(pixels.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)))
            { fail(error, "无法编码PNG图片"); return Png::failed; }
        source = convert;
    }
    if (FAILED(frame->WriteSource(source.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit())) {
        if (stream->overflow) { fail(error, "PNG编码结果过大"); return Png::too_large; }
        fail(error, "PNG编码失败"); return Png::failed;
    }
    if (stream->bytes.empty()) { fail(error, "无法读取PNG编码结果"); return Png::failed; }
    // Growth doubles the buffer; do not keep up to twice the file in a cache.
    if (stream->bytes.capacity() - stream->bytes.size() > stream->bytes.size() / 8) stream->bytes.shrink_to_fit();
    out = std::move(stream->bytes); return Png::ok;
}
inline bool encode_png(const Image& image, std::vector<uint8_t>& out, std::string& error) {
    return write_png(image, source_byte_limit, out, error) == Png::ok;
}
// True for the layout write_png produces at width x height: IHDR 8-bit RGBA,
// not interlaced; optional sRGB 0 and gAMA 45455; IDAT with a plain zlib header;
// IEND; no other chunk. normalize keeps such a file byte for byte, because the
// 32-bit (client) and 64-bit (server) WIC encoders compress the same pixels
// differently and both sides must agree on the canonical bytes and hash.
inline bool canonical_layout(const std::vector<uint8_t>& bytes, uint32_t width, uint32_t height) {
    if (!avatar::detail::complete_png(bytes)) return false;
    unsigned stage = 0; // 1 after IHDR, 2 after sRGB, 3 after gAMA, 4 in IDAT
    for (size_t offset = 8; offset + 12 <= bytes.size();) {
        const uint32_t count = wire::read32(bytes.data() + offset);
        const uint8_t* type = bytes.data() + offset + 4; const uint8_t* data = type + 4;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (stage || count != 13 || wire::read32(data) != width || wire::read32(data + 4) != height ||
                data[8] != 8 || data[9] != 6 || data[10] || data[11] || data[12]) return false;
            stage = 1;
        } else if (!std::memcmp(type, "sRGB", 4)) {
            if (stage != 1 || count != 1 || data[0]) return false;
            stage = 2;
        } else if (!std::memcmp(type, "gAMA", 4)) {
            if (stage < 1 || stage > 2 || count != 4 || wire::read32(data) != 45455) return false;
            stage = 3;
        } else if (!std::memcmp(type, "IDAT", 4)) {
            if (stage < 1) return false;
            if (stage < 4 && (count < 2 || (data[0] & 15) != 8 || (data[0] >> 4) > 7 || (data[1] & 32) ||
                ((unsigned(data[0]) << 8) | data[1]) % 31)) return false;
            stage = 4;
        } else if (!std::memcmp(type, "IEND", 4)) return stage == 4;
        else return false;
        offset += size_t(count) + 12;
    }
    return false;
}

struct Palette { std::vector<uint8_t> rgb; size_t size() const { return rgb.size() / 3; } };
struct GifFrame {
    uint32_t x = 0, y = 0, width = 0, height = 0;
    uint16_t delay = 0; uint8_t disposal = 0, transparent = 0, minimum = 0;
    bool alpha = false, interlaced = false;
    Palette local; std::vector<uint8_t> compressed;
};
struct Gif {
    Info info; Palette global; uint8_t background = 0, aspect = 0;
    bool looping = false; uint16_t repeats = 0;
    std::vector<GifFrame> frames;
};
struct Cursor {
    const std::vector<uint8_t>& bytes; size_t offset = 0;
    bool take(size_t count, const uint8_t*& out) {
        if (count > bytes.size() - offset) return false;
        out = bytes.data() + offset; offset += count; return true;
    }
    bool subblocks(std::vector<uint8_t>& out) {
        out.clear(); const uint8_t* value = nullptr;
        for (;;) {
            if (!take(1, value)) return false;
            const size_t count = *value; if (!count) return true;
            if (!take(count, value)) return false;
            out.insert(out.end(), value, value + count);
        }
    }
    bool palette(Palette& out, unsigned bits) {
        const uint8_t* data = nullptr; const size_t count = size_t(3) << (bits + 1);
        if (!take(count, data)) return false;
        out.rgb.assign(data, data + count); return true;
    }
};
// Strict bounded GIF LZW decoder: validates dictionary references, output count,
// clear/end codes and trailing data. No compressed stream is trusted to WIC's
// best-effort recovery behavior. Each output palette index is validated below.
inline bool lzw(GifFrame& frame, size_t palette_size, std::vector<uint8_t>& pixels) {
    if (frame.minimum < 2 || frame.minimum > 8 || frame.compressed.empty()) return false;
    const unsigned clear = 1u << frame.minimum, end = clear + 1;
    unsigned next = end + 1, width = frame.minimum + 1;
    std::array<uint16_t, 4096> prefix{}; std::array<uint8_t, 4096> suffix{}, stack{};
    size_t bit = 0; int previous = -1; uint8_t first = 0; bool initialized = false;
    const size_t expected = size_t(frame.width) * frame.height;
    pixels.clear(); pixels.reserve(expected);
    for (;;) {
        if (bit + width > frame.compressed.size() * 8) return false;
        unsigned code = 0;
        for (unsigned i = 0; i < width; ++i) code |= unsigned((frame.compressed[(bit+i)/8] >> ((bit+i)%8)) & 1u) << i;
        bit += width;
        if (!initialized && code != clear) return false;
        initialized = true;
        if (code == clear) { next = end + 1; width = frame.minimum + 1; previous = -1; continue; }
        if (code == end) {
            if (pixels.size() != expected || (bit + 7) / 8 != frame.compressed.size()) return false;
            if (bit % 8) frame.compressed.back() &= static_cast<uint8_t>((1u << (bit % 8)) - 1);
            return true;
        }
        if (previous < 0) {
            if (code >= clear || code >= palette_size || pixels.size() >= expected) return false;
            first = static_cast<uint8_t>(code); pixels.push_back(first); previous = static_cast<int>(code); continue;
        }
        const unsigned incoming = code; size_t used = 0;
        if (code == next) { stack[used++] = first; code = static_cast<unsigned>(previous); }
        else if (code > next || code >= 4096) return false;
        while (code >= clear) {
            if (code < end + 1 || code >= next || used >= stack.size()) return false;
            stack[used++] = suffix[code]; code = prefix[code];
        }
        if (code >= palette_size || used >= stack.size()) return false;
        first = static_cast<uint8_t>(code); stack[used++] = first;
        if (used > expected - pixels.size()) return false;
        while (used) { const auto index = stack[--used]; if (index >= palette_size) return false; pixels.push_back(index); }
        if (next < 4096) {
            prefix[next] = static_cast<uint16_t>(previous); suffix[next] = first; ++next;
            if (next == (1u << width) && width < 12) ++width;
        }
        previous = static_cast<int>(incoming);
    }
}
inline bool parse_gif(const std::vector<uint8_t>& bytes, Gif& gif, std::string& error) {
    if (!is_gif(bytes) || bytes.size() > gif_byte_limit) return fail(error, "GIF必须完整且不超过2MiB");
    Cursor cursor{bytes}; const uint8_t* data = nullptr;
    if (!cursor.take(13, data)) return fail(error, "GIF头部不完整");
    gif.info.kind = Kind::gif; gif.info.width = little16(data+6); gif.info.height = little16(data+8);
    const uint8_t packed = data[10]; gif.background = data[11]; gif.aspect = data[12];
    if (!gif.info.width || !gif.info.height || gif.info.width > gif_side_limit || gif.info.height > gif_side_limit)
        return fail(error, "GIF尺寸最多1024×1024");
    if ((packed & 128) && !cursor.palette(gif.global, packed & 7)) return fail(error, "GIF全局调色板不完整");
    if (gif.global.size() && gif.background >= gif.global.size()) return fail(error, "GIF背景颜色索引无效");
    GifFrame control; bool pending_control = false; uint64_t raw_pixels = 0;
    std::vector<uint8_t> extra, indices;
    for (;;) {
        if (!cursor.take(1, data)) return fail(error, "GIF缺少结束标记");
        if (*data == 0x3b) {
            if (cursor.offset != bytes.size() || pending_control || gif.frames.empty()) return fail(error, "GIF结束位置无效");
            gif.info.frame_count = static_cast<uint32_t>(gif.frames.size()); return true;
        }
        if (*data == 0x21) {
            if (!cursor.take(1, data)) return fail(error, "GIF扩展不完整");
            const uint8_t label = *data;
            if (label == 0xf9) {
                if (pending_control || !cursor.take(6, data) || data[0] != 4 || data[5] != 0 || (data[1] & 0xe2))
                    return fail(error, "GIF帧控制无效或要求用户交互");
                control = {}; control.disposal = (data[1] >> 2) & 7;
                if (control.disposal > 3) return fail(error, "GIF处置模式无效");
                control.alpha = (data[1] & 1) != 0; control.delay = little16(data+2); control.transparent = data[4]; pending_control = true;
            } else if (label == 0xff) {
                if (!cursor.take(1, data) || *data != 11 || !cursor.take(11, data)) return fail(error, "GIF应用扩展无效");
                const bool loop = !std::memcmp(data, "NETSCAPE2.0", 11) || !std::memcmp(data, "ANIMEXTS1.0", 11);
                if (!cursor.subblocks(extra)) return fail(error, "GIF应用扩展不完整");
                if (loop) {
                    if (gif.looping || extra.size() != 3 || extra[0] != 1) return fail(error, "GIF循环信息无效");
                    gif.looping = true; gif.repeats = little16(extra.data()+1);
                    gif.info.loop_count = gif.repeats ? uint32_t(gif.repeats) + 1 : 0;
                }
            } else if (label == 0xfe) {
                if (!cursor.subblocks(extra)) return fail(error, "GIF注释不完整");
            } else return fail(error, "不支持GIF文本或未知渲染扩展");
            continue;
        }
        if (*data != 0x2c || !cursor.take(9, data)) return fail(error, "GIF图像描述不完整");
        GifFrame frame = std::move(control); control = {}; pending_control = false;
        frame.x = little16(data); frame.y = little16(data+2); frame.width = little16(data+4); frame.height = little16(data+6);
        const uint8_t image_packed = data[8]; frame.interlaced = (image_packed & 64) != 0;
        if (!frame.width || !frame.height || frame.x >= gif.info.width || frame.y >= gif.info.height ||
            frame.width > gif.info.width - frame.x || frame.height > gif.info.height - frame.y || (image_packed & 0x18))
            return fail(error, "GIF帧超出画布范围");
        raw_pixels += uint64_t(frame.width) * frame.height;
        const uint64_t composed = uint64_t(gif.frames.size()+1) * gif.info.width * gif.info.height;
        if (gif.frames.size() >= gif_frame_limit || raw_pixels > decoded_pixel_limit || composed > decoded_pixel_limit || composed * 4 > rgba_budget)
            return fail(error, "GIF帧数或解码内存超出限制");
        if ((image_packed & 128) && !cursor.palette(frame.local, image_packed & 7)) return fail(error, "GIF局部调色板不完整");
        const auto& palette = frame.local.size() ? frame.local : gif.global;
        if (!palette.size() || (frame.alpha && frame.transparent >= palette.size()) || !cursor.take(1, data))
            return fail(error, "GIF调色板或透明索引无效");
        frame.minimum = *data;
        if (!cursor.subblocks(frame.compressed) || !lzw(frame, palette.size(), indices)) return fail(error, "GIF压缩数据损坏或不完整");
        gif.frames.push_back(std::move(frame));
    }
}
inline unsigned palette_bits(size_t count) { unsigned bits = 0; while ((size_t(2) << bits) < count) ++bits; return bits; }
inline void blocks(std::vector<uint8_t>& out, const std::vector<uint8_t>& data) {
    for (size_t offset = 0; offset < data.size(); offset += 255) {
        const size_t count = (std::min)(size_t(255), data.size()-offset); out.push_back(static_cast<uint8_t>(count));
        out.insert(out.end(), data.begin()+offset, data.begin()+offset+count);
    }
    out.push_back(0);
}
inline void encode_gif(const Gif& gif, std::vector<uint8_t>& out) {
    out = {'G','I','F','8','9','a'}; add16(out, gif.info.width); add16(out, gif.info.height);
    out.push_back(static_cast<uint8_t>(gif.global.size() ? 0xf0 | palette_bits(gif.global.size()) : 0x70));
    out.push_back(gif.global.size() ? gif.background : 0); out.push_back(gif.aspect);
    out.insert(out.end(), gif.global.rgb.begin(), gif.global.rgb.end());
    if (gif.looping) {
        const uint8_t application[]{0x21,0xff,11,'N','E','T','S','C','A','P','E','2','.','0',3,1};
        out.insert(out.end(), std::begin(application), std::end(application)); add16(out, gif.repeats); out.push_back(0);
    }
    for (const auto& frame : gif.frames) {
        out.insert(out.end(), {0x21,0xf9,4,static_cast<uint8_t>((frame.disposal << 2) | (frame.alpha ? 1 : 0))});
        add16(out, frame.delay); out.push_back(frame.alpha ? frame.transparent : 0); out.push_back(0);
        out.push_back(0x2c); add16(out, frame.x); add16(out, frame.y); add16(out, frame.width); add16(out, frame.height);
        out.push_back(static_cast<uint8_t>((frame.interlaced ? 64 : 0) | (frame.local.size() ? 128 | palette_bits(frame.local.size()) : 0)));
        out.insert(out.end(), frame.local.rgb.begin(), frame.local.rgb.end()); out.push_back(frame.minimum); blocks(out, frame.compressed);
    }
    out.push_back(0x3b);
}
inline std::array<uint8_t,4> background(const Gif& gif, bool transparent) {
    std::array<uint8_t,4> value{};
    if (!transparent && gif.global.size()) {
        std::copy_n(gif.global.rgb.data()+size_t(gif.background)*3, 3, value.data()); value[3] = 255;
    }
    return value;
}
inline void clear_rect(std::vector<uint8_t>& canvas, uint32_t width, uint32_t x, uint32_t y, uint32_t w, uint32_t h, const std::array<uint8_t,4>& color) {
    for (uint32_t row = y; row < y+h; ++row) for (uint32_t col = x; col < x+w; ++col)
        std::copy(color.begin(), color.end(), canvas.begin()+(size_t(row)*width+col)*4);
}
inline bool compose(Gif& gif, Decoded& result, std::string& error) {
    result.info = gif.info; result.frames.reserve(gif.frames.size());
    std::vector<uint8_t> canvas(size_t(gif.info.width)*gif.info.height*4), saved, indices;
    clear_rect(canvas, gif.info.width, 0, 0, gif.info.width, gif.info.height, background(gif, gif.frames.front().alpha));
    for (auto& frame : gif.frames) {
        const auto& palette = frame.local.size() ? frame.local : gif.global;
        if (!lzw(frame, palette.size(), indices)) return fail(error, "GIF帧解码失败");
        if (frame.disposal == 3) saved = canvas;
        size_t source_row = 0;
        const auto draw_row = [&](uint32_t row) {
            for (uint32_t x = 0; x < frame.width; ++x) {
                const uint8_t index = indices[source_row*frame.width+x];
                if (frame.alpha && index == frame.transparent) continue;
                auto* pixel = canvas.data()+(size_t(frame.y+row)*gif.info.width+frame.x+x)*4;
                std::copy_n(palette.rgb.data()+size_t(index)*3, 3, pixel); pixel[3] = 255;
            }
            ++source_row;
        };
        if (frame.interlaced) {
            static constexpr uint32_t starts[]{0,4,2,1}, steps[]{8,8,4,2};
            for (unsigned pass = 0; pass < 4; ++pass) for (uint32_t row = starts[pass]; row < frame.height; row += steps[pass]) draw_row(row);
        } else for (uint32_t row = 0; row < frame.height; ++row) draw_row(row);
        result.frames.push_back(Frame{canvas, uint32_t(frame.delay)*10});
        if (frame.disposal == 2) clear_rect(canvas, gif.info.width, frame.x, frame.y, frame.width, frame.height, background(gif, frame.alpha));
        else if (frame.disposal == 3) canvas.swap(saved);
    }
    return true;
}
inline bool exception_error(std::string& error) noexcept {
    try { error = "图片处理失败或超出内存预算"; } catch (...) {}
    return false;
}
}

// Strong output guarantee: out is replaced only after the entire file passes.
inline bool normalize(const std::vector<uint8_t>& encoded, Normalized& out, std::string& error) noexcept {
    try {
        Normalized candidate;
        if (detail::is_gif(encoded)) {
            detail::Gif gif; if (!detail::parse_gif(encoded, gif, error)) return false;
            candidate.info = gif.info; detail::encode_gif(gif, candidate.bytes);
            if (candidate.bytes.size() > gif_byte_limit) return detail::fail(error, "规范GIF超过2MiB");
        } else {
            // A canonical PNG (already decoded in full above, so its data is sound)
            // is kept as is. Otherwise: full (fitted) resolution first; a PNG over
            // static_byte_limit shrinks the longest side in 15 % steps, always
            // from the fitted original.
            avatar::Image original, smaller;
            if (!detail::read_static(encoded, original, false, error)) return false;
            const avatar::Image* image = &original;
            if (encoded.size() <= static_byte_limit && detail::canonical_layout(encoded, original.width, original.height)) {
                candidate.bytes = encoded;
            } else for (;;) {
                const auto written = detail::write_png(*image, static_byte_limit, candidate.bytes, error);
                if (written == detail::Png::ok) break;
                if (written != detail::Png::too_large) return false;
                const uint32_t longest = (std::max)(image->width, image->height);
                if (longest <= 1 || !detail::shrink(original, (std::max)(1u, longest*85/100), smaller, error))
                    return detail::fail(error, "图片无法缩小到发送限制");
                image = &smaller;
            }
            candidate.info = {Kind::png, image->width, image->height, 1, 1};
        }
        if (!sha256(candidate.bytes, candidate.hash)) return detail::fail(error, "无法计算图片校验值");
        out = std::move(candidate); error.clear(); return true;
    } catch (...) { return detail::exception_error(error); }
}
// Decodes canonical PNG/GIF only. JPG source files must first be normalized.
inline bool decode(const std::vector<uint8_t>& encoded, Decoded& out, std::string& error) noexcept {
    try {
        Decoded candidate;
        if (detail::is_gif(encoded)) {
            detail::Gif gif;
            if (!detail::parse_gif(encoded, gif, error) || !detail::compose(gif, candidate, error)) return false;
        } else {
            avatar::Image image; if (!detail::read_static(encoded, image, true, error)) return false;
            candidate.info = {Kind::png, image.width, image.height, 1, 1};
            candidate.frames.push_back(Frame{std::move(image.pixels), 0});
        }
        out = std::move(candidate); error.clear(); return true;
    } catch (...) { return detail::exception_error(error); }
}
}
