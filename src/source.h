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
};
std::unique_ptr<IImageSource> openSource(const fs::path& path, std::shared_ptr<RawCache> cache,
                                         std::shared_ptr<Metrics> metrics, const Cancel& cancel);
std::unique_ptr<IImageSource> openArchive(const fs::path& path, std::shared_ptr<RawCache> cache,
                                          std::shared_ptr<Metrics> metrics, const Cancel& cancel);
} // namespace viewer
