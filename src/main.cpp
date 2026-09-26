#include "pipeline.h"
#include "file_tree.h"
#include "renderer.h"
#include "resource.h"
#include "shell_integration.h"
#include <cmath>
#include <commctrl.h>
#include <dwmapi.h>
#include <fstream>
#include <shellapi.h>
#include <shobjidl.h>
#include <wincodec.h>
#include <windowsx.h>
namespace viewer {
constexpr UINT WM_EVENT = WM_APP + 1, WM_SHELL = WM_APP + 2;
enum Command {
    Open = 400,
    OpenFolder,
    Exit,
    Previous,
    Next,
    Fit,
    Actual,
    Rotate,
    Fullscreen,
    Thumbnails,
    ToggleBookmark,
    Bookmarks,
    Preferences,
    About,
    ToggleFileTree,
    PreviousArchive,
    NextArchive
};
struct App {
    HWND window = nullptr, settingsWindow = nullptr;
    HINSTANCE instance;
    ViewModel model;
    std::shared_ptr<StateStore> state;
    std::unique_ptr<ImagePipeline> pipeline;
    std::unique_ptr<Renderer> renderer;
    std::unique_ptr<FileTree> fileTree;
    fs::path currentPath;
    fs::path sourcePath;
    Settings settings;
    fs::path initialPath, testOutput;
    std::optional<uint32_t> initialEntry;
    bool full = false, drag = false, shellBusy = false, closing = false, testWritten = false;
    POINT dragStart{};
    float oldPanX = 0, oldPanY = 0;
    WINDOWPLACEMENT placement{sizeof(placement)};
    DWORD savedStyle = 0;
    std::thread shellThread;
    std::vector<Bookmark> bookmarks;
    std::chrono::steady_clock::time_point start = std::chrono::steady_clock::now();
    uint64_t requestedToken = 0;
    explicit App(HINSTANCE i) : instance(i) {}
    ~App() {
        closing = true;
        pipeline.reset();
        if (shellThread.joinable())
            shellThread.join();
    }
    void invalidate() {
        InvalidateRect(window, nullptr, FALSE);
    }
    int restoreWindow(int show) {
        if (!testOutput.empty())
            return show;
        auto saved = state->windowPlacement();
        if (!saved)
            return show;
        int restored = (int)saved->showCmd;
        saved->showCmd = SW_HIDE; // Restore geometry before the first visible frame.
        if (!SetWindowPlacement(window, &*saved))
            return show;
        // Preserve explicit hidden/minimized startup requests; restore normal/maximized launches.
        return show == SW_SHOWNORMAL || show == SW_SHOW || show == SW_SHOWDEFAULT ? restored : show;
    }
    void open(const fs::path& path, std::optional<uint32_t> entry = {}) {
        currentPath = fs::absolute(path).lexically_normal();
        if (fileTree)
            fileTree->location(currentPath);
        model.loading = true;
        model.status = L"Открытие: " + path.filename().wstring();
        model.thumbs.clear();
        model.entries.clear();
        model.thumbFirst = 0;
        model.rotation = 0;
        model.fit = true;
        model.panX = model.panY = 0;
        decodeSize();
        requestedToken = pipeline->open(path, entry);
        invalidate();
    }
    void layoutTree() {
        bool visible = settings.fileTree && !full;
        model.treeWidth = visible ? 240 * model.dpi : 0;
        if (fileTree)
            fileTree->layout(model.dpi, visible);
    }
    void archive(int direction) {
        if (fileTree)
            fileTree->jump(currentPath, direction);
    }
    void decodeSize() {
        if (!pipeline || !renderer)
            return;
        auto v = renderer->viewport(model);
        uint32_t w = (uint32_t)std::max(1.f, v.right - v.left), h = (uint32_t)std::max(1.f, v.bottom - v.top);
        if (model.frame && !model.fit) {
            float z = model.zoom;
            w = (uint32_t)std::clamp(std::ceil(model.frame->originalWidth * z), 1.f, 131072.f);
            h = (uint32_t)std::clamp(std::ceil(model.frame->originalHeight * z), 1.f, 131072.f);
        }
        if (model.fit && model.rotation % 180)
            std::swap(w, h);
        pipeline->resize(w, h);
    }
    void requestThumbs() {
        if (!model.sidebar) {
            pipeline->thumbnails(0, 0);
            model.thumbs.clear();
            return;
        }
        auto v = renderer->viewport(model);
        size_t count = (size_t)((v.bottom - v.top) / (146 * model.dpi)) + 2;
        pipeline->thumbnails(model.thumbFirst, std::min(model.entries.size(), model.thumbFirst + count));
        for (auto i = model.thumbs.begin(); i != model.thumbs.end();) {
            if (i->first < model.thumbFirst || i->first >= model.thumbFirst + count)
                i = model.thumbs.erase(i);
            else
                ++i;
        }
    }
    void navigate(int delta) {
        if (model.entries.empty())
            return;
        int64_t next =
            std::clamp((int64_t)model.selected + delta, int64_t(0), (int64_t)model.entries.size() - 1);
        select((size_t)next, delta < 0 ? -1 : 1);
    }
    void select(size_t index, int direction = 1) {
        if (index >= model.entries.size())
            return;
        model.selected = index;
        model.loading = true;
        model.status = std::to_wstring(index + 1) + L" / " + std::to_wstring(model.entries.size()) + L"   " +
                       model.entries[index].name;
        model.panX = model.panY = 0;
        model.rotation = 0;
        pipeline->select(index, direction);
        if (model.sidebar) {
            auto v = renderer->viewport(model);
            size_t count = std::max<size_t>(1, (size_t)((v.bottom - v.top) / (146 * model.dpi)));
            if (index < model.thumbFirst)
                model.thumbFirst = index;
            else if (index >= model.thumbFirst + count)
                model.thumbFirst = index - count + 1;
            requestThumbs();
        }
        invalidate();
    }
    void dialogOpen(bool folder = false) {
        ComPtr<IFileOpenDialog> dialog;
        if (FAILED(
                CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&dialog))))
            return;
        DWORD flags = 0;
        dialog->GetOptions(&flags);
        dialog->SetOptions(flags | FOS_FORCEFILESYSTEM | FOS_FILEMUSTEXIST | (folder ? FOS_PICKFOLDERS : 0));
        if (!folder) {
            COMDLG_FILTERSPEC filter[] = {
                {L"Изображения и архивы",
                 L"*.jpg;*.jpeg;*.jpe;*.png;*.webp;*.bmp;*.gif;*.tif;*.tiff;*.psd;*.zip;*.rar"},
                {L"Все файлы", L"*.*"}};
            dialog->SetFileTypes(2, filter);
        }
        if (SUCCEEDED(dialog->Show(window))) {
            ComPtr<IShellItem> item;
            PWSTR path = nullptr;
            if (SUCCEEDED(dialog->GetResult(&item)) &&
                SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path))) {
                open(path);
                CoTaskMemFree(path);
            }
        }
    }
    void fullscreen() {
        if (!full) {
            placement.length = sizeof(placement);
            GetWindowPlacement(window, &placement);
            savedStyle = (DWORD)GetWindowLongPtrW(window, GWL_STYLE);
            MONITORINFO info{sizeof(info)};
            GetMonitorInfoW(MonitorFromWindow(window, MONITOR_DEFAULTTONEAREST), &info);
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle & ~WS_OVERLAPPEDWINDOW);
            SetWindowPos(window, HWND_TOP, info.rcMonitor.left, info.rcMonitor.top,
                         info.rcMonitor.right - info.rcMonitor.left,
                         info.rcMonitor.bottom - info.rcMonitor.top, SWP_FRAMECHANGED);
            full = true;
        } else {
            SetWindowLongPtrW(window, GWL_STYLE, savedStyle);
            SetWindowPlacement(window, &placement);
            SetWindowPos(window, nullptr, 0, 0, 0, 0,
                         SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_FRAMECHANGED);
            full = false;
        }
        layoutTree();
        decodeSize();
        invalidate();
    }
    void command(UINT id) {
        switch (id) {
        case Open:
            dialogOpen();
            break;
        case OpenFolder:
            dialogOpen(true);
            break;
        case Exit:
            DestroyWindow(window);
            break;
        case Previous:
            navigate(-1);
            break;
        case Next:
            navigate(1);
            break;
        case PreviousArchive:
            archive(-1);
            break;
        case NextArchive:
            archive(1);
            break;
        case ToggleFileTree:
            settings.fileTree = !settings.fileTree;
            layoutTree();
            pipeline->configure(settings);
            decodeSize();
            invalidate();
            SetFocus(window);
            break;
        case Fit:
            model.fit = true;
            model.panX = model.panY = 0;
            decodeSize();
            invalidate();
            break;
        case Actual:
            model.fit = false;
            model.zoom = 1;
            model.panX = model.panY = 0;
            decodeSize();
            invalidate();
            break;
        case Rotate:
            model.rotation = (model.rotation + 90) % 360;
            decodeSize();
            invalidate();
            break;
        case Fullscreen:
            fullscreen();
            break;
        case Thumbnails:
            model.sidebar = !model.sidebar;
            settings.thumbnails = model.sidebar;
            pipeline->configure(settings);
            decodeSize();
            requestThumbs();
            invalidate();
            break;
        case ToggleBookmark:
            pipeline->toggleBookmark();
            break;
        case Bookmarks:
            pipeline->requestBookmarks();
            break;
        case Preferences:
            DialogBoxParamW(instance, MAKEINTRESOURCEW(IDD_SETTINGS), window, settingsProc, (LPARAM)this);
            break;
        case About:
            MessageBoxW(window,
                        L"Viewer 0.1.2\nНативный просмотр изображений, ZIP и RAR.\n\nF11 — полный экран · "
                        L"Ctrl+B — закладка\nCtrl+колесо — масштаб · R — поворот\n\n7-Zip 26.03 · libwebp "
                        L"1.6.0 · Windows WIC\nЛицензии находятся в папке licenses.",
                        L"О Viewer", MB_OK | MB_ICONINFORMATION);
            break;
        }
    }
    void shellAction(const std::wstring& action) {
        if (shellBusy)
            return;
        if (shellThread.joinable())
            shellThread.join();
        shellBusy = true;
        if (settingsWindow) {
            SetDlgItemTextW(settingsWindow, IDC_SHELL_STATUS, L"Выполняется…");
            for (int id : {IDC_ENABLE_SHELL, IDC_DISABLE_SHELL, IDC_REPAIR_SHELL})
                EnableWindow(GetDlgItem(settingsWindow, id), FALSE);
        }
        shellThread = std::thread([this, action] {
            auto result = std::make_unique<ShellResult>(shellIntegration(action));
            if (!PostMessageW(window, WM_SHELL, 0, (LPARAM)result.get()))
                return;
            result.release();
        });
    }
    static INT_PTR CALLBACK settingsProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
        auto* app = (App*)GetWindowLongPtrW(hwnd, DWLP_USER);
        if (msg == WM_INITDIALOG) {
            app = (App*)lp;
            SetWindowLongPtrW(hwnd, DWLP_USER, (LONG_PTR)app);
            app->settingsWindow = hwnd;
            SetDlgItemInt(hwnd, IDC_RAM, (UINT)(app->settings.ramBytes / MiB), FALSE);
            SetDlgItemInt(hwnd, IDC_DISK, (UINT)(app->settings.diskBytes / MiB), FALSE);
            SetDlgItemInt(hwnd, IDC_GPU, (UINT)(app->settings.gpuBytes / MiB), FALSE);
            app->shellAction(L"Status");
            return TRUE;
        }
        if (!app)
            return FALSE;
        if (msg == WM_COMMAND) {
            switch (LOWORD(wp)) {
            case IDOK: {
                BOOL a = FALSE, b = FALSE, c = FALSE;
                UINT ram = GetDlgItemInt(hwnd, IDC_RAM, &a, FALSE),
                     disk = GetDlgItemInt(hwnd, IDC_DISK, &b, FALSE);
                UINT gpu = GetDlgItemInt(hwnd, IDC_GPU, &c, FALSE);
                if (!a || !b || !c || ram < 64 || ram > 4096 || disk > 65536 || gpu < 32 || gpu > 1024) {
                    MessageBoxW(
                        hwnd, L"RAM: 64–4096 МиБ (не более 1/8 памяти). Диск: 0–65536 МиБ. GPU: 32–1024 МиБ.",
                        L"Лимиты кеша", MB_OK);
                    return TRUE;
                }
                app->settings.ramBytes = std::min((uint64_t)ram * MiB, physicalMemory() / 8);
                app->settings.diskBytes = (uint64_t)disk * MiB;
                app->settings.gpuBytes = (uint64_t)gpu * MiB;
                app->pipeline->configure(app->settings);
                EndDialog(hwnd, IDOK);
                return TRUE;
            }
            case IDCANCEL:
                EndDialog(hwnd, IDCANCEL);
                return TRUE;
            case IDC_CLEAR:
                app->pipeline->clearCache();
                SetDlgItemTextW(hwnd, IDC_SHELL_STATUS, L"Очистка кеша запущена");
                return TRUE;
            case IDC_ENABLE_SHELL:
            case IDC_REPAIR_SHELL:
                if (MessageBoxW(
                        hwnd,
                        L"Зарегистрировать пункт «Открыть в Viewer» для текущего пользователя?\n\nДля "
                        L"локальной сборки будет добавлен публичный тестовый сертификат Viewer в хранилище "
                        L"доверенных издателей пакетов текущего пользователя. Если Windows не принимает "
                        L"локальную подпись, используется уже включённый режим разработчика. Настройки "
                        L"безопасности Windows и приложения по умолчанию не изменятся.",
                        L"Интеграция с Windows", MB_OKCANCEL | MB_ICONINFORMATION) == IDOK)
                    app->shellAction(L"Register");
                return TRUE;
            case IDC_DISABLE_SHELL:
                app->shellAction(L"Unregister");
                return TRUE;
            }
        }
        if (msg == WM_DESTROY)
            app->settingsWindow = nullptr;
        return FALSE;
    }
    void event(std::unique_ptr<Event> e) {
        if (e->type == Event::Type::Bookmarks) {
            bookmarks = std::move(e->bookmarks);
            HMENU menu = CreatePopupMenu();
            if (bookmarks.empty())
                AppendMenuW(menu, MF_STRING | MF_GRAYED, 0, L"Закладок пока нет — Ctrl+B");
            for (size_t i = 0; i < std::min<size_t>(bookmarks.size(), 300); ++i) {
                auto label = fs::path(bookmarks[i].path).filename().wstring() + L" / " + bookmarks[i].name;
                std::wstring escaped;
                for (auto c : label) {
                    escaped += c;
                    if (c == L'&')
                        escaped += c;
                }
                AppendMenuW(menu, MF_STRING, (UINT_PTR)i + 1, escaped.c_str());
            }
            POINT point;
            GetCursorPos(&point);
            UINT selected =
                TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, point.x, point.y, 0, window, nullptr);
            DestroyMenu(menu);
            if (selected && selected <= bookmarks.size())
                open(bookmarks[selected - 1].path, bookmarks[selected - 1].id);
            return;
        }
        if (e->sourceToken != requestedToken && e->type != Event::Type::SettingsSaved)
            return;
        if (e->type == Event::Type::Opened) {
            sourcePath = e->path;
            model.entries = std::move(e->entries);
            model.selected = e->index;
            model.sourceName = e->path.filename().wstring();
            SetWindowTextW(window, (model.sourceName + L" — Viewer").c_str());
            requestThumbs();
        } else if (e->type == Event::Type::FrameReady && e->generation == pipeline->generation()) {
            model.frame = std::move(e->frame);
            model.selected = e->index;
            model.bookmarked = e->bookmarked;
            if (fileTree && !isArchive(sourcePath) && e->index < model.entries.size())
                fileTree->highlight(sourcePath / model.entries[e->index].name);
            model.loading = false;
            model.status = std::to_wstring(e->index + 1) + L" / " + std::to_wstring(model.entries.size()) +
                           L"   " + model.entries[e->index].name;
            if (!e->message.empty())
                model.status += L" · " + e->message;
            decodeSize();
        } else if (e->type == Event::Type::Thumbnail && e->generation == pipeline->generation()) {
            auto v = renderer->viewport(model);
            size_t count = (size_t)((v.bottom - v.top) / (146 * model.dpi)) + 2;
            if (model.sidebar && e->index >= model.thumbFirst && e->index < model.thumbFirst + count)
                model.thumbs[e->index] = std::move(e->frame);
        } else if (e->type == Event::Type::Error && e->generation == pipeline->generation()) {
            model.loading = false;
            model.status = e->message;
            if (!testOutput.empty()) {
                fs::create_directories(testOutput);
                std::ofstream out(testOutput / L"error.txt");
                out << utf8(e->message);
                SetTimer(window, 99, 100, nullptr);
            }
        } else if (e->type == Event::Type::Notice) {
            model.status = e->message;
            model.bookmarked = e->bookmarked;
        }
        invalidate();
        if (!testOutput.empty() && model.frame && !model.loading) {
            renderer->draw(model);
            capture();
        }
    }
    void capture() {
        if (testOutput.empty() || testWritten || !model.frame || model.loading)
            return;
        testWritten = true;
        DwmFlush();
        double ms =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        fs::create_directories(testOutput);
        auto m = pipeline->metrics();
        {
            std::ofstream out(testOutput / L"metrics.json");
            out << "{\"first_frame_ms\":" << ms << ",\"archive_read_bytes\":" << m->archiveReads.load()
                << ",\"extractions\":" << m->extractions.load()
                << ",\"extracted_entries\":" << m->extractedEntries.load()
                << ",\"extracted_bytes\":" << m->extractedBytes.load()
                << ",\"width\":" << model.frame->originalWidth
                << ",\"height\":" << model.frame->originalHeight << "}";
        }
        renderer->snapshot(model, testOutput / L"window.png");
        SetTimer(window, 99, 200, nullptr);
    }
};
LRESULT CALLBACK windowProc(HWND hwnd, UINT message, WPARAM wp, LPARAM lp) {
    auto* app = (App*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (message == WM_NCCREATE) {
        app = (App*)((CREATESTRUCTW*)lp)->lpCreateParams;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)app);
        app->window = hwnd;
    }
    if (!app)
        return DefWindowProcW(hwnd, message, wp, lp);
    try {
        switch (message) {
        case WM_CREATE: {
            app->model.dpi = GetDpiForWindow(hwnd) / 96.f;
            BOOL dark = TRUE;
            DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
            DragAcceptFiles(hwnd, TRUE);
            app->state = std::make_shared<StateStore>();
            app->settings = app->state->settings();
            app->model.sidebar = app->settings.thumbnails;
            app->renderer = std::make_unique<Renderer>(hwnd);
            app->pipeline = std::make_unique<ImagePipeline>(app->state, [hwnd](std::unique_ptr<Event> e) {
                if (PostMessageW(hwnd, WM_EVENT, 0, (LPARAM)e.get()))
                    e.release();
            });
            app->fileTree =
                std::make_unique<FileTree>(hwnd, [app](const fs::path& path) { app->open(path); });
            app->layoutTree();
            SetTimer(hwnd, 42, 50, nullptr);
            if (app->initialPath.empty()) {
                app->currentPath = fs::current_path();
                app->fileTree->location(app->currentPath);
            }
            app->decodeSize();
            if (!app->initialPath.empty())
                app->open(app->initialPath, app->initialEntry);
            if (!app->testOutput.empty())
                SetTimer(hwnd, 98, 30000, nullptr);
            return 0;
        }
        case WM_PAINT: {
            PAINTSTRUCT p;
            BeginPaint(hwnd, &p);
            try {
                app->renderer->draw(app->model);
            } catch (const std::exception& e) {
                app->model.status = wide(e.what());
            }
            EndPaint(hwnd, &p);
            app->capture();
            return 0;
        }
        case WM_ERASEBKGND:
            return 1;
        case WM_SIZE:
            app->layoutTree();
            if (app->renderer) {
                app->renderer->resize();
                SetTimer(hwnd, 1, 100, nullptr);
                app->invalidate();
            }
            return 0;
        case WM_DPICHANGED: {
            app->model.dpi = HIWORD(wp) / 96.f;
            auto r = (RECT*)lp;
            SetWindowPos(hwnd, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
                         SWP_NOZORDER | SWP_NOACTIVATE);
            return 0;
        }
        case WM_TIMER:
            if (wp == 42) {
                if (app->fileTree)
                    app->fileTree->poll();
            } else if (wp == 1) {
                KillTimer(hwnd, 1);
                app->decodeSize();
                app->requestThumbs();
            } else if (wp == 99 || wp == 98)
                DestroyWindow(hwnd);
            return 0;
        case WM_EVENT:
            app->event(std::unique_ptr<Event>((Event*)lp));
            return 0;
        case WM_SHELL: {
            std::unique_ptr<ShellResult> result((ShellResult*)lp);
            app->shellBusy = false;
            if (app->shellThread.joinable())
                app->shellThread.join();
            if (app->settingsWindow) {
                SetDlgItemTextW(app->settingsWindow, IDC_SHELL_STATUS, result->text.c_str());
                for (int id : {IDC_ENABLE_SHELL, IDC_DISABLE_SHELL, IDC_REPAIR_SHELL})
                    EnableWindow(GetDlgItem(app->settingsWindow, id), TRUE);
            }
            app->model.status = result->text;
            app->invalidate();
            return 0;
        }
        case WM_DROPFILES: {
            HDROP drop = (HDROP)wp;
            if (DragQueryFileW(drop, 0xffffffff, nullptr, 0) == 1) {
                UINT count = DragQueryFileW(drop, 0, nullptr, 0);
                std::wstring path(count + 1, L'\0');
                DragQueryFileW(drop, 0, path.data(), count + 1);
                path.resize(count);
                app->open(path);
            }
            DragFinish(drop);
            return 0;
        }
        case WM_COMMAND:
            app->command(LOWORD(wp));
            return 0;
        case WM_NOTIFY:
            if (app->fileTree)
                return app->fileTree->notify((NMHDR*)lp);
            return 0;
        case WM_KEYDOWN: {
            bool ctrl = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
            if (ctrl && wp == 'O')
                app->command(Open);
            else if (ctrl && wp == 'B')
                app->command(ToggleBookmark);
            else if (wp == VK_NEXT)
                app->archive(1);
            else if (wp == VK_PRIOR)
                app->archive(-1);
            else if (wp == VK_F9)
                app->command(ToggleFileTree);
            else if (wp == VK_F5 && app->fileTree)
                app->fileTree->refresh();
            else if (wp == VK_RIGHT || wp == VK_DOWN || wp == VK_SPACE)
                app->navigate(1);
            else if (wp == VK_LEFT || wp == VK_UP)
                app->navigate(-1);
            else if (wp == VK_HOME)
                app->select(0, -1);
            else if (wp == VK_END && !app->model.entries.empty())
                app->select(app->model.entries.size() - 1);
            else if (wp == VK_F11)
                app->command(Fullscreen);
            else if (wp == VK_ESCAPE) {
                if (app->full)
                    app->fullscreen();
                else {
                    app->pipeline->cancel();
                    app->model.loading = false;
                    app->model.status = L"Загрузка отменена";
                    app->invalidate();
                }
            } else if (wp == 'F')
                app->command(Fit);
            else if (wp == '1')
                app->command(Actual);
            else if (wp == 'R')
                app->command(Rotate);
            else if (wp == VK_TAB)
                app->command(Thumbnails);
            return 0;
        }
        case WM_LBUTTONDBLCLK:
            if (GET_Y_LPARAM(lp) > 44 * app->model.dpi)
                app->fullscreen();
            return 0;
        case WM_LBUTTONDOWN: {
            SetFocus(hwnd);
            float x = (float)GET_X_LPARAM(lp), y = (float)GET_Y_LPARAM(lp);
            if (y < 44 * app->model.dpi) {
                const float widths[] = {90, 75, 115, 90, 50, 105, 100, 120};
                const UINT ids[] = {Open,   ToggleFileTree, Thumbnails, Fit,
                                    Actual, Rotate,         Bookmarks,  Preferences};
                float left = 12 * app->model.dpi;
                for (int i = 0; i < 8; ++i) {
                    if (x >= left && x < left + widths[i] * app->model.dpi) {
                        app->command(ids[i]);
                        break;
                    }
                    left += widths[i] * app->model.dpi;
                }
            } else if (app->model.sidebar && x >= app->model.treeWidth &&
                       x < app->model.treeWidth + 200 * app->model.dpi) {
                size_t row = (size_t)((y - 44 * app->model.dpi) / (146 * app->model.dpi));
                app->select(app->model.thumbFirst + row);
            } else {
                SetFocus(hwnd);
                app->drag = true;
                app->dragStart = {GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
                app->oldPanX = app->model.panX;
                app->oldPanY = app->model.panY;
                SetCapture(hwnd);
            }
            return 0;
        }
        case WM_MOUSEMOVE:
            if (app->drag) {
                app->model.panX = app->oldPanX + GET_X_LPARAM(lp) - app->dragStart.x;
                app->model.panY = app->oldPanY + GET_Y_LPARAM(lp) - app->dragStart.y;
                app->invalidate();
            }
            return 0;
        case WM_LBUTTONUP:
        case WM_CAPTURECHANGED:
            app->drag = false;
            if (GetCapture() == hwnd)
                ReleaseCapture();
            return 0;
        case WM_MOUSEWHEEL: {
            int delta = GET_WHEEL_DELTA_WPARAM(wp);
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            ScreenToClient(hwnd, &p);
            if (GET_KEYSTATE_WPARAM(wp) & MK_CONTROL) {
                if (app->model.frame) {
                    float old = app->model.fit ? app->renderer->fitZoom(app->model) : app->model.zoom;
                    float z = std::clamp(old * std::pow(1.2f, delta / 120.f), 0.001f, 32.f);
                    auto v = app->renderer->viewport(app->model);
                    float x = p.x - (v.left + v.right) / 2, y = p.y - (v.top + v.bottom) / 2;
                    app->model.panX = x - (x - app->model.panX) * z / old;
                    app->model.panY = y - (y - app->model.panY) * z / old;
                    app->model.zoom = z;
                    app->model.fit = false;
                    SetTimer(hwnd, 1, 120, nullptr);
                    app->invalidate();
                }
            } else if (app->model.sidebar && p.x >= app->model.treeWidth &&
                       p.x < app->model.treeWidth + 200 * app->model.dpi) {
                int64_t next = (int64_t)app->model.thumbFirst - (delta > 0 ? 3 : -3);
                app->model.thumbFirst = (size_t)std::clamp(
                    next, int64_t(0), (int64_t)std::max<size_t>(1, app->model.entries.size()) - 1);
                app->requestThumbs();
                app->invalidate();
            } else
                app->navigate(delta > 0 ? -1 : 1);
            return 0;
        }
        case WM_CONTEXTMENU: {
            HMENU menu = CreatePopupMenu();
            for (auto [id, label] : std::vector<std::pair<UINT, const wchar_t*>>{
                     {Open, L"Открыть…\tCtrl+O"},
                     {OpenFolder, L"Открыть папку…"},
                     {Fit, L"Вписать\tF"},
                     {Actual, L"Масштаб 100%\t1"},
                     {Rotate, L"Повернуть\tR"},
                     {Fullscreen, L"Полный экран\tF11"},
                     {Thumbnails, L"Миниатюры\tTab"},
                     {ToggleFileTree, L"Дерево файлов\tF9"},
                     {PreviousArchive, L"Предыдущий архив\tPage Up"},
                     {NextArchive, L"Следующий архив\tPage Down"},
                     {ToggleBookmark, L"Добавить / удалить закладку\tCtrl+B"},
                     {Bookmarks, L"Закладки"},
                     {Preferences, L"Настройки"},
                     {About, L"О Viewer"}})
                AppendMenuW(menu, MF_STRING, id, label);
            POINT p{GET_X_LPARAM(lp), GET_Y_LPARAM(lp)};
            if (p.x == -1)
                GetCursorPos(&p);
            UINT id = TrackPopupMenu(menu, TPM_RETURNCMD | TPM_RIGHTBUTTON, p.x, p.y, 0, hwnd, nullptr);
            DestroyMenu(menu);
            if (id)
                app->command(id);
            return 0;
        }
        case WM_DESTROY:
            app->closing = true;
            KillTimer(hwnd, 42);
            app->fileTree.reset();
            if (app->pipeline && app->testOutput.empty()) {
                WINDOWPLACEMENT saved{sizeof(saved)};
                bool valid = true;
                if (app->full)
                    saved = app->placement;
                else
                    valid = GetWindowPlacement(hwnd, &saved) != FALSE;
                if (valid)
                    app->pipeline->rememberWindowPlacement(saved);
            }
            app->pipeline.reset();
            if (app->shellThread.joinable())
                app->shellThread.join();
            {
                MSG message;
                while (PeekMessageW(&message, hwnd, WM_EVENT, WM_SHELL, PM_REMOVE)) {
                    if (message.message == WM_EVENT)
                        delete (Event*)message.lParam;
                    else if (message.message == WM_SHELL)
                        delete (ShellResult*)message.lParam;
                }
            }
            PostQuitMessage(0);
            return 0;
        }
    } catch (const std::exception& e) {
        app->model.status = wide(e.what());
        app->model.loading = false;
        app->invalidate();
        if (message == WM_CREATE)
            return -1;
    }
    return DefWindowProcW(hwnd, message, wp, lp);
}
} // namespace viewer
int WINAPI wWinMain(HINSTANCE instance, HINSTANCE, LPWSTR, int show) {
    using namespace viewer;
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    INITCOMMONCONTROLSEX cc{sizeof(cc), ICC_STANDARD_CLASSES | ICC_TREEVIEW_CLASSES};
    InitCommonControlsEx(&cc);
    int count = 0;
    auto args = CommandLineToArgvW(GetCommandLineW(), &count);
    App app(instance);
    for (int i = 1; i < count; ++i) {
        std::wstring arg = args[i];
        if (arg == L"--register-shell" || arg == L"--unregister-shell") {
            auto result = shellIntegration(arg == L"--register-shell" ? L"Register" : L"Unregister");
            LocalFree(args);
            CoUninitialize();
            return result.success ? 0 : 1;
        }
        if (arg == L"--test-output" && i + 1 < count)
            app.testOutput = args[++i];
        else if (arg == L"--entry" && i + 1 < count)
            app.initialEntry = (uint32_t)_wtoi(args[++i]);
        else if (arg.rfind(L"--", 0) != 0)
            app.initialPath = fs::absolute(arg);
    }
    LocalFree(args);
    WNDCLASSEXW cls{sizeof(cls)};
    cls.style = CS_DBLCLKS;
    cls.lpfnWndProc = windowProc;
    cls.hInstance = instance;
    cls.hCursor = LoadCursorW(nullptr, IDC_ARROW);
    cls.hIcon = LoadIconW(instance, MAKEINTRESOURCEW(IDI_VIEWER));
    cls.hIconSm = cls.hIcon;
    cls.lpszClassName = L"Viewer.Window";
    RegisterClassExW(&cls);
    HWND hwnd = CreateWindowExW(WS_EX_ACCEPTFILES, cls.lpszClassName, L"Viewer",
                                WS_OVERLAPPEDWINDOW | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 1200,
                                820, nullptr, nullptr, instance, &app);
    if (!hwnd) {
        MessageBoxW(nullptr, app.model.status.c_str(), L"Не удалось запустить Viewer", MB_OK | MB_ICONERROR);
        return 1;
    }
    ShowWindow(hwnd, app.restoreWindow(show));
    UpdateWindow(hwnd);
    MSG message;
    while (GetMessageW(&message, nullptr, 0, 0) > 0) {
        TranslateMessage(&message);
        DispatchMessageW(&message);
    }
    app.renderer.reset();
    CoUninitialize();
    return 0;
}
