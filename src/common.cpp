#include "common.h"
#include <bcrypt.h>
#include <fstream>
#include <iomanip>
#include <shlobj.h>
#include <shlwapi.h>
#include <sstream>
#include <regex>
namespace viewer {
void check(HRESULT hr, const char* what) {
    if (FAILED(hr))
        throw Error(std::string(what) + ": " + utf8(winError(hr)));
}
std::string utf8(std::wstring_view s) {
    if (s.empty())
        return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0, nullptr, nullptr);
    std::string r(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n, nullptr, nullptr);
    return r;
}
std::wstring wide(std::string_view s) {
    if (s.empty())
        return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring r(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), r.data(), n);
    return r;
}
std::wstring winError(DWORD e) {
    wchar_t* p = nullptr;
    FormatMessageW(FORMAT_MESSAGE_ALLOCATE_BUFFER | FORMAT_MESSAGE_FROM_SYSTEM |
                       FORMAT_MESSAGE_IGNORE_INSERTS,
                   nullptr, e, 0, (wchar_t*)&p, 0, nullptr);
    std::wstring r = p ? p : L"Ошибка Windows " + std::to_wstring(e);
    if (p)
        LocalFree(p);
    return r;
}
fs::path executablePath() {
    std::wstring s(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, s.data(), (DWORD)s.size());
    s.resize(n);
    return s;
}
fs::path dataDirectory() {
    // The override keeps automated tests and benchmarks isolated from user state.
    wchar_t overridePath[32768];
    DWORD n = GetEnvironmentVariableW(L"VIEWER_DATA_DIR", overridePath, 32768);
    if (n && n < 32768)
        return overridePath;
    PWSTR p = nullptr;
    check(SHGetKnownFolderPath(FOLDERID_LocalAppData, 0, nullptr, &p), "LocalAppData");
    fs::path r = fs::path(p) / L"Viewer";
    CoTaskMemFree(p);
    return r;
}
std::wstring lower(std::wstring s) {
    std::transform(s.begin(), s.end(), s.begin(), [](wchar_t c) { return (wchar_t)towlower(c); });
    return s;
}
bool isImage(const fs::path& p) {
    auto e = lower(p.extension().wstring());
    return e == L".jpg" || e == L".jpeg" || e == L".jpe" || e == L".png" || e == L".webp" || e == L".bmp" ||
           e == L".gif" || e == L".tif" || e == L".tiff" || e == L".psd";
}
bool isArchive(const fs::path& p) {
    auto e = lower(p.extension().wstring());
    auto stem = lower(p.stem().extension().wstring());
    bool numbered = (stem == L".7z" || stem == L".zip") && e.size() >= 4 &&
                    std::all_of(e.begin() + 1, e.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; });
    return e == L".zip" || e == L".rar" || e == L".7z" || numbered || firstArchiveVolume(p) != p;
}
fs::path firstArchiveVolume(const fs::path& p) {
    auto name = p.filename().wstring();
    std::wsmatch m;
    static const std::wregex numbered(LR"((.*\.(?:7z|zip))\.([0-9]{3,}))", std::regex::icase);
    static const std::wregex rar(LR"((.*\.part)([0-9]+)(\.rar))", std::regex::icase);
    static const std::wregex legacy(LR"((.*)\.([rz])([0-9]{2,}))", std::regex::icase);
    if (std::regex_match(name, m, numbered))
        return p.parent_path() / (m[1].str() + L"." + std::wstring(m[2].length() - 1, L'0') + L"1");
    if (std::regex_match(name, m, rar))
        return p.parent_path() / (m[1].str() + std::wstring(m[2].length() - 1, L'0') + L"1" + m[3].str());
    if (std::regex_match(name, m, legacy))
        return p.parent_path() / (m[1].str() + (lower(m[2].str()) == L"r" ? L".rar" : L".zip"));
    return p;
}
bool naturalLess(std::wstring_view a, std::wstring_view b) {
    // Locale-independent numeric comparison, including arbitrarily long digit runs.
    size_t i = 0, j = 0;
    while (i < a.size() && j < b.size()) {
        if (a[i] >= L'0' && a[i] <= L'9' && b[j] >= L'0' && b[j] <= L'9') {
            size_t ai = i, bj = j;
            while (i < a.size() && a[i] >= L'0' && a[i] <= L'9')
                ++i;
            while (j < b.size() && b[j] >= L'0' && b[j] <= L'9')
                ++j;
            size_t x = ai, y = bj;
            while (x < i && a[x] == L'0')
                ++x;
            while (y < j && b[y] == L'0')
                ++y;
            if (i - x != j - y)
                return i - x < j - y;
            int c = a.substr(x, i - x).compare(b.substr(y, j - y));
            if (c)
                return c < 0;
        } else {
            auto ac = towlower(a[i]), bc = towlower(b[j]);
            if (ac != bc)
                return ac < bc;
            ++i;
            ++j;
        }
    }
    if (i != a.size() || j != b.size())
        return i == a.size();
    return a < b;
}
std::wstring quoteArgument(std::wstring_view s) {
    std::wstring out = L"\"";
    size_t slashes = 0;
    for (wchar_t c : s) {
        if (c == L'\\') {
            ++slashes;
            continue;
        }
        if (c == L'\"') {
            out.append(slashes * 2 + 1, L'\\');
            out += c;
        } else {
            out.append(slashes, L'\\');
            out += c;
        }
        slashes = 0;
    }
    out.append(slashes * 2, L'\\');
    out += L'\"';
    return out;
}
std::string sha256(std::span<const uint8_t> bytes) {
    uint8_t digest[32];
    if (BCryptHash(BCRYPT_SHA256_ALG_HANDLE, nullptr, 0, const_cast<PUCHAR>(bytes.data()),
                   (ULONG)bytes.size(), digest, 32) < 0)
        throw Error("SHA-256 failed");
    static constexpr char hex[] = "0123456789abcdef";
    std::string r;
    for (auto c : digest) {
        r += hex[c >> 4];
        r += hex[c & 15];
    }
    return r;
}
std::string fingerprint(const fs::path& path) {
    HANDLE h = CreateFileW(path.c_str(), FILE_READ_ATTRIBUTES,
                           FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_FLAG_BACKUP_SEMANTICS, nullptr);
    if (h == INVALID_HANDLE_VALUE)
        throw Error(utf8(winError(GetLastError())));
    BY_HANDLE_FILE_INFORMATION info{};
    BOOL ok = GetFileInformationByHandle(h, &info);
    CloseHandle(h);
    if (!ok)
        throw Error("Cannot identify source");
    auto p = utf8(fs::absolute(path).lexically_normal().wstring());
    p += '|';
    p += std::to_string(info.dwVolumeSerialNumber) + ":" + std::to_string(info.nFileIndexHigh) + ":" +
         std::to_string(info.nFileIndexLow) + ":" + std::to_string(info.nFileSizeHigh) + ":" +
         std::to_string(info.nFileSizeLow) + ":" + std::to_string(info.ftLastWriteTime.dwHighDateTime) + ":" +
         std::to_string(info.ftLastWriteTime.dwLowDateTime);
    return sha256({reinterpret_cast<const uint8_t*>(p.data()), p.size()});
}
BytePtr readFile(const fs::path& path, const Cancel& cancel, uint64_t limit) {
    std::ifstream in(path, std::ios::binary | std::ios::ate);
    if (!in)
        throw Error("Не удалось открыть файл");
    auto size = in.tellg();
    if (size < 0 || (uint64_t)size > limit)
        throw Error("Файл превышает допустимый размер буфера");
    auto data = std::make_shared<Bytes>((size_t)size);
    in.seekg(0);
    for (size_t pos = 0; pos < data->size();) {
        if (cancel && cancel())
            throw Cancelled();
        size_t n = std::min<size_t>(MiB, data->size() - pos);
        if (!in.read((char*)data->data() + pos, n))
            throw Error("Ошибка чтения файла");
        pos += n;
    }
    return data;
}
void writeFileAtomic(const fs::path& path, std::span<const uint8_t> bytes) {
    auto temp = path;
    temp += L".tmp";
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out)
            throw Error("Не удалось создать кеш");
        out.write((const char*)bytes.data(), bytes.size());
        out.close();
        if (!out) {
            std::error_code ec;
            fs::remove(temp, ec);
            throw Error("Недостаточно места для кеша");
        }
    }
    if (!MoveFileExW(temp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        std::error_code ec;
        fs::remove(temp, ec);
        throw Error("Не удалось сохранить кеш");
    }
}
uint64_t physicalMemory() {
    MEMORYSTATUSEX s{sizeof(s)};
    GlobalMemoryStatusEx(&s);
    return s.ullTotalPhys;
}
int64_t unixTime() {
    return std::chrono::duration_cast<std::chrono::seconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}
} // namespace viewer
