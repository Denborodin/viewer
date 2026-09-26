#include "decode.h"
#include <turbojpeg.h>
#include <iostream>
#include <cmath>
using namespace viewer;
int assertions = 0;
void require(bool condition, const char* reason) {
    ++assertions;
    if (!condition)
        throw Error(reason);
}
BytePtr encode(int format, bool progressive = false) {
    const int w = 97, h = 65, bpp = tjPixelSize[format];
    Bytes pixels(w * h * bpp);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x) {
            auto* p = pixels.data() + (y * w + x) * bpp;
            for (int c = 0; c < bpp; ++c)
                p[c] = (uint8_t)((x / 16 * 31 + y / 16 * 43 + c * 57) % 220);
            if (format == TJPF_BGRA)
                p[3] = 255;
        }
    auto handle = tj3Init(TJINIT_COMPRESS);
    if (!handle)
        throw Error("JPEG encoder init");
    tj3Set(handle, TJPARAM_QUALITY, 95);
    tj3Set(handle, TJPARAM_SUBSAMP, format == TJPF_GRAY ? TJSAMP_GRAY : TJSAMP_444);
    tj3Set(handle, TJPARAM_PROGRESSIVE, progressive);
    unsigned char* raw = nullptr;
    size_t size = 0;
    int result = tj3Compress8(handle, pixels.data(), w, w * bpp, h, format, &raw, &size);
    std::string error = tj3GetErrorStr(handle);
    tj3Destroy(handle);
    std::unique_ptr<unsigned char, decltype(&tj3Free)> owned(raw, tj3Free);
    if (result < 0)
        throw Error(error);
    return std::make_shared<Bytes>(raw, raw + size);
}
BytePtr metadata(BytePtr original, unsigned orientation, BytePtr profile = {}) {
    auto result = std::make_shared<Bytes>(original->begin(), original->begin() + 2);
    auto marker = [&](uint8_t id, const Bytes& payload) {
        size_t n = payload.size() + 2;
        result->insert(result->end(), {0xff, id, (uint8_t)(n >> 8), (uint8_t)n});
        result->insert(result->end(), payload.begin(), payload.end());
    };
    marker(0xe1, Bytes{'E', 'x',  'i', 'f', 0,
                       0,   'I',  'I', 42,  0,
                       8,   0,    0,   0,   1,
                       0,   0x12, 1,   3,   0,
                       1,   0,    0,   0,   (uint8_t)orientation,
                       0,   0,    0,   0,   0,
                       0,   0});
    if (profile) { // Deliberately reversed chunk order tests ICC assembly by sequence number.
        size_t half = (profile->size() + 1) / 2;
        for (int sequence : {2, 1}) {
            Bytes chunk{'I', 'C', 'C', '_', 'P', 'R', 'O', 'F', 'I', 'L', 'E', 0, (uint8_t)sequence, 2};
            size_t begin = sequence == 1 ? 0 : half, end = sequence == 1 ? half : profile->size();
            chunk.insert(chunk.end(), profile->begin() + begin, profile->begin() + end);
            marker(0xe2, chunk);
        }
    }
    result->insert(result->end(), original->begin() + 2, original->end());
    return result;
}
void compare(const Frame& a, const Frame& b) {
    require(a.width == b.width && a.height == b.height, "JPEG dimensions match WIC");
    double error = 0;
    for (size_t i = 0; i < a.pixels.size(); ++i)
        error += std::abs((int)a.pixels[i] - (int)b.pixels[i]);
    require(error / a.pixels.size() < 2, "JPEG pixels remain within decoder rounding tolerance");
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        {
            ImageDecoder decoder;
            auto rgb = encode(TJPF_BGRA), gray = encode(TJPF_GRAY), progressive = encode(TJPF_BGRA, true),
                 cmyk = encode(TJPF_CMYK);
            for (auto bytes : {rgb, gray, progressive}) {
                auto fast = decoder.decode(bytes, 1000, 1000, MiB, {}, {JpegBackend::Turbo, true});
                auto reference = decoder.decode(bytes, 1000, 1000, MiB, {}, {JpegBackend::Wic, false});
                compare(*fast, *reference);
                for (size_t i = 3; i < fast->pixels.size(); i += 4)
                    if (fast->pixels[i] != 255)
                        throw Error("JPEG alpha must be opaque");
                require(fast->originalWidth == 97 && fast->originalHeight == 65,
                        "Odd JPEG dimensions preserved");
                auto reduced = decoder.decode(bytes, 28, 20, MiB, {}, {JpegBackend::Turbo, true});
                require(reduced->width < 97 && reduced->width >= 28 && reduced->height >= 18,
                        "Native DCT reduced JPEG above viewport");
                auto tiny = decoder.decode(bytes, 1000, 1000, 128, {}, {JpegBackend::Automatic, true});
                require(tiny->pixels.size() <= 128, "Tiny JPEG budget falls back safely");
            }
            wchar_t windows[MAX_PATH];
            GetWindowsDirectoryW(windows, MAX_PATH);
            auto profile =
                readFile(fs::path(windows) / L"System32/spool/drivers/color/sRGB Color Space Profile.icm");
            for (unsigned orientation = 1; orientation <= 8; ++orientation) {
                auto bytes = metadata(rgb, orientation, profile);
                auto fast = decoder.decode(bytes, 1000, 1000, MiB, {}, {JpegBackend::Turbo, true});
                auto reference = decoder.decode(bytes, 1000, 1000, MiB, {}, {JpegBackend::Wic, false});
                compare(*fast, *reference);
                require(fast->originalWidth == (orientation >= 5 ? 65 : 97), "All EXIF orientations");
            }
            auto afterProfile = decoder.decode(rgb, 1000, 1000, MiB);
            auto fresh = decodeImage(rgb, 1000, 1000, MiB);
            require(afterProfile->pixels == fresh->pixels, "ICC does not leak to the next JPEG");
            auto fallback = decoder.decode(cmyk, 1000, 1000, MiB);
            auto reference = decoder.decode(cmyk, 1000, 1000, MiB, {}, {JpegBackend::Wic, false});
            require(fallback->pixels == reference->pixels, "CMYK preserves the WIC fallback");
            bool rejected = false;
            try {
                decoder.decode(cmyk, 1000, 1000, MiB, {}, {JpegBackend::Turbo, true});
            } catch (const Error&) {
                rejected = true;
            }
            require(rejected, "CMYK not silently treated as RGB");
            auto malformed = std::make_shared<Bytes>(Bytes{0xff, 0xd8, 0xff, 0xda, 0, 1, 0xff, 0xd9});
            rejected = false;
            try {
                decoder.decode(malformed, 1000, 1000, MiB);
            } catch (const Error&) {
                rejected = true;
            }
            require(rejected, "Malformed JPEG rejected");
            require(decoder.decode(rgb, 1000, 1000, MiB)->pixels == fresh->pixels,
                    "Decoder recovers after malformed JPEG");
            rejected = false;
            try {
                decoder.decode(rgb, 1000, 1000, MiB, [] { return true; });
            } catch (const Cancelled&) {
                rejected = true;
            }
            require(rejected, "JPEG cancellation");
            if (argc > 1) {
                fs::path out = argv[1];
                fs::create_directories(out);
                writeFileAtomic(out / L"rgb.jpg", *rgb);
                writeFileAtomic(out / L"cmyk.jpg", *cmyk);
                writeFileAtomic(out / L"icc-exif.jpg", *metadata(rgb, 6, profile));
            }
        }
        std::cout << "PASS " << assertions << " JPEG assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
