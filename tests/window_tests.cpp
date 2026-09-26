// Exercise the real window procedure and shutdown path, using hidden windows and isolated state.
#include "../src/main.cpp"
#include <iostream>
using namespace viewer;
namespace {
int assertions = 0;
void require(bool value, const char* message) {
    ++assertions;
    if (!value)
        throw Error(message);
}
bool same(const RECT& a, const RECT& b) {
    return EqualRect(&a, &b) != FALSE;
}
void treeScreenshot(HWND hwnd, const fs::path& path) {
    RECT r;
    GetClientRect(hwnd, &r);
    auto dc = CreateCompatibleDC(nullptr);
    BITMAPINFO info{};
    info.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    info.bmiHeader.biWidth = r.right;
    info.bmiHeader.biHeight = -r.bottom;
    info.bmiHeader.biPlanes = 1;
    info.bmiHeader.biBitCount = 32;
    void* pixels = nullptr;
    auto bitmap = CreateDIBSection(dc, &info, DIB_RGB_COLORS, &pixels, nullptr, 0);
    auto previous = SelectObject(dc, bitmap);
    SendMessageW(hwnd, WM_PRINT, (WPARAM)dc, PRF_CLIENT | PRF_ERASEBKGND);
    ComPtr<IWICImagingFactory> wic;
    check(CoCreateInstance(CLSID_WICImagingFactory, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&wic)),
          "Screenshot WIC");
    ComPtr<IWICBitmap> source;
    check(wic->CreateBitmapFromHBITMAP(bitmap, nullptr, WICBitmapIgnoreAlpha, &source), "Tree bitmap");
    SelectObject(dc, previous);
    DeleteObject(bitmap);
    DeleteDC(dc);
    ComPtr<IWICStream> stream;
    wic->CreateStream(&stream);
    check(stream->InitializeFromFilename(path.c_str(), GENERIC_WRITE), "Tree PNG");
    ComPtr<IWICBitmapEncoder> encoder;
    wic->CreateEncoder(GUID_ContainerFormatPng, nullptr, &encoder);
    encoder->Initialize(stream.Get(), WICBitmapEncoderNoCache);
    ComPtr<IWICBitmapFrameEncode> frame;
    encoder->CreateNewFrame(&frame, nullptr);
    frame->Initialize(nullptr);
    check(frame->WriteSource(source.Get(), nullptr), "Tree pixels");
    check(frame->Commit(), "Tree frame");
    check(encoder->Commit(), "Tree PNG commit");
}
struct Window {
    App app{GetModuleHandleW(nullptr)};
    HWND hwnd = nullptr;
    Window() {
        hwnd = CreateWindowExW(0, L"Viewer.PlacementTest", L"Placement test", WS_OVERLAPPEDWINDOW, 100, 100,
                               700, 500, nullptr, nullptr, app.instance, &app);
        if (!hwnd)
            throw Error("Create test window");
    }
    void close() {
        if (hwnd) {
            DestroyWindow(hwnd);
            hwnd = nullptr;
        }
    }
    ~Window() {
        close();
    }
    WINDOWPLACEMENT get() {
        WINDOWPLACEMENT p{sizeof(p)};
        require(GetWindowPlacement(hwnd, &p) != FALSE, "Read placement");
        return p;
    }
};
} // namespace
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
    try {
        fs::path root =
            fs::absolute(argc > 1 ? argv[1] : L"window-tests") / std::to_wstring(GetCurrentProcessId());
        SetEnvironmentVariableW(L"VIEWER_DATA_DIR", root.c_str());
        WNDCLASSW cls{};
        cls.lpfnWndProc = windowProc;
        cls.hInstance = GetModuleHandleW(nullptr);
        cls.lpszClassName = L"Viewer.PlacementTest";
        require(RegisterClassW(&cls) != 0, "Register test class");
        WINDOWPLACEMENT expected{sizeof(expected)};
        {
            Window window;
            require(!window.app.state->windowPlacement(), "Fresh state has no saved window");
            auto p = window.get();
            p.showCmd = SW_HIDE;
            p.rcNormalPosition = {160, 140, 900, 680};
            require(SetWindowPlacement(window.hwnd, &p) != FALSE, "Move and resize");
            expected = window.get();
            window.close();
        }
        {
            Window window;
            require(window.app.restoreWindow(SW_SHOWNORMAL) == SW_SHOWNORMAL, "Normal startup state");
            require(same(window.get().rcNormalPosition, expected.rcNormalPosition),
                    "Position and size restored on next launch");
            window.app.fullscreen();
            window.close();
        }
        {
            Window window;
            window.app.restoreWindow(SW_HIDE);
            require(same(window.get().rcNormalPosition, expected.rcNormalPosition),
                    "Fullscreen close preserves normal window");
            window.close();
        }
        StateStore store(root);
        auto p = expected;
        p.showCmd = SW_SHOWMINIMIZED;
        p.flags = WPF_RESTORETOMAXIMIZED;
        store.saveWindowPlacement(p);
        require(store.windowPlacement()->showCmd == SW_SHOWMAXIMIZED,
                "Minimized maximized window restores maximized");
        {
            Window window;
            require(window.app.restoreWindow(SW_SHOWNORMAL) == SW_SHOWMAXIMIZED,
                    "Startup applies maximized state");
            require(window.app.restoreWindow(SW_HIDE) == SW_HIDE, "Hidden startup remains hidden");
            window.close();
        }
        p.flags = 0;
        store.saveWindowPlacement(p);
        require(store.windowPlacement()->showCmd == SW_SHOWNORMAL, "Minimized normal window reopens visible");
        p.showCmd = SW_SHOWNORMAL;
        p.rcNormalPosition = {100000, 100000, 100700, 100500};
        store.saveWindowPlacement(p);
        {
            Window window;
            window.app.restoreWindow(SW_HIDE);
            require(MonitorFromWindow(window.hwnd, MONITOR_DEFAULTTONULL) != nullptr,
                    "Disconnected monitor placement returns to a monitor");
            window.close();
        }
        p.rcNormalPosition = {-1200, 80, -400, 680};
        store.saveWindowPlacement(p);
        require(same(store.windowPlacement()->rcNormalPosition, p.rcNormalPosition),
                "Negative monitor coordinates preserved in database");
        {
            auto state = std::make_shared<StateStore>(root);
            ImagePipeline pipeline(state, [](std::unique_ptr<Event>) {});
            pipeline.open(root / L"missing.zip");
            pipeline.rememberWindowPlacement(expected);
            // Immediate destruction must flush state even when the command queue has not run.
        }
        require(same(store.windowPlacement()->rcNormalPosition, expected.rcNormalPosition),
                "Shutdown flushes geometry despite cancelled image work");
        auto files = root / L"Дерево & архивы";
        fs::create_directories(files / L"Вложенная папка");
        for (auto name : {L"10.zip", L"2.RAR", L"1.zip", L"photo.psd", L"ignored.txt"})
            writeFileAtomic(files / name, Bytes{1});
        writeFileAtomic(files / L"Вложенная папка" / L"3.zip", Bytes{1});
        auto entries = listTreeDirectory(files);
        require(entries.size() == 5 && entries.front().directory,
                "Tree filters files and lists folders first");
        require(adjacentArchive(entries, files / L"1.zip", 1) == files / L"2.RAR",
                "Natural next archive across formats");
        require(adjacentArchive(entries, files / L"10.zip", -1) == files / L"2.RAR",
                "Natural previous archive");
        require(!adjacentArchive(entries, files / L"10.zip", 1), "Last archive does not wrap");
        require(!adjacentArchive(entries, files / L"1.zip", -1), "First archive does not wrap");
        require(adjacentArchive(entries, files, 1) == files / L"1.zip", "Folder jumps to first archive");
        require(adjacentArchive(entries, files, -1) == files / L"10.zip",
                "Folder jumps backward to last archive");
        require(adjacentArchive(entries, files / L"1.zip", 2) == files / L"10.zip",
                "Repeated archive steps accumulate");
        bool cancelled = false;
        try {
            listTreeDirectory(files, [] { return true; });
        } catch (const Cancelled&) {
            cancelled = true;
        }
        require(cancelled, "Directory enumeration cancellation");
        {
            Window window;
            window.app.currentPath = files / L"1.zip";
            window.app.fileTree->location(window.app.currentPath);
            auto wait = [&](const std::function<bool()>& done) {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    window.app.fileTree->poll();
                    if (done())
                        return;
                    Sleep(1);
                } while (std::chrono::steady_clock::now() < deadline);
                throw Error("Tree/navigation timeout");
            };
            wait([&] { return TreeView_GetCount(window.app.fileTree->handle()) >= 7; });
            require(window.app.model.treeWidth > 0, "Tree reserves viewer space");
            auto selected = TreeView_GetSelection(window.app.fileTree->handle());
            wchar_t text[256]{};
            TVITEMW item{};
            item.hItem = selected;
            item.mask = TVIF_TEXT;
            item.pszText = text;
            item.cchTextMax = 256;
            TreeView_GetItem(window.app.fileTree->handle(), &item);
            require(std::wstring(text) == L"1.zip", "Tree selects active archive");
            SendMessageW(window.app.fileTree->handle(), WM_KEYDOWN, VK_NEXT, 0);
            wait([&] { return window.app.currentPath == files / L"2.RAR"; });
            require(true, "Page Down works with tree focus");
            SendMessageW(window.hwnd, WM_KEYDOWN, VK_PRIOR, 0);
            wait([&] { return window.app.currentPath == files / L"1.zip"; });
            require(true, "Page Up works with viewer focus");
            auto treeRoot = TreeView_GetParent(window.app.fileTree->handle(),
                                               TreeView_GetSelection(window.app.fileTree->handle()));
            auto folder = TreeView_GetChild(window.app.fileTree->handle(), treeRoot);
            TreeView_Expand(window.app.fileTree->handle(), folder, TVE_EXPAND);
            wait([&] { return TreeView_GetChild(window.app.fileTree->handle(), folder) != nullptr; });
            require(true, "Subfolders expand lazily");
            auto tree = window.app.fileTree->handle();
            auto label = [&](HTREEITEM handle) {
                wchar_t value[256]{};
                TVITEMW info{};
                info.hItem = handle;
                info.mask = TVIF_TEXT;
                info.pszText = value;
                info.cchTextMax = 256;
                TreeView_GetItem(tree, &info);
                return std::wstring(value);
            };
            std::vector<Entry> catalog{{7, L"Глава 2/Часть 2/2.jpg"},     {9, L"Глава 2/Часть 2/10.jpg"},
                                       {11, L"Глава 2/Часть 2/10.jpg"},   {15, L"Глава 2/Часть 10/1.png"},
                                       {21, L"Глава 10\\Обложки\\1.psd"}, {30, L"cover.jpg"}};
            size_t chosen = SIZE_MAX;
            auto select = [&](size_t index) { chosen = index; };
            window.app.fileTree->archiveCatalog(files / L"1.zip", catalog, select);
            window.app.fileTree->highlightArchive(1);
            auto image = TreeView_GetSelection(tree);
            auto part = TreeView_GetParent(tree, image);
            auto chapter = TreeView_GetParent(tree, part);
            auto archive = TreeView_GetParent(tree, chapter);
            require(label(image) == L"10.jpg" && label(part) == L"Часть 2" && label(chapter) == L"Глава 2" &&
                        label(archive) == L"1.zip",
                    "Archive hierarchy preserves nested Unicode paths");
            require(chosen == SIZE_MAX, "Synchronizing preview does not request another image");
            require(label(TreeView_GetChild(tree, part)) == L"2.jpg" &&
                        label(TreeView_GetNextSibling(tree, part)) == L"Часть 10",
                    "Archive folders and images use natural order");
            window.app.fileTree->highlightArchive(2);
            auto duplicate = TreeView_GetSelection(tree);
            require(duplicate != image && label(duplicate) == L"10.jpg",
                    "Duplicate archive filenames retain separate entries");
            TreeView_SelectItem(tree, image);
            require(chosen == 1, "Archive selection uses catalog index rather than archive ID");
            TreeView_SelectItem(tree, part);
            require(chosen == 0, "Folder previews its first contained image");
            window.app.fileTree->highlightArchive(4);
            require(label(TreeView_GetParent(tree, TreeView_GetSelection(tree))) == L"Обложки",
                    "Backslash archive paths form folders too");
            window.app.fileTree->refresh();
            wait([&] { return label(TreeView_GetSelection(tree)) == L"1.psd"; });
            require(true, "Refresh restores archive hierarchy and selected image");
            window.app.fileTree->location(files / L"2.RAR");
            require(TreeView_GetCount(tree) == 7, "Changing archive removes stale virtual folders");
            window.app.fileTree->location(files / L"1.zip");
            window.app.fileTree->archiveCatalog(files / L"1.zip", catalog, select);
            window.app.fileTree->highlightArchive(0);
            require(label(TreeView_GetSelection(tree)) == L"2.jpg",
                    "Previously opened archive can rebuild its folders");
            treeScreenshot(window.app.fileTree->handle(), root / L"file-tree.png");
            window.app.command(ToggleFileTree);
            require(window.app.model.treeWidth == 0, "Tree can be hidden");
            window.app.command(ToggleFileTree);
            require(window.app.model.treeWidth > 0, "Tree can be restored");
            window.app.fileTree->jump(files / L"1.zip", 1);
            window.app.fileTree->location(files / L"Вложенная папка");
            window.close(); // Pending filesystem results must not outlive the child control.
        }
        if (argc > 2) {
            Window window;
            auto waitFrame = [&] {
                auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
                do {
                    MSG message{};
                    while (PeekMessageW(&message, window.hwnd, WM_EVENT, WM_EVENT, PM_REMOVE))
                        DispatchMessageW(&message);
                    window.app.fileTree->poll();
                    if (window.app.model.frame && !window.app.model.loading)
                        return;
                    Sleep(1);
                } while (std::chrono::steady_clock::now() < deadline);
                throw Error("Archive preview timeout");
            };
            window.app.open(fs::absolute(argv[2]));
            waitFrame();
            auto token = window.app.requestedToken;
            size_t target = 0;
            for (size_t i = 1; i < window.app.model.entries.size(); ++i)
                if (window.app.model.entries[i].name.find(L'/') != std::wstring::npos ||
                    window.app.model.entries[i].name.find(L'\\') != std::wstring::npos)
                    target = i;
            require(target != 0, "ZIP fixture contains nested image paths");
            window.app.fileTree->highlightArchive(target);
            auto tree = window.app.fileTree->handle();
            auto image = TreeView_GetSelection(tree);
            TreeView_SelectItem(tree, TreeView_GetParent(tree, image));
            TreeView_SelectItem(tree, image);
            waitFrame();
            require(window.app.model.selected == target && window.app.requestedToken == token,
                    "Selecting a nested ZIP image updates preview without reopening the archive");
        }
        std::cout << "PASS " << assertions << " window and navigation assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
