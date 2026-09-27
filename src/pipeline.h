#pragma once
#include "decode.h"
#include "source.h"
namespace viewer {
struct Event {
    enum class Type { Opened, FrameReady, Thumbnail, Error, Bookmarks, SettingsSaved, Notice };
    Type type;
    uint64_t generation = 0, sourceToken = 0;
    size_t index = 0;
    fs::path path;
    std::vector<Entry> entries;
    std::shared_ptr<Frame> frame;
    std::wstring message;
    std::vector<Bookmark> bookmarks;
    bool bookmarked = false;
    fs::path previousArchive, nextArchive;
};
class ImagePipeline {
  public:
    using Notify = std::function<void(std::unique_ptr<Event>)>;
    ImagePipeline(std::shared_ptr<StateStore> state, Notify notify);
    ~ImagePipeline();
    uint64_t open(fs::path path, std::optional<uint32_t> entry = {});
    uint64_t select(size_t index, int direction = 1);
    uint64_t resize(uint32_t width, uint32_t height);
    void thumbnails(size_t first, size_t last);
    void toggleBookmark();
    void requestBookmarks();
    void configure(Settings settings);
    void rememberWindowPlacement(WINDOWPLACEMENT placement);
    void clearCache();
    void cancel();
    uint64_t generation() const {
        return generation_;
    }
    uint64_t sourceToken() const {
        return sourceToken_;
    }
    std::shared_ptr<Metrics> metrics() const {
        return metrics_;
    }

  private:
    std::shared_ptr<StateStore> state_;
    std::shared_ptr<Metrics> metrics_;
    std::shared_ptr<RawCache> raw_;
    Notify notify_;
    std::thread thread_;
    std::mutex mutex_;
    std::condition_variable cv_;
    bool stop_ = false;
    std::optional<WINDOWPLACEMENT> finalWindowPlacement_;
    std::atomic<bool> stopping_ = false;
    std::atomic<uint64_t> generation_{0}, sourceToken_{0};
    std::atomic<uint64_t> cancelSerial_{0};
    fs::path pendingPath_;
    bool opening_ = false, paused_ = false;
    std::optional<uint32_t> openingEntry_;
    size_t wanted_ = 0, thumbFirst_ = 0, thumbLast_ = 0;
    int direction_ = 1;
    uint32_t width_ = 1920, height_ = 1080;
    bool thumbDirty_ = false;
    std::deque<std::function<void()>> commands_;
    Settings settings_;
    std::unique_ptr<IImageSource> source_;
    std::unique_ptr<ImageDecoder> decoder_;
    struct Decoded {
        std::shared_ptr<Frame> frame;
        uint64_t stamp;
        uint32_t w, h;
    };
    std::unordered_map<std::string, Decoded> decoded_;
    uint64_t decodedBytes_ = 0, tick_ = 0;
    std::shared_ptr<Frame> image(size_t index, uint32_t w, uint32_t h, const Cancel& cancel,
                                 bool cacheOnly = false);
    void run();
    void emit(Event event);
    void command(std::function<void()> fn);
};
} // namespace viewer
