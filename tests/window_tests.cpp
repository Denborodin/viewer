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
        std::cout << "PASS " << assertions << " window placement assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        CoUninitialize();
        return 1;
    }
}
