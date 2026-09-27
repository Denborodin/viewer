#pragma once
#include "cache.h"
namespace viewer {
class IImageSource {
  public:
    virtual ~IImageSource() = default;
    virtual BytePtr read(uint32_t id, const Cancel& cancel) = 0;
    virtual BytePtr cached(uint32_t id) = 0;
    virtual std::string version(uint32_t id) const = 0;
    virtual bool isSolid(uint32_t id) const = 0;
    virtual void cancel() {}
    virtual std::unique_ptr<IImageSource> nested(uint32_t, const Cancel&) {
        throw Error("Источник не содержит вложенных архивов");
    }
    const std::wstring& displayName() const {
        return displayName_;
    }
    const fs::path& previousArchive() const {
        return previousArchive_;
    }
    const fs::path& nextArchive() const {
        return nextArchive_;
    }
    const std::vector<Entry>& entries() const {
        return entries_;
    }
    const fs::path& path() const {
        return path_;
    }
    const std::string& identity() const {
        return identity_;
    }

  protected:
    fs::path path_;
    std::string identity_;
    std::vector<Entry> entries_;
    std::wstring displayName_;
    fs::path previousArchive_, nextArchive_;
};
fs::path sourceRootPath(fs::path path);
fs::path nestedSourcePath(const fs::path& parent, const Entry& entry);
std::unique_ptr<IImageSource> openSource(const fs::path& path, std::shared_ptr<RawCache> cache,
                                         std::shared_ptr<Metrics> metrics, const Cancel& cancel);
std::unique_ptr<IImageSource> openArchive(const fs::path& path, std::shared_ptr<RawCache> cache,
                                          std::shared_ptr<Metrics> metrics, const Cancel& cancel);
} // namespace viewer
