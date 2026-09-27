#include "pipeline.h"
#include <winsqlite/winsqlite3.h>
#include <fstream>
#include <iostream>
#include <shellapi.h>
using namespace viewer;
namespace {
int assertions = 0;
void require(bool yes, const char* message) {
    ++assertions;
    if (!yes)
        throw Error(message);
}
template <class F> void throws(F action, const char* text) {
    bool caught = false;
    try {
        action();
    } catch (const std::exception&) {
        caught = true;
    }
    require(caught, text);
}
void waitFor(const std::function<bool()>& condition, int seconds = 5) {
    auto until = std::chrono::steady_clock::now() + std::chrono::seconds(seconds);
    while (!condition()) {
        if (std::chrono::steady_clock::now() > until)
            throw Error("Timed out");
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    }
}
} // namespace
int wmain(int argc, wchar_t** argv) {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    try {
        fs::path fixtures = argc > 1 ? fs::path(argv[1]) : fs::path(L"tests/fixtures");
        fixtures /= L"generated";
        require(fs::exists(fixtures / L"stored.zip"), "Run tests/make-fixtures.ps1 first");
        auto root = fs::absolute(fixtures / L"test-state" / std::to_wstring(GetCurrentProcessId()));
        auto state = std::make_shared<StateStore>(root);
        Settings settings;
        settings.diskBytes = 8 * MiB;
        settings.ramBytes = 64 * MiB;
        auto metrics = std::make_shared<Metrics>();
        auto cache = std::make_shared<RawCache>(state, settings, metrics);
        require(naturalLess(L"page2.png", L"page10.png"), "Natural order");
        require(!naturalLess(L"page10.png", L"page2.png"), "Reverse natural order");
        require(naturalLess(L"x999999999999999999999", L"x1000000000000000000000"), "Large numeric runs");
        for (auto s : {L"C:\\space folder\\фото &%.zip", L"C:\\tail\\", L"a\\\"b", L"", L"plain"}) {
            auto line = L"viewer.exe " + quoteArgument(s);
            int n = 0;
            auto args = CommandLineToArgvW(line.c_str(), &n);
            require(n == 2 && args[1] == std::wstring(s), "Windows argument round trip");
            LocalFree(args);
        }
        state->savePosition(L"test", "version1", 7);
        require(state->position(L"test", "version1") == 7, "Resume saved");
        require(!state->position(L"test", "version2"), "Invalidate changed source");
        state->toggleBookmark({L"test", L"name", 7});
        require(state->bookmarked(L"test", 7), "Bookmark add");
        state->toggleBookmark({L"test", L"name", 7});
        require(!state->bookmarked(L"test", 7), "Bookmark remove");
        for (auto name : {L"stored.zip", L"deflate.zip", L"zip64.zip"}) {
            auto source = openSource(fixtures / name, cache, metrics, {});
            require(source->entries().size() == 14, "ZIP image catalog, duplicate names, ZIP64");
            auto first = std::find_if(source->entries().begin(), source->entries().end(),
                                      [](const Entry& e) { return e.name == L"страницы/1.png"; });
            require(first != source->entries().end(), "Unicode internal path");
            auto bytes = source->read(first->id, {});
            auto f = decodeImage(bytes, 320, 200, 32 * MiB);
            require(f->width == 320 && f->height == 200, "Viewport decoding");
            require(f->originalWidth == 640 && f->originalHeight == 400, "Original dimensions");
            std::vector<BytePtr> duplicate;
            for (auto& e : source->entries())
                if (e.name == L"повтор.png")
                    duplicate.push_back(source->read(e.id, {}));
            require(duplicate.size() == 2 && *duplicate[0] != *duplicate[1],
                    "Duplicate archive names preserve distinct IDs");
            auto before = metrics->extractions.load();
            source->read(first->id, {});
            require(metrics->extractions == before, "Encoded cache hit avoids extraction");
        }
        for (auto format : {L"photo.jpg", L"bitmap.bmp", L"first.gif", L"first.tiff"}) {
            auto f = decodeImage(readFile(fixtures / L"images" / format), 1000, 1000, 32 * MiB);
            require(f->originalWidth == 640 && f->originalHeight == 400, "WIC formats");
        }
        throws([&] { decodeImage(readFile(fixtures / L"images" / L"broken.png"), 100, 100, MiB); },
               "Broken image rejected");
        throws([&] { openSource(fixtures / L"broken.zip", cache, metrics, {}); }, "Broken archive rejected");
        {
            auto bad = std::make_shared<Bytes>(*readFile(fixtures / L"stored.zip"));
            auto u16 = [&](size_t p) { return (*bad)[p] + 256 * (*bad)[p + 1]; };
            size_t data = 30 + u16(26) + u16(28);
            (*bad)[data + 50] ^= 1;
            auto path = root / L"bad-crc.zip";
            writeFileAtomic(path, *bad);
            auto source = openSource(path, cache, metrics, {});
            throws([&] { source->read(0, {}); }, "ZIP checksum failure returned without hanging");
        }
        {
            auto folder = root / L"folder";
            fs::create_directories(folder / L"nested");
            auto path = folder / L"1.png";
            auto first = readFile(fixtures / L"images/1.png"), second = readFile(fixtures / L"images/2.png");
            writeFileAtomic(path, *first);
            writeFileAtomic(folder / L"nested/2.png", *second);
            auto source = openSource(folder, cache, metrics, {});
            require(source->entries().size() == 1, "Folders are not recursive");
            auto old = source->read(0, {});
            writeFileAtomic(path, *second);
            require(*source->read(0, {}) != *old, "Modified image invalidates encoded cache");
        }
        throws(
            [&] {
                decodeImage(readFile(fixtures / L"images" / L"1.png"), 100, 100, MiB, [] { return true; });
            },
            "Decode cancelled");
        auto large = openSource(fixtures / L"10000.zip", cache, metrics, {});
        require(large->entries().size() == 10002, "Large index");
        large.reset();
        {
            fs::path longPath = L"\\\\?\\" + root.wstring();
            for (int i = 0; i < 3; ++i)
                longPath /= std::wstring(95, L'x');
            fs::create_directories(longPath);
            longPath /= L"архив & %.zip";
            fs::copy_file(fixtures / L"stored.zip", longPath, fs::copy_options::overwrite_existing);
            auto source = openSource(longPath, cache, metrics, {});
            require(source->entries().size() == 14, "Unicode and extended-length paths");
            require(!source->read(source->entries()[0].id, {})->empty(), "Long-path extraction");
        }
        // Optional stress corpus: generated with make-large-rar.ps1 (RAR 6.x).
        for (auto count : {1000, 10000})
            for (auto version : {4, 5})
                for (bool solid : {false, true}) {
                    auto name = L"large-" + std::to_wstring(count) + L"-rar" + std::to_wstring(version) +
                                (solid ? L"-solid" : L"") + L".rar";
                    if (!fs::exists(fixtures / name))
                        continue;
                    auto m = std::make_shared<Metrics>();
                    auto local = std::make_shared<RawCache>(state, settings, m);
                    auto source = openSource(fixtures / name, local, m, {});
                    require(source->entries().size() == count, "Large RAR catalog");
                    for (size_t i = 0; i < 3; ++i)
                        source->read(source->entries()[i].id, {});
                    if (solid)
                        require(m->extractions == 1 && m->extractedEntries == 3,
                                "Large solid pauses within one session");
                    source->read(source->entries().back().id, {});
                    auto f = decodeImage(source->read(source->entries()[count / 2].id, {}), 160, 128, MiB);
                    require(f->width > 0, "Large RAR far and backward navigation");
                }
        for (auto name : {L"rar4.rar", L"rar5.rar", L"rar4-solid.rar", L"rar5-solid.rar"}) {
            require(fs::exists(fixtures / name), "RAR fixtures are required");
            auto localMetrics = std::make_shared<Metrics>();
            auto localCache = std::make_shared<RawCache>(state, settings, localMetrics);
            auto source = openSource(fixtures / name, localCache, localMetrics, {});
            std::vector<Entry> entries;
            for (auto e : source->entries())
                if (e.name != L"broken.png")
                    entries.push_back(e);
            require(entries.size() == 12, "RAR catalog");
            std::sort(entries.begin(), entries.end(),
                      [](const Entry& a, const Entry& b) { return a.id < b.id; });
            auto before = localMetrics->extractions.load();
            for (size_t i = 0; i < 3; ++i) {
                auto f = decodeImage(source->read(entries[i].id, {}), 160, 100, MiB);
                require(f->width == 160, "RAR decode");
            }
            bool solid = std::wstring(name).find(L"solid") != std::wstring::npos;
            if (solid) {
                require(localMetrics->extractions - before == 1, "Solid decoder session is preserved");
                require(localMetrics->extractedEntries == 3, "Solid stream pauses at demand frontier");
            }
            auto count = localMetrics->extractions.load();
            source->read(entries[0].id, {});
            require(localMetrics->extractions == count, "RAR back navigation from cache");
            source->read(entries.back().id, {});
            require(localMetrics->extractedEntries <= 13, "No duplicate extraction at end of session");
        }
        if (fs::exists(fixtures / L"password.rar"))
            throws([&] { openSource(fixtures / L"password.rar", cache, metrics, {}); },
                   "Password archive rejected");
        require(isArchive(L"book.7Z") && isArchive(L"book.7z.001") && isArchive(L"book.zip.002") &&
                    isArchive(L"book.r00") && isArchive(L"book.z01") && !isArchive(L"notes.001"),
                "Archive and volume extension recognition");
        require(firstArchiveVolume(L"book.part003.rar") == L"book.part001.rar" &&
                    firstArchiveVolume(L"book.7z.003") == L"book.7z.001" &&
                    firstArchiveVolume(L"book.r01") == L"book.rar" &&
                    firstArchiveVolume(L"book.z01") == L"book.zip",
                "Selecting a later volume resolves the archive entry point");
        for (auto name :
             {L"plain.7z", L"solid.7z", L"split.7z.001", L"split.7z.003", L"split.zip.001", L"split.zip.003",
              L"disk.zip", L"disk.z02", L"volumes.part1.rar", L"volumes.part3.rar", L"rar4-vol.part1.rar",
              L"rar4-vol.part3.rar", L"legacy.rar", L"legacy.r01"}) {
            auto m = std::make_shared<Metrics>();
            auto local = std::make_shared<RawCache>(state, settings, m);
            auto source = openSource(fixtures / name, local, m, {});
            require(!source->entries().empty(), "7z/multipart catalog");
            require(m->extractedEntries == 0, "Multipart catalog does not extract images");
            auto bitmap = std::find_if(source->entries().begin(), source->entries().end(),
                                       [](const Entry& e) { return e.name == L"bitmap.bmp"; });
            require(bitmap != source->entries().end(), "Multipart bitmap catalog entry");
            require(*source->read(bitmap->id, {}) == *readFile(fixtures / L"images/bitmap.bmp"),
                    "Image bytes are exact across volume boundaries");
        }
        {
            auto m = std::make_shared<Metrics>();
            auto noDisk = settings;
            noDisk.diskBytes = 0;
            auto local = std::make_shared<RawCache>(state, noDisk, m);
            auto source = openSource(fixtures / L"solid.7z", local, m, {});
            auto entries = source->entries();
            std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.id < b.id; });
            for (size_t i = 0; i < 3; ++i)
                source->read(entries[i].id, {});
            require(m->extractions == 1, "Solid 7z continues one extraction session");
            source->cancel();
            source->read(entries[3].id, {});
        }
        {
            auto m = std::make_shared<Metrics>();
            auto noDisk = settings;
            noDisk.diskBytes = 0;
            auto local = std::make_shared<RawCache>(state, noDisk, m);
            auto source = openSource(fixtures / L"solid-vol.part2.rar", local, m, {});
            auto entries = source->entries();
            std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) { return a.id < b.id; });
            for (size_t i = 0; i < 3; ++i)
                require(*source->read(entries[i].id, {}) == *readFile(fixtures / L"images" / entries[i].name),
                        "Solid multipart RAR image bytes");
            require(m->extractions == 1, "Solid multipart RAR retains extraction session");
        }
        for (auto prefix : {L"volumes.part", L"split.7z.", L"disk."}) {
            auto folder = root / prefix;
            fs::create_directories(folder);
            for (auto& file : fs::directory_iterator(fixtures)) {
                auto name = file.path().filename().wstring();
                if (file.is_regular_file() && name.starts_with(prefix) && name != L"volumes.part2.rar" &&
                    name != L"split.7z.002" && name != L"disk.z02")
                    fs::copy_file(file.path(), folder / file.path().filename());
            }
            auto name = std::wstring(prefix) == L"volumes.part" ? L"volumes.part1.rar"
                        : std::wstring(prefix) == L"disk."      ? L"disk.zip"
                                                                : L"split.7z.001";
            throws([&] { openSource(folder / name, cache, metrics, {}); },
                   "Missing middle volume is rejected");
        }
        {
            auto folder = root / L"changed-volume";
            fs::create_directories(folder);
            for (auto& file : fs::directory_iterator(fixtures))
                if (file.path().filename().wstring().starts_with(L"volumes.part"))
                    fs::copy_file(file.path(), folder / file.path().filename());
            auto source = openSource(folder / L"volumes.part1.rar", cache, metrics, {});
            auto before = source->version(source->entries()[0].id);
            source.reset();
            auto part = folder / L"volumes.part2.rar";
            fs::last_write_time(part, fs::last_write_time(part) + std::chrono::seconds(5));
            source = openSource(folder / L"volumes.part1.rar", cache, metrics, {});
            require(before != source->version(source->entries()[0].id),
                    "Companion changes invalidate archive cache");
        }
        {
            auto folder = root / L"missing-last";
            fs::create_directories(folder);
            std::vector<fs::path> parts;
            for (auto& file : fs::directory_iterator(fixtures))
                if (file.path().filename().wstring().starts_with(L"split.7z."))
                    parts.push_back(file.path());
            std::sort(parts.begin(), parts.end());
            for (size_t i = 0; i + 1 < parts.size(); ++i)
                fs::copy_file(parts[i], folder / parts[i].filename());
            throws([&] { openSource(folder / L"split.7z.001", cache, metrics, {}); },
                   "Missing final volume rejected");
            throws([&] { openSource(root / L"absent.part3.rar", cache, metrics, {}); },
                   "Missing first volume rejected");
            throws([&] { openSource(fixtures / L"split.7z.001", cache, metrics, [] { return true; }); },
                   "Multipart opening can be cancelled");
        }
        // Disk writes can fail while RAM-backed viewing must remain usable.
        {
            auto staleState = std::make_shared<StateStore>(root / L"expired");
            fs::create_directories(staleState->root() / L"cache");
            auto expired = staleState->root() / L"cache/old-1";
            writeFileAtomic(expired, Bytes{42});
            staleState->putDisk("old-1", 1);
            sqlite3* db = nullptr;
            require(sqlite3_open16((staleState->root() / L"state.sqlite").c_str(), &db) == SQLITE_OK,
                    "Expiry fixture database");
            require(sqlite3_exec(db, "UPDATE cache SET accessed=0", nullptr, nullptr, nullptr) == SQLITE_OK,
                    "Expiry fixture timestamp");
            sqlite3_close(db);
            RawCache stale(staleState, settings, metrics);
            waitFor([&] { return !fs::exists(expired); });
            require(staleState->diskRecords().empty(), "Seven-day expiry removes file and index");
        }
        {
            auto failedState = std::make_shared<StateStore>(root / L"disk-failure");
            writeFileAtomic(failedState->root() / L"cache", Bytes{0});
            RawCache failed(failedState, settings, metrics);
            auto bytes = readFile(fixtures / L"images/1.png");
            failed.put("fault", 1, bytes);
            require(failed.diskFailed(), "Unavailable disk cache detected");
            require(failed.get("fault", 1) == bytes, "RAM cache survives disk failure");
        }
        {
            auto evictionState = std::make_shared<StateStore>(root / L"eviction");
            auto tiny = settings;
            tiny.diskBytes = 1024;
            RawCache eviction(evictionState, tiny, metrics);
            auto bytes = std::make_shared<Bytes>(700, 42);
            eviction.put("lru", 1, bytes);
            eviction.put("lru", 2, bytes);
            waitFor([&] {
                auto rows = evictionState->diskRecords();
                return rows.size() == 1;
            });
            waitFor([&] { return fs::exists(evictionState->root() / L"cache/lru-2"); });
            require(!fs::exists(evictionState->root() / L"cache/lru-1"), "Disk LRU enforces capacity");
            require(!eviction.get("different-version", 2), "Changed identity invalidates cache");
        }
        {
            auto cancelState = std::make_shared<StateStore>(root / L"cancel-solid");
            auto cancelMetrics = std::make_shared<Metrics>();
            auto noDisk = settings;
            noDisk.diskBytes = 0;
            auto cancelCache = std::make_shared<RawCache>(cancelState, noDisk, cancelMetrics);
            auto source = openSource(fixtures / L"rar5-solid.rar", cancelCache, cancelMetrics, {});
            auto first = source->entries()[0].id;
            source->read(first, {});
            source->cancel();
            source->read(source->entries()[1].id, {});
            require(cancelMetrics->extractions >= 1, "Solid session restarts after explicit cancellation");
            source.reset(); // Must wake a paused extraction worker during shutdown.
        }
        std::mutex eventsMutex;
        std::condition_variable eventsCv;
        std::vector<Event> events;
        {
            ImagePipeline pipeline(state, [&](std::unique_ptr<Event> e) {
                std::lock_guard lock(eventsMutex);
                events.push_back(std::move(*e));
                eventsCv.notify_all();
            });
            pipeline.resize(320, 200);
            pipeline.open(fixtures / L"stored.zip");
            waitFor([&] {
                std::lock_guard lock(eventsMutex);
                return std::any_of(events.begin(), events.end(),
                                   [](const Event& e) { return e.type == Event::Type::FrameReady; });
            });
            for (size_t i = 0; i < 14; ++i)
                pipeline.select(i);
            uint64_t final = pipeline.select(3);
            waitFor([&] {
                std::lock_guard lock(eventsMutex);
                return std::any_of(events.begin(), events.end(), [&](const Event& e) {
                    return e.type == Event::Type::FrameReady && e.generation == final && e.index == 3;
                });
            });
            pipeline.open(fixtures / L"rar5-solid.rar");
            pipeline.open(fixtures / L"deflate.zip");
        }
        cache.reset();
        state.reset();
        std::cout << "PASS " << assertions << " assertions\n";
        CoUninitialize();
        return 0;
    } catch (const std::exception& e) {
        std::cerr << "FAIL: " << e.what() << "\n";
        CoUninitialize();
        return 1;
    }
}
