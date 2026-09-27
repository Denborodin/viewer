#pragma once
#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <list>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <stdexcept>
#include <string>
#include <thread>
#include <unordered_map>
#include <utility>
#include <vector>
#include <windows.h>
#include <wrl/client.h>
namespace viewer {
namespace fs = std::filesystem;
template <class T> using ComPtr = Microsoft::WRL::ComPtr<T>;
using Bytes = std::vector<uint8_t>;
using BytePtr = std::shared_ptr<const Bytes>;
using Cancel = std::function<bool()>;
constexpr uint64_t MiB = 1024ull * 1024;
struct Error : std::runtime_error {
    using std::runtime_error::runtime_error;
};
struct Cancelled : Error {
    Cancelled() : Error("cancelled") {}
};
void check(HRESULT hr, const char* what);
std::string utf8(std::wstring_view text);
std::wstring wide(std::string_view text);
std::wstring winError(DWORD error);
fs::path executablePath();
fs::path dataDirectory();
std::wstring lower(std::wstring value);
bool isImage(const fs::path& path);
bool isArchive(const fs::path& path);
fs::path firstArchiveVolume(const fs::path& path);
bool naturalLess(std::wstring_view a, std::wstring_view b);
std::wstring quoteArgument(std::wstring_view argument);
std::string sha256(std::span<const uint8_t> bytes);
std::string fingerprint(const fs::path& path);
BytePtr readFile(const fs::path& path, const Cancel& cancel = {}, uint64_t limit = 512 * MiB);
void writeFileAtomic(const fs::path& path, std::span<const uint8_t> bytes);
uint64_t physicalMemory();
int64_t unixTime();
struct Entry {
    uint32_t id = 0;
    std::wstring name;
    uint64_t size = 0;
    uint32_t block = 0;
    bool solid = false;
};
struct Settings {
    uint64_t ramBytes = 512 * MiB, diskBytes = 2048 * MiB, gpuBytes = 256 * MiB;
    bool thumbnails = false;
    bool fileTree = true;
};
struct Frame {
    uint32_t width = 0, height = 0, originalWidth = 0, originalHeight = 0;
    Bytes pixels;
};
struct Metrics {
    std::atomic<uint64_t> archiveReads{0}, extractions{0}, extractedEntries{0}, extractedBytes{0},
        cacheHits{0}, solidRestarts{0};
};
} // namespace viewer
