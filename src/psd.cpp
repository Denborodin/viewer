#include "psd.h"
#include <cmath>
#include <zlib.h>
namespace viewer {
namespace {
struct Reader {
    std::span<const uint8_t> data;
    std::span<const uint8_t> take(size_t n) {
        if (n > data.size())
            throw Error("PSD: файл повреждён или отсутствует сведённое изображение");
        auto part = data.first(n);
        data = data.subspan(n);
        return part;
    }
    uint8_t byte() {
        return take(1)[0];
    }
    uint16_t u16() {
        auto p = take(2);
        return (p[0] << 8) | p[1];
    }
    uint32_t u32() {
        auto a = u16();
        return (uint32_t(a) << 16) | u16();
    }
    Reader block() {
        auto n = u32();
        return {take(n)};
    }
};
bool tag(std::span<const uint8_t> p, const char* value) {
    return p.size() == 4 && memcmp(p.data(), value, 4) == 0;
}
void packbits(std::span<const uint8_t> src, Bytes& out) {
    Reader in{src};
    size_t pos = 0;
    while (!in.data.empty()) {
        int n = in.byte();
        if (n == 128)
            continue;
        size_t count = n < 128 ? n + 1 : 257 - n;
        if (count > out.size() - pos)
            throw Error("PSD: неверная длина RLE-строки");
        if (n < 128) {
            auto p = in.take(count);
            std::copy(p.begin(), p.end(), out.begin() + pos);
        } else {
            auto value = in.byte();
            std::fill_n(out.begin() + pos, count, value);
        }
        pos += count;
    }
    if (pos != out.size())
        throw Error("PSD: неполная RLE-строка");
}
struct Inflater {
    z_stream z{};
    bool active = false, ended = false;
    ~Inflater() {
        if (active)
            inflateEnd(&z);
    }
    void start(std::span<const uint8_t> data) {
        if (data.size() > UINT_MAX || inflateInit(&z) != Z_OK)
            throw Error("PSD: ошибка ZIP-декодера");
        active = true;
        z.next_in = const_cast<Bytef*>(data.data());
        z.avail_in = (uInt)data.size();
    }
    void row(Bytes& bytes) {
        if (ended)
            throw Error("PSD: неполные ZIP-данные");
        z.next_out = bytes.data();
        z.avail_out = (uInt)bytes.size();
        while (z.avail_out) {
            auto before = z.avail_in;
            auto output = z.avail_out;
            int status = inflate(&z, Z_NO_FLUSH);
            if (status == Z_STREAM_END) {
                ended = true;
                break;
            }
            if (status != Z_OK || (before == z.avail_in && output == z.avail_out))
                throw Error("PSD: повреждённые ZIP-данные");
        }
        if (z.avail_out)
            throw Error("PSD: неполная ZIP-строка");
    }
    void finish() {
        if (!ended) {
            uint8_t extra;
            z.next_out = &extra;
            z.avail_out = 1;
            int status = inflate(&z, Z_FINISH);
            if (status != Z_STREAM_END || z.avail_out != 1)
                throw Error("PSD: неверная длина или контрольная сумма ZIP");
        }
        if (z.avail_in)
            throw Error("PSD: лишние данные ZIP");
    }
};
} // namespace
PsdImage decodePsd(std::span<const uint8_t> bytes, uint32_t maxW, uint32_t maxH, uint64_t budget,
                   const Cancel& cancel) {
    Reader in{bytes};
    if (!tag(in.take(4), "8BPS") || in.u16() != 1)
        throw Error("Поддерживаются PSD; формат PSB пока не поддерживается");
    auto reserved = in.take(6);
    if (std::any_of(reserved.begin(), reserved.end(), [](auto b) { return b != 0; }))
        throw Error("PSD: неверный заголовок");
    unsigned channels = in.u16();
    uint32_t h = in.u32(), w = in.u32();
    unsigned depth = in.u16(), mode = in.u16();
    if (!w || !h || w > 30000 || h > 30000 || channels < 1 || channels > 56)
        throw Error("PSD: недопустимые размеры или каналы");
    if ((depth != 8 && depth != 16) || (mode != 1 && mode != 2 && mode != 3) || (mode == 2 && depth != 8))
        throw Error("PSD: поддерживаются RGB/Grayscale 8/16 бит и Indexed 8 бит; CMYK, Lab и HDR пока не "
                    "поддерживаются");
    unsigned base = mode == 3 ? 3 : 1;
    if (channels < base || budget < 4)
        throw Error("PSD: недостаточно каналов или памяти");
    auto palette = in.block().data;
    if (mode == 2 && palette.size() != 768)
        throw Error("PSD: неверная палитра");
    PsdImage result;
    result.grayscale = mode == 1;
    int transparentIndex = -1;
    auto resources = in.block();
    while (!resources.data.empty()) {
        if (!tag(resources.take(4), "8BIM"))
            throw Error("PSD: неверный блок ресурсов");
        auto id = resources.u16();
        size_t name = resources.byte();
        resources.take(name);
        if ((name + 1) & 1)
            resources.take(1);
        auto resource = resources.block();
        if (resource.data.size() & 1)
            resources.take(1);
        if (id == 1039)
            result.icc.assign(resource.data.begin(), resource.data.end());
        if (id == 1047 && resource.data.size() >= 2)
            transparentIndex = resource.u16();
        if (id == 1057 && resource.data.size() >= 5) {
            resource.u32();
            if (!resource.byte())
                throw Error("PSD не содержит сведённого изображения. Сохраните в Photoshop с «Максимальной "
                            "совместимостью».");
        }
    }
    bool alpha = false;
    auto layers = in.block();
    if (!layers.data.empty()) {
        auto info = layers.block();
        if (info.data.size() >= 2)
            alpha = (int16_t)info.u16() < 0;
        if (!layers.data.empty()) {
            layers.block(); // Global layer mask.
            while (layers.data.size() >= 12) {
                auto signature = layers.take(4);
                if (!tag(signature, "8BIM") && !tag(signature, "8B64"))
                    break;
                auto key = layers.take(4);
                auto item = layers.block();
                if (item.data.size() & 1)
                    layers.take(1);
                if (tag(key, "Mtrn") || tag(key, "Mt16"))
                    alpha = true;
                if (tag(key, "Lr16") && item.data.size() >= 2)
                    alpha = alpha || (int16_t)item.u16() < 0;
            }
        }
    }
    if (alpha && channels <= base)
        throw Error("PSD: отсутствует канал прозрачности");
    unsigned compression = in.u16();
    if (compression > 3)
        throw Error("PSD: неизвестное сжатие");
    Reader lengths{};
    if (compression == 1)
        lengths = {in.take(size_t(channels) * h * 2)};
    double scale = std::min({1.0, double(std::max(1u, maxW)) / w, double(std::max(1u, maxH)) / h,
                             std::sqrt(double(budget) / (4.0 * w * h))});
    auto f = std::make_shared<Frame>();
    f->originalWidth = w;
    f->originalHeight = h;
    f->width = std::max(1u, uint32_t(w * scale));
    f->height = std::max(1u, uint32_t(h * scale));
    // Extreme aspect ratios need a second clamp after the one-pixel minimum.
    while (uint64_t(f->width) * f->height * 4 > budget) {
        if (f->width > f->height)
            --f->width;
        else
            --f->height;
    }
    f->pixels.resize(size_t(f->width) * f->height * 4, 255);
    Bytes row(size_t(w) * (depth / 8));
    std::vector<uint64_t> sums(size_t(f->width) * 4);
    std::vector<uint32_t> xmap(w), counts(f->width);
    for (uint32_t x = 0; x < w; ++x) {
        xmap[x] = uint32_t(uint64_t(x) * f->width / w);
        ++counts[xmap[x]];
    }
    Inflater zip;
    if (compression >= 2)
        zip.start(in.data);
    for (unsigned c = 0; c < channels; ++c) {
        uint32_t rowCount = 0;
        bool used = c < base || (alpha && c == base);
        for (uint32_t y = 0; y < h; ++y) {
            if (cancel && cancel())
                throw Cancelled();
            if (compression == 0) {
                auto data = in.take(row.size());
                std::copy(data.begin(), data.end(), row.begin());
            } else if (compression == 1)
                packbits(in.take(lengths.u16()), row);
            else
                zip.row(row);
            if (!used)
                continue;
            if (compression == 3) {
                if (depth == 8)
                    for (size_t i = 1; i < row.size(); ++i)
                        row[i] += row[i - 1];
                else {
                    uint16_t previous = 0;
                    for (size_t i = 0; i < row.size(); i += 2) {
                        uint16_t value = uint16_t((row[i] << 8) | row[i + 1]) + previous;
                        row[i] = uint8_t(value >> 8);
                        row[i + 1] = uint8_t(value);
                        previous = value;
                    }
                }
            }
            for (uint32_t x = 0; x < w; ++x) {
                unsigned value = depth == 8 ? row[x] : (((row[2 * x] << 8) | row[2 * x + 1]) + 128) / 257;
                auto dx = xmap[x];
                if (mode == 2 && c == 0) {
                    bool opaque = int(value) != transparentIndex;
                    for (unsigned k = 0; k < 3; ++k)
                        sums[4 * dx + k] += opaque ? palette[k * 256 + value] : 0;
                    sums[4 * dx + 3] += opaque ? 255 : 0;
                } else
                    sums[4 * dx] += value;
            }
            ++rowCount;
            uint32_t dy = uint32_t(uint64_t(y) * f->height / h);
            if (y + 1 < h && uint64_t(y + 1) * f->height / h == dy)
                continue;
            for (uint32_t x = 0; x < f->width; ++x) {
                auto p = &f->pixels[(size_t(dy) * f->width + x) * 4];
                uint64_t n = uint64_t(counts[x]) * rowCount;
                auto value = uint8_t((sums[4 * x] + n / 2) / n);
                if (c == base)
                    p[3] = value;
                else if (mode == 3)
                    p[2 - c] = value;
                else if (mode == 1)
                    p[0] = p[1] = p[2] = value;
                else {
                    for (unsigned k = 0; k < 3; ++k)
                        p[2 - k] = uint8_t((sums[4 * x + k] + n / 2) / n);
                    if (!alpha)
                        p[3] = uint8_t((sums[4 * x + 3] + n / 2) / n);
                }
            }
            std::fill(sums.begin(), sums.end(), 0);
            rowCount = 0;
        }
    }
    if (compression >= 2)
        zip.finish();
    for (size_t i = 0; i < f->pixels.size(); i += 4) {
        auto p = &f->pixels[i];
        int a = p[3];
        if (a == 0)
            p[0] = p[1] = p[2] = 0;
        else if (a < 255)
            for (unsigned k = 0; k < 3; ++k) {
                // RGB/gray composite data is matted against white; palette values are premultiplied above.
                int value = mode == 2 ? p[k] * 255 / a : (int(p[k]) + a - 255) * 255 / a;
                p[k] = uint8_t(std::clamp(value, 0, 255));
            }
    }
    result.frame = std::move(f);
    return result;
}
} // namespace viewer
