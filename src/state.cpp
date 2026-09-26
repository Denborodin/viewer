#include "state.h"
#include <winsqlite/winsqlite3.h>
namespace viewer {
namespace {
struct Statement {
    sqlite3_stmt* s = nullptr;
    Statement(sqlite3* db, const char* sql) {
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
            throw Error(sqlite3_errmsg(db));
    }
    ~Statement() {
        sqlite3_finalize(s);
    }
    void text(int n, const std::string& v) {
        sqlite3_bind_text(s, n, v.c_str(), (int)v.size(), SQLITE_TRANSIENT);
    }
    void integer(int n, int64_t v) {
        sqlite3_bind_int64(s, n, v);
    }
    bool row() {
        int r = sqlite3_step(s);
        if (r != SQLITE_ROW && r != SQLITE_DONE)
            throw Error(sqlite3_errmsg(sqlite3_db_handle(s)));
        return r == SQLITE_ROW;
    }
    int64_t number(int n) {
        return sqlite3_column_int64(s, n);
    }
    std::string str(int n) {
        const auto* p = sqlite3_column_text(s, n);
        return p ? (const char*)p : "";
    }
};
} // namespace
StateStore::StateStore(fs::path root) : root_(std::move(root)) {
    fs::create_directories(root_);
    if (sqlite3_open16((root_ / L"state.sqlite").c_str(), &db_) != SQLITE_OK) {
        if (db_)
            sqlite3_close(db_);
        db_ = nullptr;
        throw Error("Не удалось открыть базу состояния");
    }
    sqlite3_busy_timeout(db_, 2000);
    exec(
        "PRAGMA journal_mode=WAL; PRAGMA synchronous=NORMAL; CREATE TABLE IF NOT EXISTS settings(k TEXT "
        "PRIMARY KEY,v INTEGER); CREATE TABLE IF NOT EXISTS positions(path TEXT PRIMARY KEY,identity TEXT,id "
        "INTEGER); CREATE TABLE IF NOT EXISTS bookmarks(path TEXT,id INTEGER,name TEXT,PRIMARY "
        "KEY(path,id)); CREATE TABLE IF NOT EXISTS cache(k TEXT PRIMARY KEY,size INTEGER,accessed INTEGER);"
        "CREATE TABLE IF NOT EXISTS window_state(id INTEGER PRIMARY KEY CHECK(id=1),"
        "l INTEGER,t INTEGER,r INTEGER,b INTEGER,maximized INTEGER);");
}
StateStore::~StateStore() {
    if (db_)
        sqlite3_close(db_);
}
void StateStore::exec(const char* sql) {
    char* e = nullptr;
    if (sqlite3_exec(db_, sql, nullptr, nullptr, &e) != SQLITE_OK) {
        std::string msg = e ? e : "SQLite error";
        sqlite3_free(e);
        throw Error(msg);
    }
}
Settings StateStore::settings() {
    std::lock_guard lock(mutex_);
    Settings r;
    Statement q(db_, "SELECT k,v FROM settings");
    while (q.row()) {
        auto k = q.str(0);
        uint64_t v = (uint64_t)q.number(1);
        if (k == "ram")
            r.ramBytes = std::clamp(v, 64 * MiB, 4096 * MiB);
        if (k == "disk")
            r.diskBytes = std::min(v, 65536 * MiB);
        if (k == "gpu")
            r.gpuBytes = std::clamp(v, 32 * MiB, 1024 * MiB);
        if (k == "thumbnails")
            r.thumbnails = v != 0;
    }
    r.ramBytes = std::min(r.ramBytes, physicalMemory() / 8);
    return r;
}
void StateStore::saveSettings(const Settings& v) {
    std::lock_guard lock(mutex_);
    for (auto [k, n] : std::vector<std::pair<const char*, uint64_t>>{{"ram", v.ramBytes},
                                                                     {"disk", v.diskBytes},
                                                                     {"gpu", v.gpuBytes},
                                                                     {"thumbnails", v.thumbnails ? 1 : 0}}) {
        Statement q(db_, "INSERT OR REPLACE INTO settings VALUES(?,?)");
        q.text(1, k);
        q.integer(2, n);
        q.row();
    }
}
std::optional<WINDOWPLACEMENT> StateStore::windowPlacement() {
    std::lock_guard lock(mutex_);
    Statement q(db_, "SELECT l,t,r,b,maximized FROM window_state WHERE id=1");
    if (!q.row())
        return {};
    for (int i = 0; i < 4; ++i)
        if (q.number(i) < -1000000 || q.number(i) > 1000000)
            return {};
    if (q.number(2) <= q.number(0) || q.number(3) <= q.number(1) || q.number(2) - q.number(0) > 100000 ||
        q.number(3) - q.number(1) > 100000)
        return {};
    WINDOWPLACEMENT p{sizeof(p)};
    p.rcNormalPosition = {(LONG)q.number(0), (LONG)q.number(1), (LONG)q.number(2), (LONG)q.number(3)};
    p.showCmd = q.number(4) ? SW_SHOWMAXIMIZED : SW_SHOWNORMAL;
    p.ptMinPosition = p.ptMaxPosition = {-1, -1};
    return p;
}
void StateStore::saveWindowPlacement(const WINDOWPLACEMENT& p) {
    const auto& r = p.rcNormalPosition;
    if (r.right <= r.left || r.bottom <= r.top)
        return;
    bool minimized =
        p.showCmd == SW_SHOWMINIMIZED || p.showCmd == SW_MINIMIZE || p.showCmd == SW_SHOWMINNOACTIVE;
    bool maximized = p.showCmd == SW_SHOWMAXIMIZED || (minimized && (p.flags & WPF_RESTORETOMAXIMIZED));
    std::lock_guard lock(mutex_);
    Statement q(db_, "INSERT OR REPLACE INTO window_state VALUES(1,?,?,?,?,?)");
    q.integer(1, r.left);
    q.integer(2, r.top);
    q.integer(3, r.right);
    q.integer(4, r.bottom);
    q.integer(5, maximized);
    q.row();
}
std::optional<uint32_t> StateStore::position(const fs::path& p, const std::string& identity) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "SELECT id FROM positions WHERE path=? AND identity=?");
    q.text(1, utf8(p.wstring()));
    q.text(2, identity);
    if (q.row())
        return (uint32_t)q.number(0);
    return {};
}
void StateStore::savePosition(const fs::path& p, const std::string& identity, uint32_t id) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "INSERT OR REPLACE INTO positions VALUES(?,?,?)");
    q.text(1, utf8(p.wstring()));
    q.text(2, identity);
    q.integer(3, id);
    q.row();
}
bool StateStore::bookmarked(const fs::path& p, uint32_t id) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "SELECT 1 FROM bookmarks WHERE path=? AND id=?");
    q.text(1, utf8(p.wstring()));
    q.integer(2, id);
    return q.row();
}
void StateStore::toggleBookmark(const Bookmark& b) {
    std::lock_guard lock(mutex_);
    Statement find(db_, "SELECT 1 FROM bookmarks WHERE path=? AND id=?");
    find.text(1, utf8(b.path));
    find.integer(2, b.id);
    bool exists = find.row();
    Statement q(db_, exists ? "DELETE FROM bookmarks WHERE path=? AND id=?"
                            : "INSERT INTO bookmarks(path,id,name) VALUES(?,?,?)");
    q.text(1, utf8(b.path));
    q.integer(2, b.id);
    if (!exists)
        q.text(3, utf8(b.name));
    q.row();
}
std::vector<Bookmark> StateStore::bookmarks() {
    std::lock_guard lock(mutex_);
    std::vector<Bookmark> r;
    Statement q(db_, "SELECT path,id,name FROM bookmarks ORDER BY path,name");
    while (q.row())
        r.push_back({wide(q.str(0)), wide(q.str(2)), (uint32_t)q.number(1)});
    return r;
}
void StateStore::putDisk(const std::string& k, uint64_t size) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "INSERT OR REPLACE INTO cache VALUES(?,?,?)");
    q.text(1, k);
    q.integer(2, size);
    q.integer(3, unixTime());
    q.row();
}
void StateStore::touchDisk(const std::string& k) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "UPDATE cache SET accessed=? WHERE k=?");
    q.integer(1, unixTime());
    q.text(2, k);
    q.row();
}
void StateStore::removeDisk(const std::string& k) {
    std::lock_guard lock(mutex_);
    Statement q(db_, "DELETE FROM cache WHERE k=?");
    q.text(1, k);
    q.row();
}
std::vector<DiskRecord> StateStore::diskRecords() {
    std::lock_guard lock(mutex_);
    Statement q(db_, "SELECT k,size,accessed FROM cache ORDER BY accessed ASC");
    std::vector<DiskRecord> r;
    while (q.row())
        r.push_back({q.str(0), (uint64_t)q.number(1), q.number(2)});
    return r;
}
} // namespace viewer
