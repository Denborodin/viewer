#pragma once
#include "common.h"
#include <d2d1.h>
#include <dwrite.h>
namespace viewer {
struct ViewModel {
    std::shared_ptr<Frame> frame;
    std::unordered_map<size_t, std::shared_ptr<Frame>> thumbs;
    std::vector<Entry> entries;
    size_t selected = 0, thumbFirst = 0;
    bool sidebar = false, fit = true, bookmarked = false, loading = false;
    float zoom = 1, panX = 0, panY = 0, dpi = 1;
    int rotation = 0;
    std::wstring status = L"Откройте изображение, папку, ZIP или RAR";
    std::wstring sourceName;
};
class Renderer {
  public:
    explicit Renderer(HWND window);
    void draw(const ViewModel& model);
    void resize();
    void snapshot(const ViewModel& model, const fs::path& path);
    D2D1_RECT_F viewport(const ViewModel& model) const;
    float fitZoom(const ViewModel& model) const;

  private:
    HWND window_;
    ComPtr<ID2D1Factory> factory_;
    ComPtr<IDWriteFactory> textFactory_;
    ComPtr<ID2D1RenderTarget> target_;
    ComPtr<ID2D1HwndRenderTarget> hwndTarget_;
    ComPtr<ID2D1SolidColorBrush> brush_;
    ComPtr<IDWriteTextFormat> font_, small_, large_;
    struct Tile {
        ComPtr<ID2D1Bitmap> bitmap;
        D2D1_RECT_F rect;
    };
    std::vector<Tile> tiles_;
    const Frame* uploaded_ = nullptr;
    float fontDpi_ = 0;
    std::unordered_map<size_t, std::pair<std::weak_ptr<Frame>, ComPtr<ID2D1Bitmap>>> thumbBitmaps_;
    void ensure();
    void discard();
    void upload(const Frame& frame);
    void text(std::wstring_view value, D2D1_RECT_F rect, D2D1_COLOR_F color,
              IDWriteTextFormat* format = nullptr);
};
} // namespace viewer
