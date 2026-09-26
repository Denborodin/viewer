#include "renderer.h"
#include <cmath>
#include <wincodec.h>
namespace viewer {
void Renderer::snapshot(const ViewModel& model, const fs::path& path) {
    RECT r;
    GetClientRect(window_, &r);
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
          "Snapshot WIC");
    ComPtr<IWICBitmap> bitmap;
    check(wic->CreateBitmap(r.right, r.bottom, GUID_WICPixelFormat32bppPBGRA, WICBitmapCacheOnLoad, &bitmap),
          "Snapshot bitmap");
    discard();
    auto props = D2D1::RenderTargetProperties(
        D2D1_RENDER_TARGET_TYPE_SOFTWARE,
        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED), 96, 96);
    check(factory_->CreateWicBitmapRenderTarget(bitmap.Get(), props, &target_), "Snapshot target");
    check(target_->CreateSolidColorBrush(D2D1::ColorF(0xffffff), &brush_), "Snapshot brush");
    draw(model);
    ComPtr<IWICStream> stream;
    check(wic->CreateStream(&stream), "Snapshot stream");
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Snapshot file");
    ComPtr<IWICBitmapEncoder> encoder;
    check(wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder), "Snapshot PNG");
    check(encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache), "Snapshot encoder");
    ComPtr<IWICBitmapFrameEncode> frame;
    check(encoder->CreateNewFrame(&frame, nullptr), "Snapshot frame");
    check(frame->Initialize(nullptr), "Snapshot frame init");
    check(frame->WriteSource(bitmap.Get(), nullptr), "Snapshot pixels");
    check(frame->Commit(), "Snapshot commit");
    check(encoder->Commit(), "Snapshot save");
    discard();
}
namespace {
D2D1_COLOR_F color(uint32_t rgb, float a = 1) {
    return D2D1::ColorF(rgb, a);
}
} // namespace
Renderer::Renderer(HWND w) : window_(w) {
    check(D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, factory_.GetAddressOf()), "Direct2D");
    check(DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED, __uuidof(IDWriteFactory),
                              (IUnknown**)textFactory_.GetAddressOf()),
          "DirectWrite");
}
void Renderer::discard() {
    tiles_.clear();
    thumbBitmaps_.clear();
    uploaded_ = nullptr;
    brush_.Reset();
    target_.Reset();
    hwndTarget_.Reset();
}
void Renderer::ensure() {
    if (target_)
        return;
    RECT r;
    GetClientRect(window_, &r);
    auto props = D2D1::RenderTargetProperties();
    props.dpiX = props.dpiY = 96;
    auto hwnd =
        D2D1::HwndRenderTargetProperties(window_, D2D1::SizeU(std::max(1L, r.right), std::max(1L, r.bottom)),
                                         D2D1_PRESENT_OPTIONS_IMMEDIATELY);
    HRESULT hr = factory_->CreateHwndRenderTarget(props, hwnd, &hwndTarget_);
    if (FAILED(hr)) {
        props.type = D2D1_RENDER_TARGET_TYPE_SOFTWARE;
        check(factory_->CreateHwndRenderTarget(props, hwnd, &hwndTarget_), "Software renderer");
    }
    target_ = hwndTarget_;
    check(target_->CreateSolidColorBrush(color(0xffffff), &brush_), "Brush");
}
void Renderer::resize() {
    if (hwndTarget_) {
        RECT r;
        GetClientRect(window_, &r);
        if (FAILED(hwndTarget_->Resize(D2D1::SizeU(std::max(1L, r.right), std::max(1L, r.bottom)))))
            discard();
    }
}
D2D1_RECT_F Renderer::viewport(const ViewModel& m) const {
    RECT r;
    GetClientRect(window_, &r);
    return D2D1::RectF(m.sidebar ? 200 * m.dpi : 0, 44 * m.dpi, (float)r.right,
                       std::max(44 * m.dpi, (float)r.bottom - 30 * m.dpi));
}
float Renderer::fitZoom(const ViewModel& m) const {
    if (!m.frame)
        return 1;
    auto v = viewport(m);
    float w = (float)m.frame->originalWidth, h = (float)m.frame->originalHeight;
    if (m.rotation % 180)
        std::swap(w, h);
    return std::max(0.0001f,
                    std::min((v.right - v.left - 20 * m.dpi) / w, (v.bottom - v.top - 20 * m.dpi) / h));
}
void Renderer::text(std::wstring_view value, D2D1_RECT_F rect, D2D1_COLOR_F c, IDWriteTextFormat* format) {
    brush_->SetColor(c);
    target_->DrawTextW(value.data(), (UINT32)value.size(), format ? format : font_.Get(), rect, brush_.Get(),
                       D2D1_DRAW_TEXT_OPTIONS_CLIP);
}
void Renderer::upload(const Frame& f) {
    tiles_.clear();
    uploaded_ = nullptr;
    const UINT size = std::min(4096u, target_->GetMaximumBitmapSize());
    for (UINT y = 0; y < f.height; y += size)
        for (UINT x = 0; x < f.width; x += size) {
            UINT w = std::min(size, f.width - x), h = std::min(size, f.height - y);
            Tile t;
            t.rect = D2D1::RectF((float)x, (float)y, (float)(x + w), (float)(y + h));
            auto props = D2D1::BitmapProperties(
                D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
            check(target_->CreateBitmap(D2D1::SizeU(w, h), f.pixels.data() + ((size_t)y * f.width + x) * 4,
                                        f.width * 4, props, &t.bitmap),
                  "GPU bitmap");
            tiles_.push_back(std::move(t));
        }
    uploaded_ = &f;
}
void Renderer::draw(const ViewModel& m) {
    ensure();
    if (!font_ || fontDpi_ != m.dpi) {
        fontDpi_ = m.dpi;
        font_.Reset();
        small_.Reset();
        large_.Reset();
        textFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 14 * m.dpi,
                                       L"ru-ru", &font_);
        textFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_NORMAL,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 11 * m.dpi,
                                       L"ru-ru", &small_);
        textFactory_->CreateTextFormat(L"Segoe UI", nullptr, DWRITE_FONT_WEIGHT_SEMI_BOLD,
                                       DWRITE_FONT_STYLE_NORMAL, DWRITE_FONT_STRETCH_NORMAL, 28 * m.dpi,
                                       L"ru-ru", &large_);
        font_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
        small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    }
    auto size = target_->GetSize();
    auto v = viewport(m);
    if (m.frame && uploaded_ != m.frame.get())
        upload(*m.frame);
    target_->BeginDraw();
    target_->SetTransform(D2D1::Matrix3x2F::Identity());
    target_->Clear(color(0x111318));
    brush_->SetColor(color(0x1c2028));
    target_->FillRectangle(D2D1::RectF(0, 0, size.width, 44 * m.dpi), brush_.Get());
    const std::pair<const wchar_t*, float> commands[] = {
        {L"Открыть", 100},   {L"Миниатюры", 125}, {L"Вписать", 100},  {L"1:1", 65},
        {L"Повернуть", 115}, {L"Закладки", 110},  {L"Настройки", 130}};
    float x = 12 * m.dpi;
    for (auto [label, width] : commands) {
        text(label, D2D1::RectF(x, 0, x + width * m.dpi, 44 * m.dpi), color(0xdde3ee));
        x += width * m.dpi;
    }
    target_->PushAxisAlignedClip(v, D2D1_ANTIALIAS_MODE_ALIASED);
    if (m.frame) {
        float zoom = m.fit ? fitZoom(m) : m.zoom;
        float sx = zoom * m.frame->originalWidth / m.frame->width,
              sy = zoom * m.frame->originalHeight / m.frame->height;
        auto matrix =
            D2D1::Matrix3x2F::Translation(-(float)m.frame->width / 2.f, -(float)m.frame->height / 2.f) *
            D2D1::Matrix3x2F::Scale(sx, sy) * D2D1::Matrix3x2F::Rotation((float)m.rotation) *
            D2D1::Matrix3x2F::Translation((v.left + v.right) / 2 + m.panX, (v.top + v.bottom) / 2 + m.panY);
        target_->SetTransform(matrix);
        for (auto& tile : tiles_)
            target_->DrawBitmap(tile.bitmap.Get(), tile.rect, 1, D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
        target_->SetTransform(D2D1::Matrix3x2F::Identity());
    } else {
        text(L"Viewer", D2D1::RectF(v.left + 40 * m.dpi, v.top + 60 * m.dpi, v.right, v.top + 110 * m.dpi),
             color(0xf2f5fa), large_.Get());
        text(L"Перетащите сюда архив, папку или изображение",
             D2D1::RectF(v.left + 40 * m.dpi, v.top + 115 * m.dpi, v.right, v.top + 155 * m.dpi),
             color(0x929dad));
        text(L"ZIP · RAR · JPEG · PNG · WebP · BMP · GIF · TIFF",
             D2D1::RectF(v.left + 40 * m.dpi, v.top + 155 * m.dpi, v.right, v.top + 190 * m.dpi),
             color(0x647185));
    }
    target_->PopAxisAlignedClip();
    if (m.sidebar) {
        brush_->SetColor(color(0x191d25));
        target_->FillRectangle(D2D1::RectF(0, 44 * m.dpi, v.left, v.bottom), brush_.Get());
        target_->PushAxisAlignedClip(D2D1::RectF(0, v.top, v.left, v.bottom), D2D1_ANTIALIAS_MODE_ALIASED);
        size_t visible = (size_t)((v.bottom - v.top) / (146 * m.dpi)) + 1;
        for (auto it = thumbBitmaps_.begin(); it != thumbBitmaps_.end();) {
            if (it->first < m.thumbFirst || it->first >= m.thumbFirst + visible)
                it = thumbBitmaps_.erase(it);
            else
                ++it;
        }
        for (size_t row = 0; row < visible && m.thumbFirst + row < m.entries.size(); ++row) {
            size_t i = m.thumbFirst + row;
            float y = v.top + row * 146 * m.dpi;
            auto rect = D2D1::RectF(8 * m.dpi, y + 6 * m.dpi, v.left - 8 * m.dpi, y + 142 * m.dpi);
            brush_->SetColor(color(i == m.selected ? 0x29425e : 0x222833));
            target_->FillRoundedRectangle(D2D1::RoundedRect(rect, 5 * m.dpi, 5 * m.dpi), brush_.Get());
            auto t = m.thumbs.find(i);
            if (t != m.thumbs.end()) {
                auto& saved = thumbBitmaps_[i];
                if (saved.first.lock() != t->second) {
                    saved.second.Reset();
                    auto& f = *t->second;
                    auto props = D2D1::BitmapProperties(
                        D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM, D2D1_ALPHA_MODE_PREMULTIPLIED));
                    if (SUCCEEDED(target_->CreateBitmap(D2D1::SizeU(f.width, f.height), f.pixels.data(),
                                                        f.width * 4, props, &saved.second)))
                        saved.first = t->second;
                }
                if (saved.second) {
                    float w = t->second->width * m.dpi, h = t->second->height * m.dpi;
                    float ratio = std::min(170 * m.dpi / w, 104 * m.dpi / h);
                    w *= ratio;
                    h *= ratio;
                    target_->DrawBitmap(
                        saved.second.Get(),
                        D2D1::RectF((v.left - w) / 2, y + 12 * m.dpi, (v.left + w) / 2, y + 12 * m.dpi + h));
                }
            }
            text(std::to_wstring(i + 1) + L"  " + fs::path(m.entries[i].name).filename().wstring(),
                 D2D1::RectF(16 * m.dpi, y + 117 * m.dpi, v.left - 12 * m.dpi, y + 140 * m.dpi),
                 color(0xbdc7d6), small_.Get());
        }
        target_->PopAxisAlignedClip();
    } else
        thumbBitmaps_.clear();
    brush_->SetColor(color(0x1c2028));
    target_->FillRectangle(D2D1::RectF(0, size.height - 30 * m.dpi, size.width, size.height), brush_.Get());
    std::wstring status = m.loading ? L"Загрузка…  " + m.status : m.status;
    if (m.frame && !m.loading) {
        float z = m.fit ? fitZoom(m) : m.zoom;
        status += L"     " + std::to_wstring(m.frame->originalWidth) + L" × " +
                  std::to_wstring(m.frame->originalHeight) + L"     " +
                  std::to_wstring((int)std::round(z * 100)) + L"%";
        if (m.bookmarked)
            status += L"     ★";
    }
    text(status, D2D1::RectF(12 * m.dpi, size.height - 30 * m.dpi, size.width - 12 * m.dpi, size.height),
         color(0xaab6c7), small_.Get());
    HRESULT hr = target_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET)
        discard();
    else
        check(hr, "Отрисовка");
}
} // namespace viewer
