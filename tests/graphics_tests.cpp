#include "decode.h"
#include "renderer.h"
#include <iostream>
#include <shellapi.h>
#include <shobjidl.h>
#include <shlobj.h>
#include <webp/encode.h>

using namespace viewer;
int assertions = 0;
void require(bool condition, const char* message) {
    ++assertions;
    if (!condition)
        throw Error(message);
}
ComPtr<IShellItemArray> selection(const std::vector<fs::path>& paths) {
    std::vector<PIDLIST_ABSOLUTE> ids;
    for (const auto& path : paths) {
        PIDLIST_ABSOLUTE id = nullptr;
        check(SHParseDisplayName(path.c_str(), nullptr, &id, 0, nullptr), "Shell path");
        ids.push_back(id);
    }
    ComPtr<IShellItemArray> result;
    auto hr = SHCreateShellItemArrayFromIDLists((UINT)ids.size(), (PCIDLIST_ABSOLUTE*)ids.data(), &result);
    for (auto id : ids)
        CoTaskMemFree(id);
    check(hr, "Selection");
    return result;
}
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    try {
        auto fixtures = fs::absolute(fs::path(argv[1]) / L"generated");
        auto output = fixtures / L"graphics";
        fs::create_directories(output);
        // Lossless WebP verifies alpha premultiplication and the bundled codec.
        Bytes pixels(8 * 4 * 4);
        for (size_t i = 0; i < pixels.size(); i += 4) {
            pixels[i + 2] = 200;
            pixels[i + 3] = 128;
        }
        uint8_t* encoded = nullptr;
        auto size = WebPEncodeLosslessBGRA(pixels.data(), 8, 4, 32, &encoded);
        require(size > 0, "WebP test encoding");
        auto webp = std::make_shared<Bytes>(encoded, encoded + size);
        WebPFree(encoded);
        auto frame = decodeImage(webp, 8, 4, MiB);
        require(frame->width == 8 && frame->height == 4, "WebP dimensions");
        require(frame->pixels[2] == 100 && frame->pixels[3] == 128, "Premultiplied WebP transparency");
        writeFileAtomic(output / L"alpha.webp", *webp);
        // WebP extended container: EXIF + an embedded standard sRGB ICC profile.
        auto profile =
            readFile(fs::path(L"C:/Windows/System32/spool/drivers/color/sRGB Color Space Profile.icm"));
        auto extended = std::make_shared<Bytes>(Bytes{'R', 'I', 'F', 'F', 0, 0, 0, 0, 'W', 'E', 'B', 'P'});
        auto chunk = [&](const char* name, std::span<const uint8_t> data) {
            extended->insert(extended->end(), name, name + 4);
            uint32_t length = (uint32_t)data.size();
            for (int i = 0; i < 4; ++i)
                extended->push_back((uint8_t)(length >> (8 * i)));
            extended->insert(extended->end(), data.begin(), data.end());
            if (length % 2)
                extended->push_back(0);
        };
        chunk("VP8X", Bytes{0x38, 0, 0, 0, 7, 0, 0, 3, 0, 0});
        chunk("ICCP", *profile);
        extended->insert(extended->end(), webp->begin() + 12, webp->end());
        chunk("EXIF",
              Bytes{'I', 'I', 42, 0, 8, 0, 0, 0, 1, 0, 0x12, 1, 3, 0, 1, 0, 0, 0, 6, 0, 0, 0, 0, 0, 0, 0});
        uint32_t riffSize = (uint32_t)extended->size() - 8;
        for (int i = 0; i < 4; ++i)
            (*extended)[4 + i] = (uint8_t)(riffSize >> (8 * i));
        auto colorFrame = decodeImage(extended, 8, 8, MiB);
        require(colorFrame->width == 4 && colorFrame->height == 8, "WebP EXIF orientation");
        require(std::abs((int)colorFrame->pixels[2] - 100) <= 2 && colorFrame->pixels[3] == 128,
                "Embedded sRGB ICC and alpha");
        writeFileAtomic(output / L"icc-exif.webp", *extended);

        // Insert a valid EXIF orientation into a JPEG without recompressing it.
        auto jpeg = readFile(fixtures / L"images/photo.jpg");
        const Bytes exif = {0xff, 0xe1, 0,    34, 'E', 'x', 'i', 'f', 0, 0, 'I', 'I', 42, 0, 8, 0, 0, 0,
                            1,    0,    0x12, 1,  3,   0,   1,   0,   0, 0, 6,   0,   0,  0, 0, 0, 0, 0};
        auto oriented = std::make_shared<Bytes>(jpeg->begin(), jpeg->begin() + 2);
        oriented->insert(oriented->end(), exif.begin(), exif.end());
        oriented->insert(oriented->end(), jpeg->begin() + 2, jpeg->end());
        frame = decodeImage(oriented, 1000, 1000, 32 * MiB);
        require(frame->originalWidth == 400 && frame->originalHeight == 640, "JPEG EXIF rotation");
        auto limited = decodeImage(jpeg, 10000, 10000, 4096);
        require(limited->pixels.size() <= 4096, "Decoded pixel memory limit");
        writeFileAtomic(output / L"orientation.jpg", *oriented);

        // Wide frame spans five GPU tiles. Sample the software rendering result.
        HWND window = CreateWindowExW(0, L"STATIC", L"Viewer renderer test", WS_OVERLAPPEDWINDOW, 0, 0, 1000,
                                      700, nullptr, nullptr, GetModuleHandleW(nullptr), nullptr);
        require(window != nullptr, "Test window");
        {
            Renderer renderer(window);
            ViewModel model;
            model.frame = std::make_shared<Frame>();
            auto& wide = *model.frame;
            wide.width = wide.originalWidth = 20000;
            wide.height = wide.originalHeight = 256;
            wide.pixels.resize((size_t)wide.width * wide.height * 4);
            for (size_t i = 0; i < wide.pixels.size(); i += 4) {
                wide.pixels[i + 1] = 180;
                wide.pixels[i + 3] = 255;
            }
            renderer.draw(model);
            renderer.snapshot(model, output / L"wide.png");
            auto rendered = decodeImage(readFile(output / L"wide.png"), 2000, 2000, 32 * MiB);
            auto v = renderer.viewport(model);
            size_t center = ((size_t)((v.top + v.bottom) / 2) * rendered->width + rendered->width / 2) * 4;
            require(rendered->pixels[center + 1] == 180, "Tiled renderer center pixel");
            model.rotation = 90;
            renderer.snapshot(model, output / L"rotated.png");
            model.sidebar = true;
            model.entries.push_back({0, L"1.png"});
            model.thumbs[0] = decodeImage(readFile(fixtures / L"images/1.png"), 160, 128, MiB);
            renderer.snapshot(model, output / L"sidebar.png");
        }
        DestroyWindow(window);

        writeFileAtomic(output / L"supported.PSD", Bytes{1});
        // Test the extension independently of Explorer. No image decoder is loaded by it.
        auto dll = LoadLibraryW((executablePath().parent_path() / L"ViewerShell.dll").c_str());
        require(dll != nullptr, "Shell DLL loaded");
        using FactoryFn = HRESULT(__stdcall*)(REFCLSID, REFIID, void**);
        auto getFactory = (FactoryFn)GetProcAddress(dll, "DllGetClassObject");
        CLSID id;
        check(CLSIDFromString(L"{41F2E837-84C6-4662-B839-5E15499F43A1}", &id), "CLSID");
        {
            ComPtr<IClassFactory> factory;
            check(getFactory(id, IID_PPV_ARGS(&factory)), "Shell factory");
            ComPtr<IExplorerCommand> command;
            check(factory->CreateInstance(nullptr, IID_PPV_ARGS(&command)), "Shell command");
            for (const auto& path : {fixtures, fixtures / L"stored.zip", fixtures / L"rar5.rar",
                                     output / L"alpha.webp", output / L"supported.PSD"}) {
                auto items = selection({path});
                EXPCMDSTATE state = ECS_HIDDEN;
                check(command->GetState(items.Get(), FALSE, &state), "GetState");
                require(state == ECS_ENABLED, "Supported selection enabled");
            }
            auto multiple = selection({fixtures / L"stored.zip", fixtures / L"rar5.rar"});
            EXPCMDSTATE state = ECS_ENABLED;
            command->GetState(multiple.Get(), FALSE, &state);
            require(state == ECS_HIDDEN, "Multiple selection hidden");
            writeFileAtomic(output / L"unsupported.txt", Bytes{1});
            auto unsupported = selection({output / L"unsupported.txt"});
            command->GetState(unsupported.Get(), FALSE, &state);
            require(state == ECS_HIDDEN, "Unsupported selection hidden");
            command->GetState(nullptr, FALSE, &state);
            require(state == ECS_HIDDEN, "Empty selection hidden");
        }
        using UnloadFn = HRESULT(__stdcall*)();
        require(((UnloadFn)GetProcAddress(dll, "DllCanUnloadNow"))() == S_OK, "Shell objects released");
        FreeLibrary(dll);
        if (argc > 2 && std::wstring(argv[2]) == L"--registered") {
            ComPtr<IExplorerCommand> registered;
            check(CoCreateInstance(id, nullptr, CLSCTX_LOCAL_SERVER, IID_PPV_ARGS(&registered)),
                  "Registered COM activation");
            PWSTR title = nullptr;
            check(registered->GetTitle(nullptr, &title), "Registered title");
            require(std::wstring(title) == L"Открыть в Viewer", "Registered surrogate command");
            CoTaskMemFree(title);
            auto psd = selection({output / L"supported.PSD"});
            EXPCMDSTATE state=ECS_HIDDEN;
            check(registered->GetState(psd.Get(),FALSE,&state),"Registered PSD state");
            require(state==ECS_ENABLED,"Registered PSD command enabled");
        }
        std::cout << "PASS " << assertions << " graphics and shell assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        CoUninitialize();
        return 1;
    }
}
