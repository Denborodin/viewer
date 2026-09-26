#include "shell_integration.h"
namespace viewer {
ShellResult shellIntegration(std::wstring_view action) {
    auto root = executablePath().parent_path();
    auto script = root / L"integration" / L"Register.ps1";
    if (!fs::exists(script))
        return {false, L"Пакет интеграции отсутствует. Используйте папку dist/Viewer."};
    wchar_t system[MAX_PATH];
    GetSystemDirectoryW(system, MAX_PATH);
    auto powershell = fs::path(system) / L"WindowsPowerShell" / L"v1.0" / L"powershell.exe";
    std::wstring line = quoteArgument(powershell.wstring()) +
                        L" -NoLogo -NoProfile -NonInteractive -ExecutionPolicy Bypass -File " +
                        quoteArgument(script.wstring()) + L" -Action " + quoteArgument(action) +
                        L" -AppDirectory " + quoteArgument(root.wstring());
    SECURITY_ATTRIBUTES sa{sizeof(sa), nullptr, TRUE};
    HANDLE read = nullptr, write = nullptr;
    if (!CreatePipe(&read, &write, &sa, 0))
        return {false, winError(GetLastError())};
    SetHandleInformation(read, HANDLE_FLAG_INHERIT, 0);
    STARTUPINFOW startup{sizeof(startup)};
    startup.dwFlags = STARTF_USESTDHANDLES | STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    startup.hStdOutput = write;
    startup.hStdError = write;
    startup.hStdInput = GetStdHandle(STD_INPUT_HANDLE);
    PROCESS_INFORMATION process{};
    BOOL ok = CreateProcessW(powershell.c_str(), line.data(), nullptr, nullptr, TRUE, CREATE_NO_WINDOW,
                             nullptr, root.c_str(), &startup, &process);
    CloseHandle(write);
    if (!ok) {
        CloseHandle(read);
        return {false, winError(GetLastError())};
    }
    std::string output;
    char buffer[4096];
    DWORD count = 0;
    while (ReadFile(read, buffer, sizeof(buffer), &count, nullptr) && count) {
        if (output.size() < 65536)
            output.append(buffer, count);
    }
    CloseHandle(read);
    WaitForSingleObject(process.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(process.hProcess, &code);
    CloseHandle(process.hProcess);
    CloseHandle(process.hThread);
    return {code == 0, wide(output)};
}
} // namespace viewer
