#include "pipeline.h"
namespace viewer {
ImagePipeline::ImagePipeline(std::shared_ptr<StateStore> s, Notify notify)
    : state_(std::move(s)), metrics_(std::make_shared<Metrics>()), notify_(std::move(notify)),
      settings_(state_->settings()) {
    raw_ = std::make_shared<RawCache>(state_, settings_, metrics_);
    thread_ = std::thread([this] { run(); });
}
ImagePipeline::~ImagePipeline() {
    stopping_ = true;
    ++generation_;
    {
        std::lock_guard lock(mutex_);
        stop_ = true;
    }
    cv_.notify_all();
    if (thread_.joinable())
        thread_.join();
    source_.reset();
    raw_.reset();
}
uint64_t ImagePipeline::open(fs::path p, std::optional<uint32_t> entry) {
    {
        std::lock_guard lock(mutex_);
        pendingPath_ = std::move(p);
        opening_ = true;
        paused_ = false;
        openingEntry_ = entry;
        wanted_ = 0;
        thumbFirst_ = thumbLast_ = 0;
        ++generation_;
        ++sourceToken_;
    }
    cv_.notify_all();
    return sourceToken_;
}
uint64_t ImagePipeline::select(size_t i, int direction) {
    {
        std::lock_guard lock(mutex_);
        paused_ = false;
        wanted_ = i;
        direction_ = direction;
        ++generation_;
    }
    cv_.notify_all();
    return generation_;
}
uint64_t ImagePipeline::resize(uint32_t w, uint32_t h) {
    {
        std::lock_guard lock(mutex_);
        if (w == width_ && h == height_)
            return generation_;
        width_ = std::max(1u, w);
        height_ = std::max(1u, h);
        ++generation_;
    }
    cv_.notify_all();
    return generation_;
}
void ImagePipeline::thumbnails(size_t first, size_t last) {
    {
        std::lock_guard lock(mutex_);
        thumbFirst_ = first;
        thumbLast_ = last;
        thumbDirty_ = true;
    }
    cv_.notify_all();
}
void ImagePipeline::command(std::function<void()> fn) {
    {
        std::lock_guard lock(mutex_);
        commands_.push_back(std::move(fn));
    }
    cv_.notify_all();
}
void ImagePipeline::configure(Settings s) {
    command([this, s] {
        settings_ = s;
        raw_->configure(s);
        state_->saveSettings(s);
        decoded_.clear();
        decodedBytes_ = 0;
        emit({Event::Type::SettingsSaved});
        ++generation_;
    });
}
void ImagePipeline::clearCache() {
    command([this] {
        raw_->clear();
        decoded_.clear();
        decodedBytes_ = 0;
        emit({Event::Type::Notice, 0, 0, 0, {}, {}, {}, L"Кеш очищается"});
    });
}
void ImagePipeline::cancel() {
    {
        std::lock_guard lock(mutex_);
        paused_ = true;
        ++generation_;
        commands_.push_back([this] {
            if (source_)
                source_->cancel();
        });
    }
    cv_.notify_all();
}
void ImagePipeline::toggleBookmark() {
    size_t wanted;
    {
        std::lock_guard lock(mutex_);
        wanted = wanted_;
    }
    command([this, wanted] {
        if (!source_ || wanted >= source_->entries().size())
            return;
        const auto& e = source_->entries()[wanted];
        state_->toggleBookmark({source_->path().wstring(), e.name, e.id});
        Event event{Event::Type::Notice};
        event.message =
            state_->bookmarked(source_->path(), e.id) ? L"Закладка добавлена" : L"Закладка удалена";
        event.bookmarked = state_->bookmarked(source_->path(), e.id);
        emit(std::move(event));
    });
}
void ImagePipeline::requestBookmarks() {
    command([this] {
        Event e{Event::Type::Bookmarks};
        e.bookmarks = state_->bookmarks();
        emit(std::move(e));
    });
}
void ImagePipeline::emit(Event e) {
    if (!e.sourceToken)
        e.sourceToken = sourceToken_;
    notify_(std::make_unique<Event>(std::move(e)));
}
std::shared_ptr<Frame> ImagePipeline::image(size_t index, uint32_t w, uint32_t h, const Cancel& cancel,
                                            bool cacheOnly) {
    const auto& entry = source_->entries().at(index);
    std::string key = source_->version(entry.id) + ":" + std::to_string(entry.id) +
                      (w <= 256 && h <= 256 ? ":thumb" : ":image");
    auto found = decoded_.find(key);
    if (found != decoded_.end() && found->second.w >= w && found->second.h >= h) {
        found->second.stamp = ++tick_;
        return found->second.frame;
    }
    BytePtr bytes = cacheOnly ? source_->cached(entry.id) : source_->read(entry.id, cancel);
    if (!bytes)
        return {};
    uint64_t limit = std::min(settings_.ramBytes, physicalMemory() / 8) / 3;
    // Reserve room for the visible frame, pending upload and small thumbnails.
    auto frame = decoder_->decode(bytes, w, h,
                                  std::min(settings_.gpuBytes - 8 * MiB, std::max<uint64_t>(MiB, limit / 2)),
                                  cancel, {JpegBackend::Automatic, w > 256 || h > 256});
    if (cancel && cancel())
        throw Cancelled();
    if (found != decoded_.end()) {
        decodedBytes_ -= found->second.frame->pixels.size();
        decoded_.erase(found);
    }
    while (decodedBytes_ + frame->pixels.size() > limit && !decoded_.empty()) {
        auto victim = std::min_element(decoded_.begin(), decoded_.end(), [](const auto& a, const auto& b) {
            return a.second.stamp < b.second.stamp;
        });
        decodedBytes_ -= victim->second.frame->pixels.size();
        decoded_.erase(victim);
    }
    decodedBytes_ += frame->pixels.size();
    decoded_[key] = {frame, ++tick_, w, h};
    return frame;
}
void ImagePipeline::run() {
    CoInitializeEx(nullptr, COINIT_MULTITHREADED);
    decoder_ = std::make_unique<ImageDecoder>();
    uint64_t completed = 0;
    uint64_t localSource = 0;
    for (;;) {
        uint64_t gen = 0, token = 0;
        size_t index = 0, first = 0, last = 0;
        uint32_t w = 0, h = 0;
        int direction = 1;
        bool open = false, thumbs = false, paused = false;
        fs::path path;
        std::optional<uint32_t> initial;
        std::deque<std::function<void()>> commands;
        {
            std::unique_lock lock(mutex_);
            cv_.wait(lock, [&] {
                return stop_ || opening_ || generation_ != completed || thumbDirty_ || !commands_.empty();
            });
            if (stop_)
                break;
            gen = generation_;
            token = sourceToken_;
            open = std::exchange(opening_, false);
            paused = paused_;
            path = pendingPath_;
            initial = openingEntry_;
            index = wanted_;
            direction = direction_;
            w = width_;
            h = height_;
            first = thumbFirst_;
            last = thumbLast_;
            thumbs = std::exchange(thumbDirty_, false);
            commands.swap(commands_);
        }
        auto cancel = [this, &gen] { return stopping_ || generation_ != gen; };
        try {
            for (auto& fn : commands)
                fn();
            if (open) {
                source_.reset();
                decoded_.clear();
                decodedBytes_ = 0;
                source_ = openSource(path, raw_, metrics_,
                                     [this, token] { return stopping_ || sourceToken_ != token; });
                localSource = token;
                if (source_->entries().empty())
                    throw Error("В выбранном источнике нет поддерживаемых изображений");
                auto saved = initial ? initial : state_->position(source_->path(), source_->identity());
                index = 0;
                for (size_t i = 0; i < source_->entries().size(); ++i) {
                    const auto& e = source_->entries()[i];
                    if ((saved && e.id == *saved) ||
                        (!fs::is_directory(path) && isImage(path) && e.name == path.filename().wstring()))
                        index = i;
                }
                {
                    std::lock_guard lock(mutex_);
                    if (stopping_ || sourceToken_ != token)
                        throw Cancelled();
                    gen = generation_;
                    w = width_;
                    h = height_;
                    wanted_ = index;
                }
                Event e{Event::Type::Opened};
                e.generation = gen;
                e.sourceToken = token;
                e.index = index;
                e.path = source_->path();
                e.entries = source_->entries();
                emit(std::move(e));
            }
            if (!paused && source_ && localSource == token && index < source_->entries().size() &&
                gen != completed) {
                auto frame = image(index, w, h, cancel);
                if (cancel())
                    throw Cancelled();
                Event e{Event::Type::FrameReady};
                e.generation = gen;
                e.sourceToken = token;
                e.index = index;
                e.frame = std::move(frame);
                if (raw_->diskFailed())
                    e.message = L"Дисковый кеш недоступен";
                e.bookmarked = state_->bookmarked(source_->path(), source_->entries()[index].id);
                emit(std::move(e));
                state_->savePosition(source_->path(), source_->identity(), source_->entries()[index].id);
                for (int offset : {direction, 2 * direction, -direction}) {
                    int64_t next = (int64_t)index + offset;
                    if (next < 0 || next >= (int64_t)source_->entries().size())
                        continue;
                    if (cancel())
                        break;
                    try {
                        image((size_t)next, w, h, cancel);
                    } catch (const Cancelled&) {
                        break;
                    } catch (...) {
                    }
                }
                thumbs = true;
            }
            if (!paused && thumbs && source_ && localSource == token) {
                for (size_t i = first; i < std::min(last, source_->entries().size()); ++i) {
                    if (cancel())
                        break;
                    try {
                        auto f = image(i, 160, 128, cancel, source_->isSolid(source_->entries()[i].id));
                        if (f) {
                            Event e{Event::Type::Thumbnail};
                            e.generation = gen;
                            e.sourceToken = token;
                            e.index = i;
                            e.frame = std::move(f);
                            emit(std::move(e));
                        }
                    } catch (...) {
                    }
                }
            }
        } catch (const Cancelled&) {
        } catch (const std::exception& ex) {
            if (!cancel()) {
                Event e{Event::Type::Error};
                e.generation = gen;
                e.sourceToken = token;
                e.message = wide(ex.what());
                emit(std::move(e));
            }
        }
        completed = gen;
    }
    source_.reset();
    decoder_.reset();
    CoUninitialize();
}
} // namespace viewer
