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
    FileStream(const fs::path& p, std::shared_ptr<Metrics> m, Cancel cancel)
        : metrics_(std::move(m)), cancel_(std::move(cancel)) {
        file_ = CreateFileW(p.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING,
                            FILE_ATTRIBUTE_NORMAL, nullptr);
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
    ComPtr<FileStream> file_;
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
                  const Cancel& cancel)
        : cache_(std::move(cache)), metrics_(std::move(metrics)), openingCancel_(cancel) {
        path_ = p;
        identity_ = fingerprint(p);
        module_ = LoadLibraryExW((executablePath().parent_path() / L"7z.dll").c_str(), nullptr,
                                 LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR | LOAD_LIBRARY_SEARCH_SYSTEM32);
        if (!module_)
            throw Error("Не найдена библиотека 7z.dll рядом с Viewer.exe");
        try {
            using Create = HRESULT(WINAPI*)(const GUID*, const GUID*, void**);
            auto create = (Create)GetProcAddress(module_, "CreateObject");
            if (!create)
                throw Error("Несовместимая 7z.dll");
            file_.Attach(new FileStream(p, metrics_, [this] {
                return stop_.load() || restart_.load() || (opening_ && openingCancel_ && openingCancel_());
            }));
            uint8_t signature[8]{};
            UInt32 n = 0;
            check(file_->Read(signature, 8, &n), "Чтение архива");
            file_->Seek(0, STREAM_SEEK_SET, nullptr);
            uint8_t format = 1;
            if (n >= 7 && memcmp(signature, "Rar!\x1a\x07", 6) == 0)
                format = signature[6] == 1 ? 0xCC : 3;
            GUID clsid = {0x23170F69, 0x40C1, 0x278A, {0x10, 0, 0, 1, 0x10, format, 0, 0}};
            check(create(&clsid, &IID_IInArchive, (void**)archive_.GetAddressOf()), "Архиватор");
            UInt64 max = 0;
            HRESULT hr = archive_->Open(file_.Get(), &max, nullptr);
            if (hr != S_OK)
                throw Error("Не удалось открыть архив: повреждение, пароль или неподдерживаемый формат");
            Property volume;
            archive_->GetArchiveProperty(kpidIsVolume, &volume.p);
            if (volume.boolean())
                throw Error("Многотомные архивы пока не поддерживаются");
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
                Property dir, name, size, solid, enc, link, alt;
                archive_->GetProperty(i, kpidIsDir, &dir.p);
                archive_->GetProperty(i, kpidPath, &name.p);
                archive_->GetProperty(i, kpidSize, &size.p);
                archive_->GetProperty(i, kpidSolid, &solid.p);
                archive_->GetProperty(i, kpidEncrypted, &enc.p);
                archive_->GetProperty(i, kpidSymLink, &link.p);
                archive_->GetProperty(i, kpidIsAltStream, &alt.p);
                if (dir.boolean())
                    continue;
                if (!solid.boolean())
                    block = i;
                else
                    blockSolid_[block] = true;
                if (enc.boolean())
                    throw Error("Архивы с паролем пока не поддерживаются");
                if (!link.string().empty() || alt.boolean() || !isImage(fs::path(name.string())))
                    continue;
                auto entryName = name.string();
                std::replace(entryName.begin(), entryName.end(), L'\\', L'/');
                Entry e{i, std::move(entryName), size.number(), block, solid.boolean()};
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
        if (fingerprint(path_) != identity_)
            throw Error("Архив изменён. Откройте его заново.");
        return identity_;
    }
    BytePtr cached(uint32_t id) override {
        return cache_->get(identity_, id);
    }
    BytePtr read(uint32_t id, const Cancel& cancel) override {
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
            HRESULT hr = archive_->Extract(indices.data(), (UInt32)indices.size(), 0, callback.Get());
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
                    } else handled_ = 0;
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
