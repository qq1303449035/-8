#pragma once
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <Windows.h>
#include <wincodec.h>
#include <wrl/client.h>
#include <bcrypt.h>
#include <algorithm>
#include <cstdint>
#include <climits>
#include <cstring>
#include <string>
#include <vector>
#include "protocol.h"
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "windowscodecs.lib")
#pragma comment(lib, "bcrypt.lib")

namespace chh::avatar {
using Microsoft::WRL::ComPtr;
constexpr uint32_t side = 256;
constexpr size_t source_byte_limit = 10 * 1024 * 1024;
struct Image { uint32_t width = 0, height = 0; std::vector<uint8_t> pixels; };
namespace detail {
struct ComScope {
    HRESULT result = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    ~ComScope() { if (SUCCEEDED(result)) CoUninitialize(); }
    bool valid() const { return SUCCEEDED(result) || result == RPC_E_CHANGED_MODE; }
};
inline bool fail(std::string& error, const char* message) { error = message; return false; }
inline bool valid_dimensions(uint32_t width, uint32_t height) {
    return width && height && width <= 8192 && height <= 8192 && uint64_t(width) * height <= 32ull * 1024 * 1024;
}
inline bool valid_image(const Image& image) {
    return valid_dimensions(image.width, image.height) && image.pixels.size() == uint64_t(image.width) * image.height * 4;
}
inline bool complete_png(const std::vector<uint8_t>& bytes) {
    static constexpr uint8_t signature[]{137,80,78,71,13,10,26,10};
    if (bytes.size() < 8 || std::memcmp(bytes.data(), signature, 8)) return false;
    static const auto crc_table = [] {
        std::array<uint32_t, 256> table{};
        for (uint32_t n = 0; n < 256; ++n) {
            uint32_t c = n;
            for (unsigned j = 0; j < 8; ++j) c = (c >> 1) ^ ((c & 1) ? 0xedb88320u : 0);
            table[n] = c;
        }
        return table;
    }();
    bool header = false, data = false, after_data = false;
    for (size_t offset = 8; offset < bytes.size();) {
        if (bytes.size() - offset < 12) return false;
        const size_t count = wire::read32(bytes.data() + offset);
        if (count > bytes.size() - offset - 12) return false;
        const auto* type = bytes.data() + offset + 4;
        for (size_t i = 0; i < 4; ++i)
            if (!((type[i] >= 'A' && type[i] <= 'Z') || (type[i] >= 'a' && type[i] <= 'z'))) return false;
        uint32_t crc = 0xffffffffu;
        for (size_t i = 0; i < count + 4; ++i) crc = crc_table[(crc ^ type[i]) & 255] ^ (crc >> 8);
        if ((crc ^ 0xffffffffu) != wire::read32(type + 4 + count)) return false;
        if (!std::memcmp(type, "IHDR", 4)) {
            if (header || offset != 8 || count != 13) return false;
            header = true;
        } else if (!header) return false;
        else if (!std::memcmp(type, "acTL", 4) || !std::memcmp(type, "fcTL", 4) || !std::memcmp(type, "fdAT", 4)) return false;
        else if (!std::memcmp(type, "IDAT", 4)) {
            if (after_data) return false;
            data = true;
        } else {
            if (data) after_data = true;
            if (!std::memcmp(type, "IEND", 4)) return count == 0 && data && offset + 12 == bytes.size();
        }
        offset += count + 12;
    }
    return false;
}
inline bool factory(ComPtr<IWICImagingFactory>& out, std::string& error) {
    return SUCCEEDED(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER,
        IID_PPV_ARGS(out.GetAddressOf()))) || fail(error, "无法初始化图片处理组件");
}
inline bool pixels(IWICImagingFactory* factory, IWICBitmapSource* source, Image& out, std::string& error) {
    Image image;
    if (FAILED(source->GetSize(&image.width, &image.height)) || !valid_dimensions(image.width, image.height))
        return fail(error, "图片尺寸无效或过大");
    ComPtr<IWICFormatConverter> convert;
    if (FAILED(factory->CreateFormatConverter(&convert)) || FAILED(convert->Initialize(source, GUID_WICPixelFormat32bppRGBA,
        WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom))) return fail(error, "无法解码图片颜色");
    image.pixels.resize(size_t(image.width) * image.height * 4);
    if (FAILED(convert->CopyPixels(nullptr, image.width * 4, static_cast<UINT>(image.pixels.size()), image.pixels.data())))
        return fail(error, "图片内容损坏或无法读取");
    out = std::move(image); return true;
}
inline void orient(Image& image, unsigned orientation) {
    if (orientation <= 1 || orientation > 8) return;
    Image result;
    result.width = orientation >= 5 ? image.height : image.width;
    result.height = orientation >= 5 ? image.width : image.height;
    result.pixels.resize(image.pixels.size());
    for (uint32_t y = 0; y < result.height; ++y) for (uint32_t x = 0; x < result.width; ++x) {
        uint32_t sx = x, sy = y;
        switch (orientation) {
        case 2: sx = image.width - 1 - x; break;
        case 3: sx = image.width - 1 - x; sy = image.height - 1 - y; break;
        case 4: sy = image.height - 1 - y; break;
        case 5: sx = y; sy = x; break;
        case 6: sx = y; sy = image.height - 1 - x; break;
        case 7: sx = image.width - 1 - y; sy = image.height - 1 - x; break;
        case 8: sx = image.width - 1 - y; sy = x; break;
        }
        std::memcpy(result.pixels.data() + (size_t(y) * result.width + x) * 4,
            image.pixels.data() + (size_t(sy) * image.width + sx) * 4, 4);
    }
    image = std::move(result);
}
inline bool decode(const std::vector<uint8_t>& encoded, Image& out, bool exact_avatar, std::string& error) {
    if (encoded.empty() || encoded.size() > (exact_avatar ? wire::avatar_size_limit : source_byte_limit))
        return fail(error, exact_avatar ? "头像文件超过允许大小" : "请选择10MB以内的JPG或PNG图片");
    // WIC may recover incomplete PNGs or present APNG as a single static frame.
    // Require an intact static container before giving any bytes to the decoder.
    const bool png = encoded.size() >= 8 && encoded[0] == 137 && encoded[1] == 80;
    if ((png && !complete_png(encoded)) || (!png && (exact_avatar || encoded.size() < 4 ||
        encoded[0] != 0xff || encoded[1] != 0xd8 || encoded[encoded.size()-2] != 0xff || encoded.back() != 0xd9)))
        return fail(error, "图片内容不完整或不是静态JPG/PNG");
    ComScope com;
    ComPtr<IWICImagingFactory> imaging;
    if (!com.valid() || !factory(imaging, error)) return fail(error, "无法初始化图片处理组件");
    ComPtr<IWICStream> stream;
    ComPtr<IWICBitmapDecoder> decoder;
    if (FAILED(imaging->CreateStream(&stream)) || FAILED(stream->InitializeFromMemory(
        const_cast<BYTE*>(encoded.data()), static_cast<DWORD>(encoded.size()))) ||
        FAILED(imaging->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder)))
        return fail(error, "图片内容损坏或不受支持");
    GUID format{}; UINT count = 0;
    if (FAILED(decoder->GetContainerFormat(&format)) || FAILED(decoder->GetFrameCount(&count)) || count != 1 ||
        (format != GUID_ContainerFormatPng && (exact_avatar || format != GUID_ContainerFormatJpeg)))
        return fail(error, exact_avatar ? "头像必须是单帧PNG" : "仅支持静态JPG和PNG图片");
    ComPtr<IWICBitmapFrameDecode> frame;
    UINT width = 0, height = 0;
    if (FAILED(decoder->GetFrame(0, &frame)) || FAILED(frame->GetSize(&width, &height)) ||
        !valid_dimensions(width, height) || (exact_avatar && (width != side || height != side)))
        return fail(error, exact_avatar ? "头像尺寸必须为256×256" : "图片尺寸不能超过8192边长或3200万像素");
    unsigned orientation = 1;
    if (!exact_avatar && format == GUID_ContainerFormatJpeg) {
        ComPtr<IWICMetadataQueryReader> metadata;
        PROPVARIANT value{};
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata)) &&
            SUCCEEDED(metadata->GetMetadataByName(L"/app1/ifd/{ushort=274}", &value))) {
            if (value.vt == VT_UI2) orientation = value.uiVal;
            else if (value.vt == VT_UI4) orientation = value.ulVal;
        }
        PropVariantClear(&value);
    }
    ComPtr<IWICBitmapSource> source = frame;
    ComPtr<IWICBitmapScaler> scaler;
    const UINT longest = (std::max)(width, height);
    if (!exact_avatar && longest > 1024) {
        const UINT preview_width = (std::max)(1u, static_cast<UINT>(uint64_t(width) * 1024 / longest));
        const UINT preview_height = (std::max)(1u, static_cast<UINT>(uint64_t(height) * 1024 / longest));
        if (FAILED(imaging->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(frame.Get(),
            preview_width, preview_height, WICBitmapInterpolationModeFant))) return fail(error, "无法缩放图片");
        source = scaler;
    }
    Image image;
    if (!pixels(imaging.Get(), source.Get(), image, error)) return false;
    orient(image, orientation);
    out = std::move(image); error.clear(); return true;
}
// WIC steps shared by avatar and media PNG encoding; callers map the failed
// step to their own message. The image must already be validated.
enum class PngStep { ok, bitmap, encoder, encode, size, read };
inline PngStep write_png(IWICImagingFactory* imaging, const Image& image, uint64_t max_bytes, std::vector<uint8_t>& encoded) {
    ComPtr<IWICBitmap> bitmap;
    if (FAILED(imaging->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppRGBA, image.width * 4,
        static_cast<UINT>(image.pixels.size()), const_cast<BYTE*>(image.pixels.data()), &bitmap))) return PngStep::bitmap;
    ComPtr<IStream> stream;
    ComPtr<IWICBitmapEncoder> encoder;
    ComPtr<IWICBitmapFrameEncode> frame;
    if (FAILED(CreateStreamOnHGlobal(nullptr, TRUE, &stream)) ||
        FAILED(imaging->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder)) ||
        FAILED(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache)) ||
        FAILED(encoder->CreateNewFrame(&frame, nullptr)) || FAILED(frame->Initialize(nullptr)) ||
        FAILED(frame->SetSize(image.width, image.height))) return PngStep::encoder;
    GUID format = GUID_WICPixelFormat32bppRGBA;
    ComPtr<IWICFormatConverter> convert;
    if (FAILED(frame->SetPixelFormat(&format)) || FAILED(imaging->CreateFormatConverter(&convert)) ||
        FAILED(convert->Initialize(bitmap.Get(), format, WICBitmapDitherTypeNone, nullptr, 0, WICBitmapPaletteTypeCustom)) ||
        FAILED(frame->WriteSource(convert.Get(), nullptr)) || FAILED(frame->Commit()) || FAILED(encoder->Commit()))
        return PngStep::encode;
    STATSTG stat{}; HGLOBAL memory = nullptr;
    if (FAILED(stream->Stat(&stat, STATFLAG_NONAME)) || !stat.cbSize.QuadPart || stat.cbSize.QuadPart > max_bytes ||
        FAILED(GetHGlobalFromStream(stream.Get(), &memory))) return PngStep::size;
    const auto* bytes = static_cast<const uint8_t*>(GlobalLock(memory));
    if (!bytes) return PngStep::read;
    try { encoded.assign(bytes, bytes + static_cast<size_t>(stat.cbSize.QuadPart)); }
    catch (...) { GlobalUnlock(memory); throw; }
    GlobalUnlock(memory); return PngStep::ok;
}
inline bool encode(const Image& image, std::vector<uint8_t>& encoded, std::string& error) {
    if (!valid_image(image) || image.width != side || image.height != side) return fail(error, "头像像素数据无效");
    ComScope com; ComPtr<IWICImagingFactory> imaging;
    if (!com.valid() || !factory(imaging, error)) return fail(error, "无法初始化图片处理组件");
    switch (write_png(imaging.Get(), image, wire::avatar_size_limit, encoded)) {
    case PngStep::ok: error.clear(); return true;
    case PngStep::bitmap: return fail(error, "无法准备头像像素");
    case PngStep::encoder: return fail(error, "无法创建PNG头像");
    case PngStep::encode: return fail(error, "无法编码PNG头像");
    case PngStep::size: return fail(error, "头像编码结果过大");
    default: return fail(error, "无法读取头像编码结果");
    }
}
}
inline bool decode_source(const std::vector<uint8_t>& encoded, Image& image, std::string& error) noexcept {
    try { return detail::decode(encoded, image, false, error); }
    catch (...) { return detail::fail(error, "读取图片失败，请尝试较小的图片"); }
}
inline bool decode_png256(const std::vector<uint8_t>& encoded, Image& image, std::string& error) noexcept {
    try { return detail::decode(encoded, image, true, error); }
    catch (...) { return detail::fail(error, "头像解码失败"); }
}
inline bool crop_png(const Image& image, uint32_t x, uint32_t y, uint32_t crop_side,
    std::vector<uint8_t>& encoded, std::string& error) noexcept {
    try {
        if (!detail::valid_image(image) || !crop_side || x > image.width || y > image.height ||
            crop_side > image.width - x || crop_side > image.height - y) return detail::fail(error, "裁剪区域超出图片范围");
        detail::ComScope com; ComPtr<IWICImagingFactory> imaging;
        if (!com.valid() || !detail::factory(imaging, error)) return detail::fail(error, "无法初始化图片处理组件");
        ComPtr<IWICBitmap> bitmap; ComPtr<IWICBitmapClipper> clipper; ComPtr<IWICBitmapScaler> scaler;
        WICRect rect{static_cast<INT>(x), static_cast<INT>(y), static_cast<INT>(crop_side), static_cast<INT>(crop_side)};
        if (FAILED(imaging->CreateBitmapFromMemory(image.width, image.height, GUID_WICPixelFormat32bppRGBA, image.width * 4,
            static_cast<UINT>(image.pixels.size()), const_cast<BYTE*>(image.pixels.data()), &bitmap)) ||
            FAILED(imaging->CreateBitmapClipper(&clipper)) || FAILED(clipper->Initialize(bitmap.Get(), &rect)) ||
            FAILED(imaging->CreateBitmapScaler(&scaler)) || FAILED(scaler->Initialize(clipper.Get(), side, side, WICBitmapInterpolationModeFant)))
            return detail::fail(error, "无法裁剪图片");
        Image result;
        return detail::pixels(imaging.Get(), scaler.Get(), result, error) && detail::encode(result, encoded, error);
    } catch (...) { return detail::fail(error, "处理头像失败，请尝试另一张图片"); }
}
inline bool normalize_png256(const std::vector<uint8_t>& encoded, std::vector<uint8_t>& canonical_png, std::string& error) noexcept {
    try {
        Image image;
        return detail::decode(encoded, image, true, error) && detail::encode(image, canonical_png, error);
    } catch (...) { return detail::fail(error, "头像内容无法处理"); }
}
inline bool valid_hash(const std::string& value) { return wire::valid_sha256_hex(value); }
inline bool sha256(const std::vector<uint8_t>& bytes, std::string& lowercase_hex) noexcept {
    BCRYPT_ALG_HANDLE algorithm = nullptr; BCRYPT_HASH_HANDLE hash = nullptr;
    try {
        DWORD object_length = 0, received = 0;
        bool good = bytes.size() <= ULONG_MAX && BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) >= 0 &&
            BCryptGetProperty(algorithm, BCRYPT_OBJECT_LENGTH, reinterpret_cast<PUCHAR>(&object_length), sizeof(object_length), &received, 0) >= 0;
        std::vector<uint8_t> object(good ? object_length : 0);
        uint8_t digest[32]{};
        good = good && BCryptCreateHash(algorithm, &hash, object.data(), object_length, nullptr, 0, 0) >= 0 &&
            BCryptHashData(hash, const_cast<PUCHAR>(bytes.data()), static_cast<ULONG>(bytes.size()), 0) >= 0 &&
            BCryptFinishHash(hash, digest, sizeof(digest), 0) >= 0;
        if (hash) { BCryptDestroyHash(hash); hash = nullptr; }
        if (algorithm) { BCryptCloseAlgorithmProvider(algorithm, 0); algorithm = nullptr; }
        if (!good) return false;
        constexpr char hex[] = "0123456789abcdef";
        std::string result; result.reserve(64);
        for (auto byte : digest) { result += hex[byte >> 4]; result += hex[byte & 15]; }
        lowercase_hex = std::move(result); return true;
    } catch (...) {
        if (hash) BCryptDestroyHash(hash);
        if (algorithm) BCryptCloseAlgorithmProvider(algorithm, 0);
        return false;
    }
}
}
