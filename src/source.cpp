#include "source.h"
namespace viewer {
namespace {
class FolderSource final : public IImageSource {
    std::shared_ptr<RawCache> cache_;

  public:
    FolderSource(const fs::path& input, std::shared_ptr<RawCache> cache, const Cancel& cancel)
        : cache_(std::move(cache)) {
        path_ = fs::absolute(fs::is_directory(input) ? input : input.parent_path()).lexically_normal();
        identity_ = fingerprint(path_);
        for (const auto& item :
             fs::directory_iterator(path_, fs::directory_options::skip_permission_denied)) {
            if (cancel && cancel())
                throw Cancelled();
            if (item.is_regular_file() && isImage(item.path()))
                entries_.push_back({0, item.path().filename().wstring(), item.file_size()});
        }
        std::sort(entries_.begin(), entries_.end(),
                  [](const Entry& a, const Entry& b) { return naturalLess(a.name, b.name); });
        for (uint32_t i = 0; i < entries_.size(); ++i)
            entries_[i].id = i;
    }
    std::string version(uint32_t id) const override {
        if (id >= entries_.size())
            throw Error("Нет изображения");
        return fingerprint(path_ / entries_[id].name);
    }
    BytePtr read(uint32_t id, const Cancel& cancel) override {
        auto key = version(id);
        if (auto b = cache_->get(key, id))
            return b;
        auto b = readFile(path_ / entries_[id].name, cancel);
        cache_->put(key, id, b);
        return b;
    }
    BytePtr cached(uint32_t id) override {
        return cache_->get(version(id), id);
    }
    bool isSolid(uint32_t) const override {
        return false;
    }
};
} // namespace
fs::path sourceRootPath(fs::path path) {
    // Virtual addresses contain archive entry IDs, never untrusted internal paths.
    for (int depth = 0; depth < 4 && !fs::exists(path); ++depth) {
        auto stem = path.stem().wstring();
        if (stem.size() < 2 || stem[0] != L'@' || !isArchive(path) ||
            !std::all_of(stem.begin() + 1, stem.end(), [](wchar_t c) { return c >= L'0' && c <= L'9'; }))
            break;
        path = path.parent_path();
    }
    return path;
}
fs::path nestedSourcePath(const fs::path& parent, const Entry& entry) {
    return parent / (L"@" + std::to_wstring(entry.id) + lower(fs::path(entry.name).extension().wstring()));
}
std::unique_ptr<IImageSource> openSource(const fs::path& input, std::shared_ptr<RawCache> cache,
                                         std::shared_ptr<Metrics> metrics, const Cancel& cancel) {
    auto path = fs::absolute(input).lexically_normal();
    auto root = sourceRootPath(path);
    if (root != path) {
        if (!isArchive(root) || !fs::is_regular_file(root))
            throw Error("Внешний архив отсутствует");
        auto source = openArchive(firstArchiveVolume(root), cache, metrics, cancel);
        size_t depth = 0;
        for (const auto& part : path.lexically_relative(root)) {
            if (++depth > 3)
                throw Error("Поддерживается до трёх уровней вложенных архивов");
            auto number = std::stoull(part.stem().wstring().substr(1));
            if (number > UINT32_MAX)
                throw Error("Некорректный адрес вложенного архива");
            source = source->nested((uint32_t)number, cancel);
        }
        return source;
    }
    if (fs::is_directory(path) || isImage(path))
        return std::make_unique<FolderSource>(path, std::move(cache), cancel);
    if (isArchive(path))
        return openArchive(firstArchiveVolume(path), std::move(cache), std::move(metrics), cancel);
    throw Error("Выберите изображение, папку, ZIP, RAR или 7z");
}
} // namespace viewer
