#include "source.h"
#include "Common/MyInitGuid.h"
#include "7zip/Archive/IArchive.h"
#include <array>
#include <propvarutil.h>
namespace viewer {
namespace {
template <class T> class SevenObject : public T {
    std::atomic<ULONG> refs_{1};

  protected:
    virtual ~SevenObject() = default;

  public:
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return ++refs_;
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        ULONG r = --refs_;
        if (!r)
            delete this;
        return r;
    }
};
class FileStream final : public SevenObject<IInStream> {
    HANDLE file_;
    std::shared_ptr<Metrics> metrics_;
    Cancel cancel_;

  public:
    FileStream(const fs::path& p, std::shared_ptr<Metrics> m, Cancel cancel, bool temporary = false)
        : metrics_(std::move(m)), cancel_(std::move(cancel)) {
        file_ = CreateFileW(p.c_str(), GENERIC_READ,
                            FILE_SHARE_READ | FILE_SHARE_DELETE | (temporary ? FILE_SHARE_WRITE : 0), nullptr,
                            OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file_ == INVALID_HANDLE_VALUE)
            throw Error(utf8(winError(GetLastError())));
    }
    ~FileStream() {
        CloseHandle(file_);
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IInStream || iid == IID_ISequentialInStream) {
            *out = static_cast<IInStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) noexcept override {
        if (processed)
            *processed = 0;
        if (cancel_ && cancel_())
            return E_ABORT;
        DWORD n = 0;
        if (!ReadFile(file_, data, size, &n, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        if (processed)
            *processed = n;
        metrics_->archiveReads += n;
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* pos) noexcept override {
        LARGE_INTEGER n{}, out{};
        n.QuadPart = offset;
        if (!SetFilePointerEx(file_, n, &out, origin))
            return HRESULT_FROM_WIN32(GetLastError());
        if (pos)
            *pos = out.QuadPart;
        return S_OK;
    }
};
// Numbered 7z/ZIP volumes are byte slices of one seekable archive, not nested archives.
class SplitStream final : public SevenObject<IInStream> {
    std::vector<fs::path> paths_;
    std::vector<uint64_t> offsets_{0};
    uint64_t position_ = 0;
    size_t active_ = SIZE_MAX;
    ComPtr<FileStream> stream_;
    std::shared_ptr<Metrics> metrics_;
    Cancel cancel_;

  public:
    SplitStream(const std::vector<fs::path>& paths, std::shared_ptr<Metrics> metrics, Cancel cancel)
        : paths_(paths), metrics_(std::move(metrics)), cancel_(std::move(cancel)) {
        for (const auto& path : paths_) {
            auto size = fs::file_size(path);
            if (!size || size > INT64_MAX - offsets_.back())
                throw Error("Некорректный размер тома: " + utf8(path.filename().wstring()));
            offsets_.push_back(offsets_.back() + size);
        }
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_IInStream && iid != IID_ISequentialInStream)
            return E_NOINTERFACE;
        *out = static_cast<IInStream*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Read(void* data, UInt32 size, UInt32* processed) noexcept override {
        if (processed)
            *processed = 0;
        try {
            UInt32 total = 0;
            while (size && position_ < offsets_.back()) {
                if (cancel_ && cancel_())
                    return E_ABORT;
                size_t index =
                    std::upper_bound(offsets_.begin(), offsets_.end(), position_) - offsets_.begin() - 1;
                if (active_ != index) {
                    stream_.Attach(new FileStream(paths_[index], metrics_, cancel_));
                    active_ = index;
                }
                HRESULT hr = stream_->Seek(position_ - offsets_[index], STREAM_SEEK_SET, nullptr);
                if (FAILED(hr))
                    return hr;
                UInt32 n = 0;
                hr = stream_->Read(static_cast<uint8_t*>(data) + total,
                                   (UInt32)std::min<uint64_t>(size, offsets_[index + 1] - position_), &n);
                if (FAILED(hr))
                    return hr;
                if (!n)
                    return HRESULT_FROM_WIN32(ERROR_HANDLE_EOF);
                position_ += n;
                total += n;
                size -= n;
                if (processed)
                    *processed = total;
            }
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE Seek(Int64 offset, UInt32 origin, UInt64* pos) noexcept override {
        uint64_t base = origin == STREAM_SEEK_SET   ? 0
                        : origin == STREAM_SEEK_CUR ? position_
                                                    : offsets_.back();
        if (origin > STREAM_SEEK_END || (offset < 0 && uint64_t(-(offset + 1)) + 1 > base) ||
            (offset >= 0 && uint64_t(offset) > INT64_MAX - base))
            return STG_E_INVALIDFUNCTION;
        position_ = offset < 0 ? base - (uint64_t(-(offset + 1)) + 1) : base + offset;
        if (pos)
            *pos = position_;
        return S_OK;
    }
};
class VolumeCallback final : public SevenObject<IArchiveOpenCallback>, public IArchiveOpenVolumeCallback {
    fs::path first_;
    std::shared_ptr<Metrics> metrics_;
    Cancel cancel_;

  public:
    std::vector<fs::path> paths;
    std::wstring missing;
    VolumeCallback(fs::path first, std::shared_ptr<Metrics> metrics, Cancel cancel)
        : first_(std::move(first)), metrics_(std::move(metrics)), cancel_(std::move(cancel)), paths{first_} {}
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return SevenObject::AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return SevenObject::Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveOpenCallback)
            *out = static_cast<IArchiveOpenCallback*>(this);
        else if (iid == IID_IArchiveOpenVolumeCallback)
            *out = static_cast<IArchiveOpenVolumeCallback*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(const UInt64*, const UInt64*) noexcept override {
        return cancel_ && cancel_() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*, const UInt64*) noexcept override {
        return cancel_ && cancel_() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetProperty(PROPID id, PROPVARIANT* value) noexcept override {
        PropVariantInit(value);
        try {
            if (id == kpidName) {
                value->vt = VT_BSTR;
                value->bstrVal = SysAllocString(first_.filename().c_str());
                return value->bstrVal ? S_OK : E_OUTOFMEMORY;
            }
            if (id == kpidSize) {
                value->vt = VT_UI8;
                value->uhVal.QuadPart = fs::file_size(first_);
            }
            if (id == kpidIsDir) {
                value->vt = VT_BOOL;
                value->boolVal = VARIANT_FALSE;
            }
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
    HRESULT STDMETHODCALLTYPE GetStream(const wchar_t* name, IInStream** out) noexcept override {
        *out = nullptr;
        try {
            if (cancel_ && cancel_())
                return E_ABORT;
            fs::path leaf(name);
            // Volume names are untrusted archive metadata; only sibling files are permitted.
            if (leaf.empty() || leaf != leaf.filename() || leaf == L".." ||
                leaf.wstring().find(L':') != std::wstring::npos)
                return E_ACCESSDENIED;
            auto path = first_.parent_path() / leaf;
            if (!fs::is_regular_file(path)) {
                missing = leaf.wstring();
                return S_FALSE;
            }
            if (std::find(paths.begin(), paths.end(), path) == paths.end())
                paths.push_back(path);
            *out = new FileStream(path, metrics_, cancel_);
            return S_OK;
        } catch (...) {
            return E_FAIL;
        }
    }
};
struct Property {
    PROPVARIANT p{};
    ~Property() {
        PropVariantClear(&p);
    }
    bool boolean() const {
        return p.vt == VT_BOOL && p.boolVal != VARIANT_FALSE;
    }
    uint64_t number() const {
        if (p.vt == VT_UI8)
            return p.uhVal.QuadPart;
        if (p.vt == VT_UI4)
            return p.ulVal;
        return 0;
    }
    std::wstring string() const {
        return p.vt == VT_BSTR && p.bstrVal ? p.bstrVal : L"";
    }
};
struct NestedFile {
    fs::path path;
    HANDLE handle = INVALID_HANDLE_VALUE;
    NestedFile() {
        auto dir = dataDirectory() / L"nested";
        fs::create_directories(dir);
        GUID guid{};
        check(CoCreateGuid(&guid), "Temporary archive name");
        wchar_t name[40]{};
        StringFromGUID2(guid, name, 40);
        path = dir / (std::wstring(name) + L".tmp");
        handle =
            CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_DELETE,
                        nullptr, CREATE_NEW, FILE_ATTRIBUTE_TEMPORARY | FILE_FLAG_DELETE_ON_CLOSE, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
            throw Error("Не удалось создать временный файл вложенного архива");
    }
    ~NestedFile() {
        if (handle != INVALID_HANDLE_VALUE)
            CloseHandle(handle);
        DeleteFileW(path.c_str());
    }
};
class NestedOutput final : public SevenObject<ISequentialOutStream> {
    std::shared_ptr<NestedFile> file_;
    Cancel cancel_;

  public:
    uint64_t size = 0;
    static constexpr uint64_t limit = 2048 * MiB;
    NestedOutput(std::shared_ptr<NestedFile> file, Cancel cancel)
        : file_(std::move(file)), cancel_(std::move(cancel)) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid != IID_IUnknown && iid != IID_ISequentialOutStream)
            return E_NOINTERFACE;
        *out = static_cast<ISequentialOutStream*>(this);
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 bytes, UInt32* processed) noexcept override {
        if (processed)
            *processed = 0;
        if (cancel_ && cancel_())
            return E_ABORT;
        if (bytes > limit - size)
            return HRESULT_FROM_WIN32(ERROR_FILE_TOO_LARGE);
        DWORD n = 0;
        if (!WriteFile(file_->handle, data, bytes, &n, nullptr))
            return HRESULT_FROM_WIN32(GetLastError());
        size += n;
        if (processed)
            *processed = n;
        return n == bytes ? S_OK : HRESULT_FROM_WIN32(ERROR_DISK_FULL);
    }
};
class NestedCallback final : public SevenObject<IArchiveExtractCallback>,
                             public IArchiveRequestMemoryUseCallback {
    uint32_t id_;
    ComPtr<NestedOutput> output_;
    Cancel cancel_;

  public:
    bool complete = false;
    NestedCallback(uint32_t id, NestedOutput* output, Cancel cancel)
        : id_(id), output_(output), cancel_(std::move(cancel)) {}
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return SevenObject::AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return SevenObject::Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
            *out = static_cast<IArchiveExtractCallback*>(this);
        else if (iid == IID_IArchiveRequestMemoryUseCallback)
            *out = static_cast<IArchiveRequestMemoryUseCallback*>(this);
        else
            return E_NOINTERFACE;
        AddRef();
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 id, ISequentialOutStream** stream,
                                        Int32 mode) noexcept override {
        *stream = nullptr;
        if (cancel_ && cancel_())
            return E_ABORT;
        if (id == id_ && mode == NArchive::NExtract::NAskMode::kExtract) {
            *stream = output_.Get();
            (*stream)->AddRef();
        }
        active_ = id;
        return S_OK;
    }
    uint32_t active_ = UINT32_MAX;
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override {
        if (active_ == id_)
            complete = result == NArchive::NExtract::NOperationResult::kOK;
        return cancel_ && cancel_() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept override {
        return cancel_ && cancel_() ? E_ABORT : S_OK;
    }
    HRESULT STDMETHODCALLTYPE RequestMemoryUse(UInt32, UInt32, UInt32, const wchar_t*, UInt64 size,
                                               UInt64* allowed, UInt32* answer) noexcept override {
        *allowed = std::min<uint64_t>(physicalMemory() / 4, 1024 * MiB);
        *answer = size <= *allowed ? NRequestMemoryAnswerFlags::k_Allow : NRequestMemoryAnswerFlags::k_Stop;
        return S_OK;
    }
};
class ArchiveSource;
class Output final : public SevenObject<ISequentialOutStream> {
  public:
    std::shared_ptr<Bytes> bytes = std::make_shared<Bytes>();
    Cancel cancel;
    uint64_t limit;
    explicit Output(Cancel c, uint64_t l) : cancel(std::move(c)), limit(l) {}
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override {
        *out = nullptr;
        if (iid == IID_IUnknown || iid == IID_ISequentialOutStream) {
            *out = static_cast<ISequentialOutStream*>(this);
            AddRef();
            return S_OK;
        }
        return E_NOINTERFACE;
    }
    HRESULT STDMETHODCALLTYPE Write(const void* data, UInt32 size, UInt32* n) noexcept override {
        if (n)
            *n = 0;
        if (cancel && cancel())
            return E_ABORT;
        if (size > limit || bytes->size() > limit - size)
            return E_OUTOFMEMORY;
        try {
            auto p = (const uint8_t*)data;
            bytes->insert(bytes->end(), p, p + size);
            if (n)
                *n = size;
            return S_OK;
        } catch (...) {
            return E_OUTOFMEMORY;
        }
    }
};
class ExtractCallback final : public SevenObject<IArchiveExtractCallback>,
                              public IArchiveRequestMemoryUseCallback {
    ArchiveSource& owner_;
    uint32_t index_ = 0;
    ComPtr<Output> output_;

  public:
    explicit ExtractCallback(ArchiveSource& a) : owner_(a) {}
    ULONG STDMETHODCALLTYPE AddRef() noexcept override {
        return SevenObject::AddRef();
    }
    ULONG STDMETHODCALLTYPE Release() noexcept override {
        return SevenObject::Release();
    }
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID iid, void** out) noexcept override;
    HRESULT STDMETHODCALLTYPE GetStream(UInt32 index, ISequentialOutStream** stream,
                                        Int32 mode) noexcept override;
    HRESULT STDMETHODCALLTYPE PrepareOperation(Int32) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetOperationResult(Int32 result) noexcept override;
    HRESULT STDMETHODCALLTYPE SetTotal(UInt64) noexcept override {
        return S_OK;
    }
    HRESULT STDMETHODCALLTYPE SetCompleted(const UInt64*) noexcept override;
    HRESULT STDMETHODCALLTYPE RequestMemoryUse(UInt32, UInt32, UInt32, const wchar_t*, UInt64 size,
                                               UInt64* allowed, UInt32* answer) noexcept override {
        *allowed = std::min<uint64_t>(physicalMemory() / 4, 1024 * MiB);
        *answer = size <= *allowed ? NRequestMemoryAnswerFlags::k_Allow : NRequestMemoryAnswerFlags::k_Stop;
        return S_OK;
    }
};
class ArchiveSource final : public IImageSource {
  public:
    std::shared_ptr<RawCache> cache_;
    std::shared_ptr<Metrics> metrics_;
    HMODULE module_ = nullptr;
    ComPtr<IInArchive> archive_;
    ComPtr<IInStream> file_;
    ComPtr<VolumeCallback> volumes_;
    std::vector<fs::path> volumePaths_;
    std::shared_ptr<NestedFile> nestedFile_;
    std::function<std::string()> rootVersion_;
    std::string rootIdentity_;
    std::mutex handlerMutex_;
    std::string volumeVersion() const {
        // Keep existing cache and saved-position keys for ordinary single-file archives.
        if (volumePaths_.size() == 1)
            return fingerprint(volumePaths_.front());
        std::string versions;
        for (const auto& path : volumePaths_)
            versions += fingerprint(path);
        return sha256(std::span<const uint8_t>((const uint8_t*)versions.data(), versions.size()));
    }
    std::mutex mutex_;
    std::condition_variable cv_;
    std::thread worker_;
    std::atomic<bool> stop_{false}, restart_{false};
    std::optional<uint32_t> desired_, activeBlock_;
    uint32_t cursor_ = 0;
    bool inSession_ = false;
    uint64_t serial_ = 0, handled_ = 0;
    uint32_t replyId_ = UINT32_MAX;
    BytePtr reply_;
    std::unordered_map<uint32_t, std::string> errors_;
    std::unordered_map<uint32_t, Entry> byId_;
    std::map<uint32_t, std::vector<uint32_t>> blocks_;
    std::unordered_map<uint32_t, bool> blockSolid_;
    Cancel openingCancel_;
    bool opening_ = true;
    ArchiveSource(const fs::path& p, std::shared_ptr<RawCache> cache, std::shared_ptr<Metrics> metrics,
                  const Cancel& cancel, bool temporary = false)
        : cache_(std::move(cache)), metrics_(std::move(metrics)), openingCancel_(cancel) {
        path_ = p;
        module_ = LoadLibraryExW((executablePath().parent_path() / L"7z.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            throw Error("Не найдена библиотека 7z.dll рядом с Viewer.exe");
        try {
            using Create = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
            auto create = (Create)GetProcAddress(module_, "CreateObject");
            if (!create)
                throw Error("Несовместимая 7z.dll");
            Cancel stopped = [this] {
                return stop_.load() || restart_.load() || (opening_ && openingCancel_ && openingCancel_());
            };
            volumes_.Attach(new VolumeCallback(p, metrics_, stopped));
            if (!fs::is_regular_file(p))
                throw Error("Отсутствует первый том: " + utf8(p.filename().wstring()));
            auto ext = lower(p.stem().extension().wstring());
            if ((ext == L".7z" || ext == L".zip") && p.extension() != L".7z" && p.extension() != L".zip") {
                for (auto& item : fs::directory_iterator(p.parent_path())) {
                    if (cancel && cancel())
                        throw Cancelled();
                    if (item.is_regular_file() &&
                        lower(firstArchiveVolume(item.path()).wstring()) == lower(p.wstring()))
                        volumePaths_.push_back(item.path());
                }
                std::sort(volumePaths_.begin(), volumePaths_.end(), [](auto& a, auto& b) {
                    return naturalLess(a.filename().wstring(), b.filename().wstring());
                });
                for (size_t i = 0; i < volumePaths_.size(); ++i) {
                    auto digits = std::to_wstring(i + 1);
                    size_t width = p.extension().wstring().size() - 1;
                    if (digits.size() < width)
                        digits.insert(0, width - digits.size(), L'0');
                    auto expected = p.parent_path() / (p.stem().wstring() + L"." + digits);
                    if (lower(volumePaths_[i].wstring()) != lower(expected.wstring()))
                        throw Error("Отсутствует том: " + utf8(expected.filename().wstring()));
                }
                if (volumePaths_.empty())
                    throw Error("Отсутствует первый том: " + utf8(p.filename().wstring()));
                file_.Attach(new SplitStream(volumePaths_, metrics_, stopped));
            } else {
                file_.Attach(new FileStream(p, metrics_, stopped, temporary));
            }
            uint8_t signature[8]{};
            UInt32 n = 0;
            check(file_->Read(signature, 8, &n), "Чтение архива");
            file_->Seek(0, STREAM_SEEK_SET, nullptr);
            uint8_t format = 1;
            if (n >= 7 && memcmp(signature, "Rar!\x1a\x07", 6) == 0)
                format = signature[6] == 1 ? 0xCC : 3;
            else if (n >= 6 && memcmp(signature, "7z\xbc\xaf\x27\x1c", 6) == 0)
                format = 7;
            GUID clsid = {0x23170F69, 0x40C1, 0x278A, {0x10, 0, 0, 1, 0x10, format, 0, 0}};
            check(create(&clsid, &IID_IInArchive, (void**)archive_.GetAddressOf()), "Архиватор");
            UInt64 max = 0;
            HRESULT hr = archive_->Open(file_.Get(), &max, volumes_.Get());
            if (cancel && cancel())
                throw Cancelled();
            if (hr != S_OK)
                throw Error("Не удалось открыть архив: повреждение, пароль или отсутствует том" +
                            (volumes_->missing.empty() ? std::string() : ": " + utf8(volumes_->missing)));
            Property flags, error;
            archive_->GetArchiveProperty(kpidErrorFlags, &flags.p);
            archive_->GetArchiveProperty(kpidError, &error.p);
            if (flags.number() || !error.string().empty())
                throw Error("Архив повреждён или отсутствует том: " + utf8(error.string()) + " " +
                            utf8(volumes_->missing));
            if (volumePaths_.empty())
                volumePaths_ = volumes_->paths;
            identity_ = volumeVersion();
            Property encrypted;
            archive_->GetArchiveProperty(kpidEncrypted, &encrypted.p);
            if (encrypted.boolean())
                throw Error("Архивы с паролем пока не поддерживаются");
            UInt32 count = 0;
            check(archive_->GetNumberOfItems(&count), "Каталог архива");
            uint32_t block = 0;
            for (uint32_t i = 0; i < count; ++i) {
                if (cancel && cancel())
                    throw Cancelled();
                Property dir, name, size, solid, enc, link, alt, folder;
                archive_->GetProperty(i, kpidIsDir, &dir.p);
                archive_->GetProperty(i, kpidPath, &name.p);
                archive_->GetProperty(i, kpidSize, &size.p);
                archive_->GetProperty(i, kpidSolid, &solid.p);
                archive_->GetProperty(i, kpidBlock, &folder.p);
                archive_->GetProperty(i, kpidEncrypted, &enc.p);
                archive_->GetProperty(i, kpidSymLink, &link.p);
                archive_->GetProperty(i, kpidIsAltStream, &alt.p);
                if (dir.boolean())
                    continue;
                if (format == 7 && (folder.p.vt == VT_UI4 || folder.p.vt == VT_UI8)) {
                    block = (uint32_t)folder.number();
                    if (blocks_.contains(block))
                        blockSolid_[block] = true;
                } else if (!solid.boolean())
                    block = i;
                else
                    blockSolid_[block] = true;
                if (enc.boolean())
                    throw Error("Архивы с паролем пока не поддерживаются");
                auto entryPath = fs::path(name.string());
                auto extension = lower(entryPath.extension().wstring());
                bool nestedArchive = extension == L".zip" || extension == L".rar" || extension == L".7z";
                if (!link.string().empty() || alt.boolean() || (!isImage(entryPath) && !nestedArchive))
                    continue;
                auto entryName = name.string();
                std::replace(entryName.begin(), entryName.end(), L'\\', L'/');
                Entry e{i, std::move(entryName), size.number(), block, solid.boolean(), nestedArchive};
                entries_.push_back(e);
                byId_[i] = e;
                blocks_[block].push_back(i);
            }
            for (auto& e : entries_)
                e.solid = blockSolid_[e.block];
            std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
                if (a.name == b.name)
                    return a.id < b.id;
                return naturalLess(a.name, b.name);
            });
            opening_ = false;
            openingCancel_ = {};
            worker_ = std::thread([this] { run(); });
        } catch (...) {
            archive_.Reset();
            file_.Reset();
            FreeLibrary(module_);
            module_ = nullptr;
            throw;
        }
    }
    ~ArchiveSource() {
        stop_ = true;
        cv_.notify_all();
        if (worker_.joinable())
            worker_.join();
        if (archive_)
            archive_->Close();
        archive_.Reset();
        file_.Reset();
        if (module_)
            FreeLibrary(module_);
    }
    bool interrupted() const {
        return stop_ || restart_;
    }
    void cancel() override {
        {
            std::lock_guard lock(mutex_);
            desired_.reset();
            restart_ = true;
        }
        cv_.notify_all();
    }
    bool isSolid(uint32_t id) const override {
        auto i = byId_.find(id);
        return i != byId_.end() && blockSolid_.contains(i->second.block) && blockSolid_.at(i->second.block);
    }
    std::string version(uint32_t) const override {
        if (rootVersion_) {
            if (rootVersion_() != rootIdentity_)
                throw Error("Внешний архив изменён. Откройте его заново.");
            return identity_;
        }
        if (volumeVersion() != identity_)
            throw Error("Архив изменён. Откройте его заново.");
        return identity_;
    }
    BytePtr cached(uint32_t id) override {
        return cache_->get(identity_, id);
    }
    std::unique_ptr<IImageSource> nested(uint32_t id, const Cancel& cancelled) override {
        if (!byId_.contains(id) || !byId_.at(id).archive)
            throw Error("Вложенный архив отсутствует");
        const auto entry = byId_.at(id);
        if (entry.size > NestedOutput::limit)
            throw Error("Вложенный архив превышает лимит 2 ГиБ");
        auto parentVersion = version(id);
        cancel();
        {
            std::unique_lock lock(mutex_);
            while (inSession_) {
                if (cancelled && cancelled())
                    throw Cancelled();
                cv_.wait_for(lock, std::chrono::milliseconds(20));
            }
        }
        if (cancelled && cancelled())
            throw Cancelled();
        std::lock_guard handlerLock(handlerMutex_);
        restart_ = false;
        auto temp = std::make_shared<NestedFile>();
        if (fs::space(temp->path.parent_path()).available < entry.size)
            throw Error("Недостаточно места для вложенного архива");
        ComPtr<NestedOutput> output;
        output.Attach(new NestedOutput(temp, cancelled));
        ComPtr<NestedCallback> callback;
        callback.Attach(new NestedCallback(id, output.Get(), cancelled));
        ++metrics_->extractions;
        HRESULT hr = archive_->Extract(&id, 1, 0, callback.Get());
        if (cancelled && cancelled())
            throw Cancelled();
        check(hr, "Извлечение вложенного архива");
        if (!callback->complete || output->size != entry.size)
            throw Error("Вложенный архив повреждён или не прошёл проверку CRC");
        ++metrics_->extractedEntries;
        metrics_->extractedBytes += output->size;
        auto child = std::make_unique<ArchiveSource>(temp->path, cache_, metrics_, cancelled, true);
        child->nestedFile_ = temp;
        child->path_ = nestedSourcePath(path_, entry);
        bool found = false;
        for (const auto& sibling : entries_) {
            if (!sibling.archive)
                continue;
            if (sibling.id == id) {
                found = true;
                continue;
            }
            if (found) {
                child->nextArchive_ = nestedSourcePath(path_, sibling);
                break;
            }
            child->previousArchive_ = nestedSourcePath(path_, sibling);
        }
        child->displayName_ =
            (displayName_.empty() ? path_.filename().wstring() : displayName_) + L" › " + entry.name;
        auto key = parentVersion + ":nested:" + std::to_string(id);
        child->identity_ = sha256(std::span<const uint8_t>((const uint8_t*)key.data(), key.size()));
        if (rootVersion_) {
            child->rootVersion_ = rootVersion_;
            child->rootIdentity_ = rootIdentity_;
        } else {
            child->rootIdentity_ = parentVersion;
            child->rootVersion_ = [paths = volumePaths_] {
                if (paths.size() == 1)
                    return fingerprint(paths.front());
                std::string versions;
                for (const auto& p : paths)
                    versions += fingerprint(p);
                return sha256(std::span<const uint8_t>((const uint8_t*)versions.data(), versions.size()));
            };
        }
        return child;
    }
    BytePtr read(uint32_t id, const Cancel& cancel) override {
        if (byId_.contains(id) && byId_.at(id).archive)
            throw Error("Выберите вложенный архив в дереве файлов");
        version(id);
        if (auto b = cached(id))
            return b;
        if (!byId_.contains(id))
            throw Error("Изображение отсутствует");
        std::unique_lock lock(mutex_);
        if (replyId_ == id && reply_)
            return reply_;
        if (errors_.contains(id))
            throw Error(errors_[id]);
        desired_ = id;
        ++serial_;
        auto block = byId_.at(id).block;
        if (inSession_ && (!activeBlock_ || *activeBlock_ != block || id < cursor_))
            restart_ = true;
        cv_.notify_all();
        for (;;) {
            if (cancel && cancel()) {
                desired_.reset();
                restart_ = true;
                cv_.notify_all();
                throw Cancelled();
            }
            if (stop_)
                throw Cancelled();
            if (replyId_ == id && reply_)
                return reply_;
            if (errors_.contains(id))
                throw Error(errors_[id]);
            cv_.wait_for(lock, std::chrono::milliseconds(20));
        }
    }
    HRESULT gate(uint32_t id) {
        std::unique_lock lock(mutex_);
        cursor_ = id;
        cv_.wait(lock, [&] { return interrupted() || (desired_ && id <= *desired_); });
        return interrupted() ? E_ABORT : S_OK;
    }
    void complete(uint32_t id, BytePtr bytes, Int32 result) {
        if (result == NArchive::NExtract::NOperationResult::kOK && bytes) {
            cache_->put(identity_, id, bytes);
            ++metrics_->extractedEntries;
            metrics_->extractedBytes += bytes->size();
        }
        {
            std::lock_guard lock(mutex_);
            if (result != NArchive::NExtract::NOperationResult::kOK)
                errors_[id] = "Ошибка распаковки или контрольной суммы (" + std::to_string(result) + ")";
            else if (bytes) {
                replyId_ = id;
                reply_ = std::move(bytes);
            }
        }
        cv_.notify_all();
    }
    void run() {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        while (!stop_) {
            uint32_t target = 0, block = 0;
            uint64_t serial = 0;
            {
                std::unique_lock lock(mutex_);
                cv_.wait(lock, [&] { return stop_ || (desired_ && serial_ != handled_); });
                if (stop_)
                    break;
                target = *desired_;
                block = byId_.at(target).block;
                serial = serial_;
                handled_ = serial;
                restart_ = false;
                activeBlock_ = block;
                cursor_ = block;
                inSession_ = true;
            }
            std::vector<UInt32> indices = isSolid(target) ? blocks_[block] : std::vector<UInt32>{target};
            ComPtr<ExtractCallback> callback;
            callback.Attach(new ExtractCallback(*this));
            ++metrics_->extractions;
            HRESULT hr;
            {
                std::lock_guard handlerLock(handlerMutex_);
                hr = archive_->Extract(indices.data(), (UInt32)indices.size(), 0, callback.Get());
            }
            {
                std::lock_guard lock(mutex_);
                inSession_ = false;
                activeBlock_.reset();
                if (hr == E_ABORT && restart_) {
                    ++metrics_->solidRestarts;
                    handled_ = 0;
                } else if (FAILED(hr) && !stop_) {
                    if (desired_)
                        errors_[*desired_] = hr == E_OUTOFMEMORY
                                                 ? "Недостаточно памяти для элемента архива"
                                                 : "Ошибка чтения архива: " + utf8(winError(hr));
                    handled_ = serial_;
                } else if (SUCCEEDED(hr)) {
                    // A new backward/other-block request can arrive after the last
                    // callback but before Extract returns. Do not consume it here.
                    if (!desired_ || replyId_ == *desired_ || errors_.contains(*desired_))
                        handled_ = serial_;
                    else if (serial_ == serial) {
                        errors_[*desired_] = "Архив не вернул запрошенный элемент";
                        handled_ = serial_;
                    } else
                        handled_ = 0;
                }
            }
            cv_.notify_all();
        }
        CoUninitialize();
    }
};
HRESULT ExtractCallback::QueryInterface(REFIID iid, void** out) noexcept {
    *out = nullptr;
    if (iid == IID_IUnknown || iid == IID_IArchiveExtractCallback || iid == IID_IProgress)
        *out = static_cast<IArchiveExtractCallback*>(this);
    else if (iid == IID_IArchiveRequestMemoryUseCallback)
        *out = static_cast<IArchiveRequestMemoryUseCallback*>(this);
    else
        return E_NOINTERFACE;
    AddRef();
    return S_OK;
}
HRESULT ExtractCallback::GetStream(UInt32 index, ISequentialOutStream** stream, Int32 mode) noexcept {
    *stream = nullptr;
    output_.Reset();
    index_ = index;
    try {
        if (owner_.interrupted())
            return E_ABORT;
        HRESULT hr = owner_.gate(index);
        if (FAILED(hr))
            return hr;
        // Stop before skipped non-image dependencies beyond the requested image too.
        if (mode != NArchive::NExtract::NAskMode::kExtract)
            return S_OK;
        const uint64_t limit = std::min<uint64_t>(512 * MiB, physicalMemory() / 16);
        auto e = owner_.byId_.find(index);
        if (e == owner_.byId_.end())
            return S_OK;
        if (e->second.archive)
            return S_OK;
        if (e->second.size > limit)
            return E_OUTOFMEMORY;
        output_.Attach(new Output([this] { return owner_.interrupted(); }, limit));
        output_->bytes->reserve((size_t)e->second.size);
        *stream = output_.Get();
        (*stream)->AddRef();
        return S_OK;
    } catch (...) {
        return E_OUTOFMEMORY;
    }
}
HRESULT ExtractCallback::SetOperationResult(Int32 result) noexcept {
    try {
        owner_.complete(index_, output_ ? output_->bytes : BytePtr{}, result);
        output_.Reset();
        return owner_.interrupted() ? E_ABORT : S_OK;
    } catch (...) {
        return E_FAIL;
    }
}
HRESULT ExtractCallback::SetCompleted(const UInt64*) noexcept {
    return owner_.interrupted() ? E_ABORT : S_OK;
}
} // namespace
std::unique_ptr<IImageSource> openArchive(const fs::path& p, std::shared_ptr<RawCache> cache,
                                          std::shared_ptr<Metrics> metrics, const Cancel& cancel) {
    return std::make_unique<ArchiveSource>(p, std::move(cache), std::move(metrics), cancel);
}
} // namespace viewer
