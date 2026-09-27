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
std::unique_ptr<IImageSource> openSource(const fs::path& input, std::shared_ptr<RawCache> cache,
                                         std::shared_ptr<Metrics> metrics, const Cancel& cancel) {
    auto path = fs::absolute(input).lexically_normal();
    if (fs::is_directory(path) || isImage(path))
        return std::make_unique<FolderSource>(path, std::move(cache), cancel);
    if (isArchive(path))
        return openArchive(firstArchiveVolume(path), std::move(cache), std::move(metrics), cancel);
    throw Error("Выберите изображение, папку, ZIP, RAR или 7z");
}
} // namespace viewer
