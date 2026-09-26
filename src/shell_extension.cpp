#include "common.h"
#include <new>
#include <shlwapi.h>
#include <shobjidl.h>
using namespace viewer;
namespace {
const CLSID CommandId = {0x41f2e837, 0x84c6, 0x4662, {0xb8, 0x39, 0x5e, 0x15, 0x49, 0x9f, 0x43, 0xa1}};
std::atomic<ULONG> objects{0};
HMODULE module = nullptr;
fs::path modulePath() {
    std::wstring path(32768, L'\0');
    DWORD n = GetModuleFileNameW(module, path.data(), (DWORD)path.size());
    path.resize(n);
    return path;
}
bool selectedPath(IShellItemArray* selection, std::wstring& path) {
    if (!selection)
        return false;
    DWORD count = 0;
    if (FAILED(selection->GetCount(&count)) || count != 1)
        return false;
    ComPtr<IShellItem> item;
    if (FAILED(selection->GetItemAt(0, &item)))
        return false;
    PWSTR raw = nullptr;
    if (FAILED(item->GetDisplayName(SIGDN_FILESYSPATH, &raw)))
        return false;
    path = raw;
    CoTaskMemFree(raw);
    SFGAOF attributes = 0;
    if (FAILED(item->GetAttributes(SFGAO_FOLDER | SFGAO_FILESYSTEM, &attributes)) ||
        !(attributes & SFGAO_FILESYSTEM))
        return false;
    return (attributes & SFGAO_FOLDER) || isImage(path) || isArchive(path);
}
class Command final : public IExplorerCommand {
    std::atomic<ULONG> refs{1};

  public:
    Command() {
        ++objects;
    }
    ~Command() {
        --objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == __uuidof(IExplorerCommand)) {
            *result = static_cast<IExplorerCommand*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE GetTitle(IShellItemArray*, PWSTR* title) override {
        return SHStrDupW(L"Открыть в Viewer", title);
    }
    HRESULT STDMETHODCALLTYPE GetIcon(IShellItemArray*, PWSTR* icon) override {
        try {
            auto path = modulePath().wstring() + L",-101";
            return SHStrDupW(path.c_str(), icon);
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE GetToolTip(IShellItemArray*, PWSTR* text) override {
        return SHStrDupW(L"Просмотр изображений и архивов", text);
    }
    HRESULT STDMETHODCALLTYPE GetCanonicalName(GUID* id) override {
        if (!id)
            return E_POINTER;
        *id = CommandId;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetState(IShellItemArray* selection, BOOL, EXPCMDSTATE* state) override {
        if (!state)
            return E_POINTER;
        *state = ECS_HIDDEN;
        try {
            std::wstring path;
            if (selectedPath(selection, path))
                *state = ECS_ENABLED;
            return S_OK;
        } catch (...) {
            return S_OK;
        }
    }
    HRESULT STDMETHODCALLTYPE Invoke(IShellItemArray* selection, IBindCtx*) override {
        try {
            std::wstring path;
            if (!selectedPath(selection, path))
                return E_INVALIDARG;
            auto exe = modulePath().parent_path() / L"Viewer.exe";
            std::wstring line = quoteArgument(exe.wstring()) + L" " + quoteArgument(path);
            STARTUPINFOW startup{sizeof(startup)};
            PROCESS_INFORMATION process{};
            if (!CreateProcessW(exe.c_str(), line.data(), nullptr, nullptr, FALSE, 0, nullptr,
                                exe.parent_path().c_str(), &startup, &process))
                return HRESULT_FROM_WIN32(GetLastError());
            CloseHandle(process.hProcess);
            CloseHandle(process.hThread);
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE GetFlags(EXPCMDFLAGS* flags) override {
        if (!flags)
            return E_POINTER;
        *flags = ECF_DEFAULT;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE EnumSubCommands(IEnumExplorerCommand** result) override {
        if (result)
            *result = nullptr;
        return E_NOTIMPL;
    }
};
class Factory final : public IClassFactory {
    std::atomic<ULONG> refs{1};

  public:
    Factory() {
        ++objects;
    }
    ~Factory() {
        --objects;
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** result) override {
        if (!result)
            return E_POINTER;
        *result = nullptr;
        if (iid == IID_IUnknown || iid == IID_IClassFactory) {
            *result = static_cast<IClassFactory*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    ULONG STDMETHODCALLTYPE AddRef() override {
        return ++refs;
    }
    ULONG STDMETHODCALLTYPE Release() override {
        ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }
    HRESULT STDMETHODCALLTYPE CreateInstance(IUnknown* outer, REFIID iid, void** result) override {
        if (outer)
            return CLASS_E_NOAGGREGATION;
        auto* command = new (std::nothrow) Command;
        if (!command)
            return E_OUTOFMEMORY;
        HRESULT hr = command->QueryInterface(iid, result);
        command->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE LockServer(BOOL lock) override {
        if (lock)
            ++objects;
        else
            --objects;
        return S_OK;
    }
};
} // namespace
extern "C" HRESULT __stdcall DllGetClassObject(REFCLSID clsid, REFIID iid, void** result) {
    if (!result)
        return E_POINTER;
    *result = nullptr;
    if (clsid != CommandId)
        return CLASS_E_CLASSNOTAVAILABLE;
    auto* factory = new (std::nothrow) Factory;
    if (!factory)
        return E_OUTOFMEMORY;
    HRESULT hr = factory->QueryInterface(iid, result);
    factory->Release();
    return hr;
}
extern "C" HRESULT __stdcall DllCanUnloadNow() {
    return objects == 0 ? S_OK : S_FALSE;
}
BOOL WINAPI DllMain(HINSTANCE instance, DWORD reason, LPVOID) {
    if (reason == DLL_PROCESS_ATTACH) {
        module = instance;
        DisableThreadLibraryCalls(instance);
    }
    return TRUE;
}
