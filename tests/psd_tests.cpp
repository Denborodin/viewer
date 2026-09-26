#include "decode.h"
#include <iostream>
#include <zlib.h>
using namespace viewer;
namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw Error(message);
}
void u16(Bytes& b, unsigned v) {
    b.push_back(uint8_t(v >> 8));
    b.push_back(uint8_t(v));
}
void u32(Bytes& b, uint32_t v) {
    u16(b, v >> 16);
    u16(b, v);
}
void tag(Bytes& b, const char* s) {
    b.insert(b.end(), s, s + 4);
}
void block(Bytes& b, const Bytes& v) {
    u32(b, (uint32_t)v.size());
    b.insert(b.end(), v.begin(), v.end());
}
void resource(Bytes& b, unsigned id, const Bytes& data) {
    tag(b, "8BIM");
    u16(b, id);
    u16(b, 0);
    block(b, data);
    if (data.size() & 1)
        b.push_back(0);
}
Bytes fixture(unsigned depth, unsigned mode, unsigned compression, bool alpha = false,
              bool savedAlpha = false, const Bytes& icc = {}, int transparentIndex = -1, bool merged = true) {
    constexpr unsigned w = 7, h = 5;
    unsigned base = mode == 3 ? 3 : 1, channels = base + (alpha || savedAlpha ? 1 : 0);
    Bytes b;
    tag(b, "8BPS");
    u16(b, 1);
    b.resize(12);
    u16(b, channels);
    u32(b, h);
    u32(b, w);
    u16(b, depth);
    u16(b, mode);
    Bytes palette;
    if (mode == 2) {
        palette.resize(768);
        for (unsigned i = 0; i < 256; ++i) {
            palette[i] = uint8_t(i);
            palette[256 + i] = uint8_t(255 - i);
            palette[512 + i] = 50;
        }
    }
    block(b, palette);
    Bytes resources;
    if (!icc.empty())
        resource(resources, 1039, icc);
    if (transparentIndex >= 0) {
        Bytes value;
        u16(value, (unsigned)transparentIndex);
        resource(resources, 1047, value);
    }
    if (!merged) {
        Bytes value;
        u32(value, 1);
        value.push_back(0);
        resource(resources, 1057, value);
    }
    block(b, resources);
    Bytes layers;
    if (alpha) {
        Bytes info;
        u16(info, 65535);
        block(layers, info);
        u32(layers, 0);
    }
    block(b, layers);
    u16(b, compression);
    Bytes planes;
    std::vector<Bytes> rows;
    for (unsigned c = 0; c < channels; ++c)
        for (unsigned y = 0; y < h; ++y) {
            Bytes row;
            for (unsigned x = 0; x < w; ++x) {
                unsigned value = c == base ? 128 : (30 + c * 40 + x * 3 + y * 5);
                if (alpha && c < base)
                    value = 127 + (value * 128 + 127) / 255; // Photoshop's white matte.
                if (depth == 8)
                    row.push_back(uint8_t(value));
                else
                    u16(row, value * 257);
            }
            if (compression == 3) {
                for (unsigned x = w - 1; x > 0; --x) {
                    if (depth == 8)
                        row[x] -= row[x - 1];
                    else {
                        unsigned value =
                            ((row[x * 2] << 8) | row[x * 2 + 1]) - ((row[x * 2 - 2] << 8) | row[x * 2 - 1]);
                        row[x * 2] = uint8_t(value >> 8);
                        row[x * 2 + 1] = uint8_t(value);
                    }
                }
            }
            planes.insert(planes.end(), row.begin(), row.end());
            rows.push_back(std::move(row));
        }
    if (compression == 0)
        b.insert(b.end(), planes.begin(), planes.end());
    else if (compression == 1) {
        for (auto& row : rows)
            u16(b, (unsigned)row.size() + 2);
        for (auto& row : rows) {
            b.push_back(128);
            b.push_back(uint8_t(row.size() - 1));
            b.insert(b.end(), row.begin(), row.end());
        }
    } else {
        uLongf length = compressBound((uLong)planes.size());
        Bytes zipped(length);
        if (compress2(zipped.data(), &length, planes.data(), (uLong)planes.size(), 6) != Z_OK)
            throw Error("compress");
        b.insert(b.end(), zipped.begin(), zipped.begin() + length);
    }
    return b;
}
std::shared_ptr<Frame> decode(const Bytes& b, uint32_t w = 7, uint32_t h = 5, uint64_t budget = MiB,
                              const Cancel& cancel = {}) {
    return decodeImage(std::make_shared<Bytes>(b), w, h, budget, cancel);
}
bool rejects(const Bytes& b) {
    try {
        decode(b);
        return false;
    } catch (const Error&) {
        return true;
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        fs::path out = argc > 1 ? argv[1] : L"psd-fixtures";
        fs::create_directories(out);
        for (unsigned mode : {1u, 2u, 3u})
            for (unsigned depth : {8u, 16u}) {
                if (mode == 2 && depth == 16)
                    continue;
                auto expected = decode(fixture(depth, mode, 0));
                require(expected->width == 7 && expected->height == 5, "PSD size");
                require(expected->pixels[2] == 30 && expected->pixels[3] == 255,
                        "PSD color and opaque alpha");
                for (unsigned compression = 0; compression < 4; ++compression) {
                    auto bytes = fixture(depth, mode, compression);
                    auto frame = decode(bytes);
                    require(frame->pixels == expected->pixels, "All PSD compression modes agree");
                    auto reduced = decode(bytes, 3, 2, 24);
                    require(reduced->width <= 3 && reduced->height <= 2 && reduced->pixels.size() <= 24 &&
                                reduced->originalWidth == 7,
                            "PSD viewport and memory budget");
                    require(reduced->pixels[2] == 40, "PSD box reduction averages pixels");
                    writeFileAtomic(out / (std::to_wstring(mode) + L"-" + std::to_wstring(depth) + L"-" +
                                           std::to_wstring(compression) + L".psd"),
                                    bytes);
                    auto damaged = bytes;
                    damaged.pop_back();
                    require(rejects(damaged), "Truncated PSD rejected");
                }
            }
        for (unsigned depth : {8u, 16u})
            for (unsigned compression = 0; compression < 4; ++compression) {
                auto frame = decode(fixture(depth, 3, compression, true));
                require(frame->pixels[3] == 128 && abs(int(frame->pixels[2]) - 15) <= 1,
                        "PSD transparency and white matte");
                auto saved = decode(fixture(depth, 3, compression, false, true));
                require(saved->pixels[3] == 255, "Saved alpha is not composite transparency");
            }
        writeFileAtomic(out / L"transparent.psd", fixture(8, 3, 1, true));
        auto bytes = fixture(8, 3, 2);
        for (size_t n = 0; n < bytes.size(); ++n)
            require(rejects(Bytes(bytes.begin(), bytes.begin() + n)), "Every truncated ZIP PSD rejected");
        auto bad = bytes;
        bad[5] = 2;
        require(rejects(bad), "PSB rejected");
        bad = bytes;
        bad[25] = 4;
        require(rejects(bad), "CMYK rejected explicitly");
        bad = bytes;
        bad[23] = 32;
        require(rejects(bad), "HDR rejected explicitly");
        bad = bytes;
        bad[18] = 0xff;
        require(rejects(bad), "Oversized dimensions rejected");
        bad = bytes;
        bad.back() ^= 1;
        require(rejects(bad), "ZIP checksum validated");
        bool cancelled = false;
        int calls = 0;
        try {
            decode(bytes, 7, 5, MiB, [&] { return ++calls > 3; });
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "PSD cancellation during rows");
        require(decode(bytes, 7, 5, 4)->pixels.size() == 4, "PSD tiny budget");
        require(rejects(fixture(8, 3, 0, false, false, {}, -1, false)), "Missing merged composite rejected");
        auto indexed = decode(fixture(8, 2, 1, false, false, {}, 30));
        require(indexed->pixels[3] == 0 && indexed->pixels[7] == 255,
                "Indexed transparency uses palette index");
        auto repeated = fixture(8, 1, 1);
        // Replace the RLE table/body with five rows of the PackBits repeat packet.
        repeated.resize(40);
        for (int y = 0; y < 5; ++y)
            u16(repeated, 2);
        for (int y = 0; y < 5; ++y) {
            repeated.push_back(250);
            repeated.push_back(77);
        }
        auto repeatFrame = decode(repeated);
        require(repeatFrame->pixels[2] == 77 && repeatFrame->pixels[repeatFrame->pixels.size() - 2] == 77,
                "PackBits repeated runs");
        repeated.back() = 42;
        repeated[50] = 249;
        require(rejects(repeated), "PackBits row overrun rejected");
        auto path = fs::path(L"C:\\Windows\\System32\\spool\\drivers\\color\\sRGB Color Space Profile.icm");
        auto profile = readFile(path);
        auto plain = decode(fixture(8, 3, 0));
        auto managed = decode(fixture(8, 3, 0, false, false, *profile));
        int error = 0;
        for (size_t i = 0; i < plain->pixels.size(); ++i)
            error = std::max(error, abs(int(plain->pixels[i]) - managed->pixels[i]));
        require(error <= 2, "PSD sRGB ICC");
        require(isImage(L"Пример.PSD"), "PSD extension accepted");
        require(!isImage(L"Пример.PSB"), "Unsupported PSB not advertised");
        if (argc > 2)
            for (auto& entry : fs::directory_iterator(argv[2])) {
                if (lower(entry.path().extension().wstring()) != L".psd")
                    continue;
                auto f = decodeImage(readFile(entry.path()), 30000, 30000, 128 * MiB);
                writeFileAtomic(entry.path().wstring() + L".bgra", f->pixels);
                require(f->width && f->height, "Independent PSD fixture decoded");
                std::cout << utf8(entry.path().filename().wstring()) << " " << f->width << "x" << f->height
                          << '\n';
            }
        std::cout << "PASS " << assertions << " PSD assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL " << e.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
