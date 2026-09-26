#include "decode.h"
#include "psd.h"
#include <cmath>
#include <propvarutil.h>
#include <shlwapi.h>
#include <webp/decode.h>
#include <webp/demux.h>
#include <wincodec.h>
#include <turbojpeg.h>
#include <array>
namespace viewer {
namespace {
uint16_t exifOrientation(std::span<const uint8_t> data) {
    if (data.size() > 6 && memcmp(data.data(), "Exif\0\0", 6) == 0)
        data = data.subspan(6);
    if (data.size() < 8)
        return 1;
    bool le = data[0] == 'I' && data[1] == 'I';
    if (!le && !(data[0] == 'M' && data[1] == 'M'))
        return 1;
    auto u16 = [&](size_t p) -> uint16_t {
        if (p + 2 > data.size())
            return 0;
        return le ? (data[p] | data[p + 1] << 8) : (data[p] << 8 | data[p + 1]);
    };
    auto u32 = [&](size_t p) -> uint32_t {
        if (p + 4 > data.size())
            return 0;
        return le ? (uint32_t)u16(p) | ((uint32_t)u16(p + 2) << 16) : ((uint32_t)u16(p) << 16) | u16(p + 2);
    };
    if (u16(2) != 42)
        return 1;
    size_t offset = u32(4);
    if (offset + 2 > data.size())
        return 1;
    uint16_t count = u16(offset);
    for (size_t n = 0; n < count; ++n) {
        size_t p = offset + 2 + n * 12;
        if (p + 12 > data.size())
            break;
        if (u16(p) == 0x112 && u16(p + 2) == 3 && u32(p + 4) == 1) {
            auto v = u16(p + 8);
            return v >= 1 && v <= 8 ? v : 1;
        }
    }
    return 1;
}
WICBitmapTransformOptions transform(uint16_t o) {
    switch (o) {
    case 2:
        return WICBitmapTransformFlipHorizontal;
    case 3:
        return WICBitmapTransformRotate180;
    case 4:
        return WICBitmapTransformFlipVertical;
    case 5:
        return (WICBitmapTransformOptions)(WICBitmapTransformRotate90 | WICBitmapTransformFlipHorizontal);
    case 6:
        return WICBitmapTransformRotate90;
    case 7:
        return (WICBitmapTransformOptions)(WICBitmapTransformRotate270 | WICBitmapTransformFlipHorizontal);
    case 8:
        return WICBitmapTransformRotate270;
    default:
        return WICBitmapTransformRotate0;
    }
}
struct JpegMetadata {
    uint16_t orientation = 1;
    Bytes icc;
};
JpegMetadata jpegMetadata(std::span<const uint8_t> bytes) {
    JpegMetadata result;
    std::array<std::span<const uint8_t>, 256> chunks{};
    unsigned chunkCount = 0;
    bool valid = true;
    size_t pos = 2;
    while (pos < bytes.size()) {
        if (bytes[pos++] != 0xff)
            break;
        while (pos < bytes.size() && bytes[pos] == 0xff)
            ++pos;
        if (pos >= bytes.size())
            break;
        uint8_t marker = bytes[pos++];
        if (marker == 0xda || marker == 0xd9)
            break;
        if (marker == 1 || (marker >= 0xd0 && marker <= 0xd7))
            continue;
        if (pos + 2 > bytes.size())
            break;
        size_t length = (bytes[pos] << 8) | bytes[pos + 1];
        if (length < 2 || length > bytes.size() - pos)
            break;
        if (marker == 0xe1 && length >= 8 && memcmp(bytes.data() + pos + 2, "Exif\0\0", 6) == 0)
            result.orientation = exifOrientation(bytes.subspan(pos + 2, length - 2));
        if (marker == 0xe2 && length >= 16 && memcmp(bytes.data() + pos + 2, "ICC_PROFILE\0", 12) == 0) {
            unsigned sequence = bytes[pos + 14], count = bytes[pos + 15];
            if (!sequence || !count || sequence > count || (chunkCount && chunkCount != count) ||
                !chunks[sequence].empty())
                valid = false;
            else {
                chunkCount = count;
                chunks[sequence] = bytes.subspan(pos + 16, length - 16);
            }
        }
        pos += length;
    }
    if (valid && chunkCount) {
        for (unsigned i = 1; i <= chunkCount; ++i)
            if (chunks[i].empty())
                valid = false;
        if (valid)
            for (unsigned i = 1; i <= chunkCount; ++i)
                result.icc.insert(result.icc.end(), chunks[i].begin(), chunks[i].end());
    }
    return result;
}
struct TurboFrame {
    std::shared_ptr<Frame> frame;
    Bytes icc;
    uint16_t orientation = 1;
};
TurboFrame turboDecode(void*& handle, BytePtr bytes, uint32_t maxW, uint32_t maxH, uint64_t budget,
                       const Cancel& cancel) {
    TurboFrame result;
    if (!handle) {
        handle = tj3Init(TJINIT_DECOMPRESS);
        if (!handle)
            throw Error("Не удалось создать JPEG-декодер");
        tj3Set(handle, TJPARAM_MAXMEMORY, (int)std::min<uint64_t>(1024, physicalMemory() / 4 / MiB));
        tj3Set(handle, TJPARAM_MAXPIXELS, 1000000000);
        tj3Set(handle, TJPARAM_SAVEMARKERS,
               0); // Parse only current-image EXIF/ICC; never reuse prior ICC state.
    }
    if (tj3DecompressHeader(handle, bytes->data(), bytes->size()) < 0)
        return result;
    int w = tj3Get(handle, TJPARAM_JPEGWIDTH), h = tj3Get(handle, TJPARAM_JPEGHEIGHT);
    int colorspace = tj3Get(handle, TJPARAM_COLORSPACE);
    if (w <= 0 || h <= 0 || (uint64_t)w * h > 1000000000ull)
        throw Error("Недопустимый размер JPEG");
    // Keep CMYK/YCCK and unusual precision on the Windows color-management path.
    if (tj3Get(handle, TJPARAM_PRECISION) != 8 || tj3Get(handle, TJPARAM_LOSSLESS) ||
        colorspace == TJCS_CMYK || colorspace == TJCS_YCCK)
        return result;
    auto metadata = jpegMetadata(*bytes);
    result.orientation = metadata.orientation;
    result.icc = std::move(metadata.icc);
    if (result.orientation >= 5)
        std::swap(maxW, maxH);
    double wanted =
        std::min({1.0, (double)maxW / w, (double)maxH / h, std::sqrt((double)budget / (4.0 * w * h))});
    int count = 0;
    auto factors = tj3GetScalingFactors(&count);
    tjscalingfactor selected{0, 1};
    double best = 0;
    for (int i = 0; i < count; ++i) {
        if (factors[i].num != 1)
            continue; // Prefer SIMD-friendly 1, 1/2, 1/4 and 1/8 reductions.
        double scale = (double)factors[i].num / factors[i].denom;
        if (scale > 1 || scale < wanted)
            continue;
        uint64_t pixels = (uint64_t)TJSCALED(w, factors[i]) * TJSCALED(h, factors[i]);
        if (pixels * 4 <= budget && (!selected.num || scale < best)) {
            selected = factors[i];
            best = scale;
        }
    }
    if (!selected.num)
        for (int i = 0; i < count; ++i) {
            if (factors[i].num != 1)
                continue;
            double scale = (double)factors[i].num / factors[i].denom;
            if (scale > 1)
                continue;
            uint64_t pixels = (uint64_t)TJSCALED(w, factors[i]) * TJSCALED(h, factors[i]);
            if (pixels * 4 <= budget && scale > best) {
                selected = factors[i];
                best = scale;
            }
        }
    if (!selected.num || tj3SetScalingFactor(handle, selected) < 0)
        return result;
    auto frame = std::make_shared<Frame>();
    frame->originalWidth = w;
    frame->originalHeight = h;
    frame->width = TJSCALED(w, selected);
    frame->height = TJSCALED(h, selected);
    frame->pixels.resize((size_t)frame->width * frame->height * 4);
    if (cancel && cancel())
        throw Cancelled();
    if (tj3Decompress8(handle, bytes->data(), bytes->size(), frame->pixels.data(), frame->width * 4,
                       TJPF_BGRA) < 0) {
        // Do not retry a fatal/resource-limit failure in a decoder with a different memory policy.
        if (tj3GetErrorCode(handle) == TJERR_FATAL)
            throw Error(tj3GetErrorStr(handle));
        return result;
    }
    if (cancel && cancel())
        throw Cancelled();
    result.frame = std::move(frame);
    return result;
}
} // namespace
ImageDecoder::~ImageDecoder() {
    if (jpeg_)
        tj3Destroy(jpeg_);
}
std::shared_ptr<Frame> decodeImage(BytePtr bytes, uint32_t maxWidth, uint32_t maxHeight, uint64_t budget,
                                   const Cancel& cancel, DecodeOptions options) {
    ImageDecoder decoder;
    return decoder.decode(std::move(bytes), maxWidth, maxHeight, budget, cancel, options);
}
std::shared_ptr<Frame> ImageDecoder::decode(BytePtr bytes, uint32_t maxWidth, uint32_t maxHeight,
                                            uint64_t budget, const Cancel& cancel, DecodeOptions options) {
    if (cancel && cancel())
        throw Cancelled();
    if (!bytes || bytes->empty())
        throw Error("Пустое изображение");
    if (budget < 4)
        throw Error("Недостаточно памяти для изображения");
    maxWidth = std::max(1u, maxWidth);
    maxHeight = std::max(1u, maxHeight);
    const bool jpeg = bytes->size() >= 3 && (*bytes)[0] == 0xff && (*bytes)[1] == 0xd8 && (*bytes)[2] == 0xff;
    TurboFrame turbo;
    if (jpeg && options.jpeg != JpegBackend::Wic) {
        turbo = turboDecode(jpeg_, bytes, maxWidth, maxHeight, budget, cancel);
        if (!turbo.frame && options.jpeg == JpegBackend::Turbo)
            throw Error("JPEG не поддерживается быстрым декодером");
        if (turbo.frame && turbo.icc.empty() && turbo.orientation == 1 &&
            (options.nativeJpegSize || (turbo.frame->width <= maxWidth && turbo.frame->height <= maxHeight)))
            return turbo.frame;
    }
    auto& factory = factory_;
    if (!factory)
        check(
            CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&factory)),
            "WIC");
    ComPtr<IWICBitmapSource> source;
    ComPtr<IWICColorContext> profile;
    ComPtr<IWICBitmapDecoder> decoder;
    ComPtr<IStream> stream;
    uint32_t originalW = 0, originalH = 0;
    uint16_t orientation = 1;
    Bytes webpPixels;
    Bytes transformPixels;
    PsdImage psd;
    bool webp = bytes->size() >= 12 && memcmp(bytes->data(), "RIFF", 4) == 0 &&
                memcmp(bytes->data() + 8, "WEBP", 4) == 0;
    if (bytes->size() >= 4 && memcmp(bytes->data(), "8BPS", 4) == 0) {
        psd = decodePsd(*bytes, maxWidth, maxHeight, budget, cancel);
        auto& f = *psd.frame;
        originalW = f.originalWidth;
        originalH = f.originalHeight;
        ComPtr<IWICBitmap> bitmap;
        check(factory->CreateBitmapFromMemory(f.width, f.height, GUID_WICPixelFormat32bppBGRA, f.width * 4,
                                              (UINT)f.pixels.size(), f.pixels.data(), &bitmap),
              "PSD bitmap");
        source = bitmap;
        if (!psd.icc.empty()) {
            check(factory->CreateColorContext(&profile), "PSD ICC");
            if (FAILED(profile->InitializeFromMemory(psd.icc.data(), (UINT)psd.icc.size())))
                profile.Reset();
        }
        if (profile && psd.grayscale) {
            // Gray ICC profiles require a one-channel input, not replicated BGRA samples.
            Bytes gray(size_t(f.width) * f.height);
            for (size_t i = 0; i < gray.size(); ++i)
                gray[i] = f.pixels[4 * i];
            ComPtr<IWICBitmap> grayBitmap;
            check(factory->CreateBitmapFromMemory(f.width, f.height, GUID_WICPixelFormat8bppGray, f.width,
                                                  (UINT)gray.size(), gray.data(), &grayBitmap),
                  "PSD grayscale");
            ComPtr<IWICColorContext> srgb;
            check(factory->CreateColorContext(&srgb), "PSD sRGB");
            check(srgb->InitializeFromExifColorSpace(1), "PSD sRGB");
            ComPtr<IWICColorTransform> color;
            check(factory->CreateColorTransformer(&color), "PSD grayscale ICC");
            check(
                color->Initialize(grayBitmap.Get(), profile.Get(), srgb.Get(), GUID_WICPixelFormat32bppBGRA),
                "PSD grayscale ICC");
            Bytes converted(f.pixels.size());
            check(color->CopyPixels(nullptr, f.width * 4, (UINT)converted.size(), converted.data()),
                  "PSD grayscale ICC pixels");
            for (size_t i = 3; i < converted.size(); i += 4)
                converted[i] = f.pixels[i];
            f.pixels = std::move(converted);
            bitmap.Reset();
            check(factory->CreateBitmapFromMemory(f.width, f.height, GUID_WICPixelFormat32bppBGRA,
                                                  f.width * 4, (UINT)f.pixels.size(), f.pixels.data(),
                                                  &bitmap),
                  "PSD grayscale bitmap");
            source = bitmap;
            profile.Reset();
        }
    } else if (turbo.frame) {
        originalW = turbo.frame->originalWidth;
        originalH = turbo.frame->originalHeight;
        orientation = turbo.orientation;
        ComPtr<IWICBitmap> bitmap;
        check(factory->CreateBitmapFromMemory(turbo.frame->width, turbo.frame->height,
                                              GUID_WICPixelFormat32bppBGRA, turbo.frame->width * 4,
                                              (UINT)turbo.frame->pixels.size(), turbo.frame->pixels.data(),
                                              &bitmap),
              "JPEG bitmap");
        source = bitmap;
        if (!turbo.icc.empty()) {
            check(factory->CreateColorContext(&profile), "JPEG ICC");
            if (FAILED(profile->InitializeFromMemory(turbo.icc.data(), (UINT)turbo.icc.size())))
                profile.Reset();
        }
    } else if (webp) {
        WebPBitstreamFeatures info{};
        if (WebPGetFeatures(bytes->data(), bytes->size(), &info) != VP8_STATUS_OK)
            throw Error("Повреждённый WebP");
        originalW = info.width;
        originalH = info.height;
        WebPData data{bytes->data(), bytes->size()};
        std::unique_ptr<WebPDemuxer, decltype(&WebPDemuxDelete)> demux(WebPDemux(&data), WebPDemuxDelete);
        if (demux) {
            WebPChunkIterator chunk{};
            if (WebPDemuxGetChunk(demux.get(), "EXIF", 1, &chunk)) {
                orientation = exifOrientation({chunk.chunk.bytes, chunk.chunk.size});
                WebPDemuxReleaseChunkIterator(&chunk);
            }
            if (WebPDemuxGetChunk(demux.get(), "ICCP", 1, &chunk)) {
                factory->CreateColorContext(&profile);
                if (profile &&
                    FAILED(profile->InitializeFromMemory(chunk.chunk.bytes, (UINT)chunk.chunk.size)))
                    profile.Reset();
                WebPDemuxReleaseChunkIterator(&chunk);
            }
        }
        uint32_t width = originalW, height = originalH;
        if (!width || !height)
            throw Error("Некорректный размер WebP");
        uint32_t boundW = orientation >= 5 ? maxHeight : maxWidth,
                 boundH = orientation >= 5 ? maxWidth : maxHeight;
        double ratio =
            std::min({1.0, (double)std::max(1u, boundW) / width, (double)std::max(1u, boundH) / height,
                      std::sqrt((double)budget / (4.0 * width * height))});
        width = std::max(1u, (uint32_t)(width * ratio));
        height = std::max(1u, (uint32_t)(height * ratio));
        if (info.has_animation) {
            if ((uint64_t)originalW * originalH * 4 > budget)
                throw Error("Первый кадр анимированного WebP превышает лимит памяти");
            WebPAnimDecoderOptions opts;
            WebPAnimDecoderOptionsInit(&opts);
            opts.color_mode = MODE_BGRA;
            std::unique_ptr<WebPAnimDecoder, decltype(&WebPAnimDecoderDelete)> animation(
                WebPAnimDecoderNew(&data, &opts), WebPAnimDecoderDelete);
            uint8_t* p = nullptr;
            int timestamp = 0;
            if (!animation || !WebPAnimDecoderGetNext(animation.get(), &p, &timestamp))
                throw Error("Не удалось прочитать первый кадр WebP");
            width = originalW;
            height = originalH;
            webpPixels.assign(p, p + (size_t)width * height * 4);
        } else {
            WebPDecoderConfig cfg;
            WebPInitDecoderConfig(&cfg);
            cfg.output.colorspace = MODE_BGRA;
            cfg.options.use_scaling = 1;
            cfg.options.scaled_width = width;
            cfg.options.scaled_height = height;
            cfg.options.use_threads = 1;
            webpPixels.resize((size_t)width * height * 4);
            cfg.output.is_external_memory = 1;
            cfg.output.u.RGBA.rgba = webpPixels.data();
            cfg.output.u.RGBA.stride = width * 4;
            cfg.output.u.RGBA.size = webpPixels.size();
            auto status = WebPDecode(bytes->data(), bytes->size(), &cfg);
            WebPFreeDecBuffer(&cfg.output);
            if (status != VP8_STATUS_OK)
                throw Error("Не удалось декодировать WebP");
        }
        ComPtr<IWICBitmap> bitmap;
        check(factory->CreateBitmapFromMemory(width, height, GUID_WICPixelFormat32bppBGRA, width * 4,
                                              (UINT)webpPixels.size(), webpPixels.data(), &bitmap),
              "WebP bitmap");
        source = bitmap;
    } else {
        ComPtr<IWICStream> memory;
        check(factory->CreateStream(&memory), "Поток изображения");
        check(memory->InitializeFromMemory(const_cast<BYTE*>(bytes->data()), (DWORD)bytes->size()),
              "Данные изображения");
        stream = memory;
        check(
            factory->CreateDecoderFromStream(stream.Get(), nullptr, WICDecodeMetadataCacheOnDemand, &decoder),
            "Декодер изображения");
        ComPtr<IWICBitmapFrameDecode> frame;
        check(decoder->GetFrame(0, &frame), "Первый кадр");
        check(frame->GetSize(&originalW, &originalH), "Размер изображения");
        source = frame;
        ComPtr<IWICMetadataQueryReader> metadata;
        if (SUCCEEDED(frame->GetMetadataQueryReader(&metadata))) {
            PROPVARIANT value{};
            for (auto query : {L"/app1/ifd/{ushort=274}", L"/ifd/{ushort=274}"}) {
                if (SUCCEEDED(metadata->GetMetadataByName(query, &value))) {
                    if (value.vt == VT_UI2)
                        orientation = value.uiVal;
                    PropVariantClear(&value);
                    break;
                }
                PropVariantClear(&value);
            }
        }
        UINT contexts = 0;
        if (SUCCEEDED(frame->GetColorContexts(0, nullptr, &contexts)) && contexts) {
            factory->CreateColorContext(&profile);
            IWICColorContext* ctx = profile.Get();
            UINT actual = 0;
            if (FAILED(frame->GetColorContexts(1, &ctx, &actual)))
                profile.Reset();
        }
        // Ask the native decoder for reduced-resolution pixels before scaling.
        ComPtr<IWICBitmapSourceTransform> fast;
        if (SUCCEEDED(frame.As(&fast)) && originalW && originalH) {
            double ratio = std::min({1.0, (double)(orientation >= 5 ? maxHeight : maxWidth) / originalW,
                                     (double)(orientation >= 5 ? maxWidth : maxHeight) / originalH,
                                     std::sqrt((double)budget / (4.0 * originalW * originalH))});
            UINT w = std::max(1u, (UINT)(originalW * ratio)), h = std::max(1u, (UINT)(originalH * ratio));
            WICPixelFormatGUID format = GUID_WICPixelFormat32bppBGRA;
            bool supported =
                SUCCEEDED(fast->GetClosestSize(&w, &h)) && SUCCEEDED(fast->GetClosestPixelFormat(&format));
            UINT bpp = format == GUID_WICPixelFormat32bppBGRA  ? 4
                       : format == GUID_WICPixelFormat24bppBGR ? 3
                       : format == GUID_WICPixelFormat8bppGray ? 1
                                                               : 0;
            if (supported && bpp && (w < originalW || h < originalH) && (uint64_t)w * h * 4 <= budget) {
                transformPixels.resize((size_t)w * h * bpp);
                if (SUCCEEDED(fast->CopyPixels(nullptr, w, h, &format, WICBitmapTransformRotate0, w * bpp,
                                               (UINT)transformPixels.size(), transformPixels.data()))) {
                    ComPtr<IWICBitmap> bitmap;
                    check(factory->CreateBitmapFromMemory(w, h, format, w * bpp, (UINT)transformPixels.size(),
                                                          transformPixels.data(), &bitmap),
                          "Reduced bitmap");
                    source = bitmap;
                }
            }
        }
    }
    if (!originalW || !originalH || (uint64_t)originalW * originalH > 1000000000ull)
        throw Error("Недопустимый размер изображения");
    if (cancel && cancel())
        throw Cancelled();
    if (orientation > 1 && orientation <= 8) {
        ComPtr<IWICBitmapFlipRotator> rotated;
        check(factory->CreateBitmapFlipRotator(&rotated), "Ориентация");
        check(rotated->Initialize(source.Get(), transform(orientation)), "EXIF");
        source = rotated;
    }
    UINT w = 0, h = 0;
    source->GetSize(&w, &h);
    if (orientation >= 5 && orientation <= 8)
        std::swap(originalW, originalH);
    double ratio = std::min({1.0, (double)std::max(1u, maxWidth) / w, (double)std::max(1u, maxHeight) / h,
                             std::sqrt((double)budget / (4.0 * w * h))});
    if (jpeg && options.nativeJpegSize && (uint64_t)w * h * 4 <= budget)
        ratio = 1;
    UINT width = std::max(1u, (UINT)(w * ratio)), height = std::max(1u, (UINT)(h * ratio));
    if (width != w || height != h) {
        ComPtr<IWICBitmapScaler> scaler;
        check(factory->CreateBitmapScaler(&scaler), "Масштаб");
        check(scaler->Initialize(source.Get(), width, height, WICBitmapInterpolationModeFant),
              "Масштабирование");
        source = scaler;
    }
    if (profile) {
        ComPtr<IWICColorContext> srgb;
        factory->CreateColorContext(&srgb);
        if (srgb && SUCCEEDED(srgb->InitializeFromExifColorSpace(1))) {
            ComPtr<IWICColorTransform> color;
            factory->CreateColorTransformer(&color);
            if (color && SUCCEEDED(color->Initialize(source.Get(), profile.Get(), srgb.Get(),
                                                     GUID_WICPixelFormat32bppBGRA)))
                source = color;
        }
    }
    ComPtr<IWICFormatConverter> converter;
    check(factory->CreateFormatConverter(&converter), "Формат пикселей");
    check(converter->Initialize(source.Get(), GUID_WICPixelFormat32bppPBGRA, WICBitmapDitherTypeNone, nullptr,
                                0, WICBitmapPaletteTypeCustom),
          "PBGRA");
    auto result = std::make_shared<Frame>();
    result->width = width;
    result->height = height;
    result->originalWidth = originalW;
    result->originalHeight = originalH;
    result->pixels.resize((size_t)width * height * 4);
    check(converter->CopyPixels(nullptr, width * 4, (UINT)result->pixels.size(), result->pixels.data()),
          "Пиксели изображения");
    if (cancel && cancel())
        throw Cancelled();
    return result;
}
} // namespace viewer
