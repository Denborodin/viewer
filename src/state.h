#pragma once
#include "common.h"
struct sqlite3;
namespace viewer {
struct Bookmark {
    std::wstring path, name;
    uint32_t id = 0;
};
struct DiskRecord {
    std::string key;
    uint64_t size = 0;
    int64_t accessed = 0;
};
class StateStore {
  public:
    explicit StateStore(fs::path root = dataDirectory());
    ~StateStore();
    Settings settings();
    void saveSettings(const Settings& s);
    std::optional<WINDOWPLACEMENT> windowPlacement();
    void saveWindowPlacement(const WINDOWPLACEMENT& placement);
    std::optional<uint32_t> position(const fs::path& path, const std::string& identity);
    void savePosition(const fs::path& path, const std::string& identity, uint32_t id);
    bool bookmarked(const fs::path& path, uint32_t id);
    void toggleBookmark(const Bookmark& b);
    std::vector<Bookmark> bookmarks();
    void putDisk(const std::string& key, uint64_t size);
    void touchDisk(const std::string& key);
    void removeDisk(const std::string& key);
    std::vector<DiskRecord> diskRecords();
    const fs::path& root() const {
        return root_;
    }

  private:
    fs::path root_;
    sqlite3* db_ = nullptr;
    std::mutex mutex_;
    void exec(const char* sql);
};
} // namespace viewer
