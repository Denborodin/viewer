#include "file_tree.h"
#include <shellapi.h>
namespace viewer {
std::vector<TreeEntry> listTreeDirectory(const fs::path& folder, const Cancel& cancel) {
    std::vector<TreeEntry> result;
    std::error_code ec;
    fs::directory_iterator it(folder, ec), end;
    if (ec)
        throw Error("Не удалось прочитать папку: " + ec.message());
    for (; it != end; it.increment(ec)) {
        if (ec)
            throw Error("Не удалось прочитать папку: " + ec.message());
        if (cancel && cancel())
            throw Cancelled();
        std::error_code status;
        bool directory = it->is_directory(status);
        if (!status && (directory || isArchive(it->path()) || isImage(it->path())))
            result.push_back({it->path(), directory});
    }
    if (ec)
        throw Error("Не удалось прочитать папку: " + ec.message());
    std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) {
        if (a.directory != b.directory)
            return a.directory;
        return naturalLess(a.path.filename().wstring(), b.path.filename().wstring());
    });
    return result;
}
std::optional<fs::path> adjacentArchive(const std::vector<TreeEntry>& entries, const fs::path& current,
                                        int delta) {
    if (!delta)
        return {};
    std::vector<fs::path> archives;
    for (auto& entry : entries)
        if (!entry.directory && isArchive(entry.path))
            archives.push_back(entry.path);
    std::sort(archives.begin(), archives.end(), [](const auto& a, const auto& b) {
        return naturalLess(a.filename().wstring(), b.filename().wstring());
    });
    if (archives.empty())
        return {};
    auto it = std::find_if(archives.begin(), archives.end(),
                           [&](auto& p) { return lower(p.wstring()) == lower(current.wstring()); });
    int64_t index =
        it == archives.end() ? (delta > 0 ? -1 : int64_t(archives.size())) : it - archives.begin();
    int64_t next = std::clamp(index + delta, int64_t(0), int64_t(archives.size() - 1));
    if (next == index)
        return {};
    return archives[(size_t)next];
}
FileTree::FileTree(HWND parent, std::function<void(const fs::path&)> open)
    : parent_(parent), open_(std::move(open)) {
    tree_ = CreateWindowExW(0, WC_TREEVIEWW, L"Файлы",
                            WS_CHILD | WS_VISIBLE | WS_TABSTOP | TVS_HASBUTTONS | TVS_HASLINES |
                                TVS_LINESATROOT | TVS_SHOWSELALWAYS | TVS_DISABLEDRAGDROP | TVS_INFOTIP,
                            0, 0, 0, 0, parent, nullptr, GetModuleHandleW(nullptr), nullptr);
    if (!tree_)
        throw Error("Не удалось создать дерево файлов");
    TreeView_SetBkColor(tree_, RGB(25, 28, 34));
    TreeView_SetTextColor(tree_, RGB(220, 224, 232));
    TreeView_SetLineColor(tree_, RGB(65, 72, 84));
    int iconSize = GetSystemMetricsForDpi(SM_CXSMICON, GetDpiForWindow(parent));
    icons_ = ImageList_Create(iconSize, iconSize, ILC_COLOR32 | ILC_MASK, 3, 0);
    for (auto id : {SIID_FOLDER, SIID_ZIPFILE, SIID_DOCNOASSOC}) {
        SHSTOCKICONINFO info{sizeof(info)};
        if (SUCCEEDED(SHGetStockIconInfo(id, SHGSI_ICON | SHGSI_SMALLICON, &info))) {
            ImageList_AddIcon(icons_, info.hIcon);
            DestroyIcon(info.hIcon);
        }
    }
    TreeView_SetImageList(tree_, icons_, TVSIL_NORMAL);
    SetWindowSubclass(tree_, subclass, 1, (DWORD_PTR)this);
    worker_ = std::thread([this] { run(); });
}
FileTree::~FileTree() {
    stopping_ = true;
    cv_.notify_all();
    if (worker_.joinable())
        worker_.join();
    if (IsWindow(tree_)) {
        RemoveWindowSubclass(tree_, subclass, 1);
        DestroyWindow(tree_);
    }
    if (font_)
        DeleteObject(font_);
    if (icons_)
        ImageList_Destroy(icons_);
}
LRESULT CALLBACK FileTree::subclass(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp, UINT_PTR, DWORD_PTR data) {
    auto self = (FileTree*)data;
    if (msg == WM_KEYDOWN && (GetKeyState(VK_CONTROL) & 0x8000) && (wp == 'O' || wp == 'B')) {
        SendMessageW(self->parent_, msg, wp, lp);
        return 0;
    }
    if (msg == WM_KEYDOWN &&
        (wp == VK_PRIOR || wp == VK_NEXT || wp == VK_F11 || wp == VK_F9 || wp == VK_ESCAPE)) {
        SendMessageW(self->parent_, msg, wp, lp);
        return 0;
    }
    if (msg == WM_KEYDOWN && wp == VK_F5) {
        self->refresh();
        return 0;
    }
    return DefSubclassProc(hwnd, msg, wp, lp);
}
void FileTree::layout(float dpi, bool visible) {
    RECT r;
    GetClientRect(parent_, &r);
    SetWindowPos(tree_, nullptr, 0, int(44 * dpi), std::min(int(240 * dpi), std::max(0, int(r.right))),
                 std::max(0, int(r.bottom - 74 * dpi)), SWP_NOZORDER | SWP_NOACTIVATE);
    ShowWindow(tree_, visible ? SW_SHOWNA : SW_HIDE);
    LOGFONTW lf{};
    lf.lfHeight = -int(14 * dpi);
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    auto font = CreateFontIndirectW(&lf);
    SendMessageW(tree_, WM_SETFONT, (WPARAM)font, TRUE);
    if (font_)
        DeleteObject(font_);
    font_ = font;
    TreeView_SetItemHeight(tree_, int(24 * dpi));
}
HTREEITEM FileTree::add(HTREEITEM parent, const fs::path& path, bool directory, const std::wstring& label,
                        bool up) {
    auto text = label.empty() ? path.filename().wstring() : label;
    TVINSERTSTRUCTW item{};
    item.hParent = parent;
    item.hInsertAfter = TVI_LAST;
    item.item.mask = TVIF_TEXT | TVIF_CHILDREN | TVIF_IMAGE | TVIF_SELECTEDIMAGE;
    item.item.iImage = item.item.iSelectedImage = directory ? 0 : isArchive(path) ? 1 : 2;
    item.item.pszText = text.data();
    item.item.cChildren = directory && !up ? 1 : 0;
    auto handle = TreeView_InsertItem(tree_, &item);
    nodes_[handle] = {path, directory, false, up};
    return handle;
}
void FileTree::enqueue(Request request) {
    {
        std::lock_guard lock(mutex_);
        requests_.push_back(std::move(request));
    }
    cv_.notify_one();
}
void FileTree::load(HTREEITEM handle) {
    auto it = nodes_.find(handle);
    if (it == nodes_.end() || !it->second.directory || it->second.loaded || it->second.up)
        return;
    it->second.loaded = true;
    enqueue({it->second.path, handle, generation_, false});
}
void FileTree::root(const fs::path& path) {
    ++generation_;
    navDelta_ = 0;
    {
        std::lock_guard lock(mutex_);
        requests_.clear();
        results_.clear();
    }
    selecting_ = true;
    TreeView_DeleteAllItems(tree_);
    nodes_.clear();
    root_ = path;
    if (path.has_parent_path() && path.parent_path() != path)
        add(TVI_ROOT, path.parent_path(), true, L"..  На уровень выше", true);
    auto item =
        add(TVI_ROOT, path, true, path.filename().empty() ? path.wstring() : path.filename().wstring());
    load(item);
    TreeView_Expand(tree_, item, TVE_EXPAND);
    selecting_ = false;
}
void FileTree::location(const fs::path& path) {
    current_ = path.lexically_normal();
    navDelta_ = 0;
    ++navigationId_;
    fs::path folder = isImage(path) || isArchive(path) ? path.parent_path() : path;
    auto relative = folder.lexically_relative(root_);
    bool known = std::any_of(nodes_.begin(), nodes_.end(), [&](const auto& item) {
        return item.second.path == current_ && !item.second.up;
    });
    if (root_.empty() || relative.empty() || *relative.begin() == L".." || (!known && folder != root_))
        root(folder);
    selectCurrent();
}
void FileTree::selectCurrent() {
    selecting_ = true;
    for (auto& [handle, node] : nodes_)
        if (!node.up && lower(node.path.wstring()) == lower(current_.wstring())) {
            TreeView_SelectItem(tree_, handle);
            TreeView_EnsureVisible(tree_, handle);
            break;
        }
    selecting_ = false;
}
void FileTree::refresh() {
    if (!root_.empty())
        root(root_);
}
void FileTree::jump(const fs::path& current, int delta) {
    if (current.empty())
        return;
    if (navDelta_ && navSource_ == current) {
        navDelta_ = std::clamp(navDelta_ + delta, -10000, 10000);
        return;
    }
    navSource_ = current;
    navDelta_ = delta;
    auto folder = isArchive(current) || isImage(current) ? current.parent_path() : current;
    enqueue({folder, nullptr, generation_, true, ++navigationId_});
}
LRESULT FileTree::notify(NMHDR* hdr) {
    if (hdr->hwndFrom != tree_)
        return 0;
    if (hdr->code == NM_CUSTOMDRAW) {
        auto draw = (NMTVCUSTOMDRAW*)hdr;
        if (draw->nmcd.dwDrawStage == CDDS_PREPAINT)
            return CDRF_NOTIFYITEMDRAW;
        if (draw->nmcd.dwDrawStage == CDDS_ITEMPREPAINT) {
            bool selected = (draw->nmcd.uItemState & CDIS_SELECTED) != 0;
            draw->clrText = RGB(220, 224, 232);
            draw->clrTextBk = selected ? RGB(41, 66, 94) : RGB(25, 28, 34);
            draw->nmcd.uItemState &= ~CDIS_SELECTED;
            return CDRF_NEWFONT;
        }
        return CDRF_DODEFAULT;
    }
    if (hdr->code == TVN_GETINFOTIPW) {
        auto tip = (NMTVGETINFOTIPW*)hdr;
        auto it = nodes_.find(tip->hItem);
        if (it != nodes_.end())
            wcsncpy_s(tip->pszText, tip->cchTextMax, it->second.path.c_str(), _TRUNCATE);
        return 0;
    }
    if (hdr->hwndFrom != tree_ || selecting_)
        return 0;
    if (hdr->code == TVN_ITEMEXPANDINGW) {
        auto n = (NMTREEVIEWW*)hdr;
        if (n->action == TVE_EXPAND)
            load(n->itemNew.hItem);
    }
    if (hdr->code == TVN_SELCHANGEDW) {
        auto n = (NMTREEVIEWW*)hdr;
        auto it = nodes_.find(n->itemNew.hItem);
        if (it == nodes_.end())
            return 0;
        auto node = it->second;
        if (node.up) {
            root(node.path);
            return 0;
        }
        if (node.directory)
            load(n->itemNew.hItem);
        open_(node.path);
    }
    return 0;
}
void FileTree::poll() {
    std::deque<Result> results;
    {
        std::lock_guard lock(mutex_);
        results.swap(results_);
    }
    for (auto& result : results) {
        if (result.request.generation != generation_)
            continue;
        if (result.request.navigation) {
            if (result.request.navigationId != navigationId_)
                continue;
            int delta = std::exchange(navDelta_, 0);
            if (!result.error.empty())
                SetWindowTextW(parent_, (L"Viewer — " + result.error).c_str());
            if (auto target = adjacentArchive(result.entries, navSource_, delta))
                open_(*target);
            continue;
        }
        auto it = nodes_.find(result.request.node);
        if (it == nodes_.end())
            continue;
        selecting_ = true;
        for (auto& entry : result.entries)
            add(result.request.node, entry.path, entry.directory);
        TVITEMW item{};
        item.hItem = result.request.node;
        item.mask = TVIF_CHILDREN;
        item.cChildren = result.entries.empty() ? 0 : 1;
        TreeView_SetItem(tree_, &item);
        if (!result.entries.empty())
            TreeView_Expand(tree_, result.request.node, TVE_EXPAND);
        if (!result.error.empty()) {
            it = nodes_.find(result.request.node);
            it->second.loaded = false;
            item.cChildren = 1;
            TreeView_SetItem(tree_, &item);
            SetWindowTextW(parent_, (L"Viewer — " + result.error).c_str());
        }
        selecting_ = false;
        selectCurrent();
    }
}
void FileTree::run() {
    while (!stopping_) {
        Request request;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] { return stopping_ || !requests_.empty(); });
            if (stopping_)
                break;
            request = std::move(requests_.front());
            requests_.pop_front();
        }
        Result result;
        result.request = request;
        try {
            result.entries = listTreeDirectory(
                request.path, [&] { return stopping_ || request.generation != generation_; });
        } catch (const Cancelled&) {
            continue;
        } catch (const std::exception& ex) {
            result.error = wide(ex.what());
        }
        {
            std::lock_guard lock(mutex_);
            results_.push_back(std::move(result));
        }
    }
}
} // namespace viewer
