#pragma once
#include "common.h"
#include <commctrl.h>
namespace viewer {
struct TreeEntry {
    fs::path path;
    bool directory = false;
};
std::vector<TreeEntry> listTreeDirectory(const fs::path& folder, const Cancel& cancel = {});
std::optional<fs::path> adjacentArchive(const std::vector<TreeEntry>& entries, const fs::path& current,
                                        int delta);
class FileTree {
  public:
    FileTree(HWND parent, std::function<void(const fs::path&)> open);
    void archiveCatalog(const fs::path& archive, const std::vector<Entry>& entries,
                        std::function<void(size_t)> select, std::function<void()> back = {},
                        std::wstring label = {});
    void highlightArchive(size_t index);
    ~FileTree();
    HWND handle() const {
        return tree_;
    }
    void location(const fs::path& path);
    void highlight(const fs::path& path) {
        current_ = path;
        selectCurrent();
    }
    void layout(float dpi, bool visible);
    LRESULT notify(NMHDR* notification);
    void poll();
    void jump(const fs::path& current, int delta);
    void refresh();

  private:
    struct Node {
        fs::path path;
        bool directory = false, loaded = false, up = false;
        std::optional<size_t> archiveNode;
    };
    struct Request {
        fs::path path;
        HTREEITEM node = nullptr;
        uint64_t generation = 0;
        bool navigation = false;
        uint64_t navigationId = 0;
    };
    struct Result {
        Request request;
        std::vector<TreeEntry> entries;
        std::wstring error;
    };
    HWND parent_, tree_;
    HFONT font_ = nullptr;
    HIMAGELIST icons_ = nullptr;
    std::function<void(const fs::path&)> open_;
    std::unordered_map<HTREEITEM, Node> nodes_;
    fs::path root_, current_, navSource_;
    bool selecting_ = false;
    int navDelta_ = 0;
    uint64_t navigationId_ = 0;
    struct ArchiveNode {
        std::wstring name, internalPath;
        size_t parent = 0, firstImage = 0;
        bool folder = true;
        std::vector<size_t> children;
        HTREEITEM handle = nullptr;
        bool archive = false;
    };
    fs::path archivePath_;
    std::vector<ArchiveNode> archiveNodes_;
    std::vector<size_t> archiveImages_;
    std::optional<size_t> selectedArchiveImage_;
    std::function<void(size_t)> selectArchive_;
    std::function<void()> backArchive_;
    std::atomic<uint64_t> generation_{0};
    std::atomic<bool> stopping_{false};
    std::mutex mutex_;
    std::condition_variable cv_;
    std::deque<Request> requests_;
    std::deque<Result> results_;
    std::thread worker_;
    HTREEITEM add(HTREEITEM parent, const fs::path& path, bool directory, const std::wstring& label = {},
                  bool up = false, HTREEITEM after = TVI_LAST);
    void load(HTREEITEM node);
    void enqueue(Request request);
    void root(const fs::path& path);
    void selectCurrent();
    void attachArchive();
    void populateArchive(HTREEITEM item, size_t node);
    void clearArchive();
    void activateArchiveSelection();
    void run();
    static LRESULT CALLBACK subclass(HWND, UINT, WPARAM, LPARAM, UINT_PTR, DWORD_PTR);
};
} // namespace viewer
