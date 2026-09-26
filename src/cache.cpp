#include "cache.h"
namespace viewer {
std::string RawCache::key(const std::string& source, uint32_t id) {
    return source + "-" + std::to_string(id);
}
RawCache::RawCache(std::shared_ptr<StateStore> state, Settings settings, std::shared_ptr<Metrics> metrics)
    : state_(std::move(state)), metrics_(std::move(metrics)), dir_(state_->root() / L"cache") {
    std::error_code ec;
    fs::create_directories(dir_, ec);
    diskFailed_ = bool(ec);
    configure(settings);
    io_ = std::thread([this] { run(); });
}
RawCache::~RawCache() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        writes_.clear();
    }
    cv_.notify_all();
    if (io_.joinable())
        io_.join();
}
void RawCache::configure(Settings s) {
    {
        std::lock_guard lock(mutex_);
        ramLimit_ = std::min(s.ramBytes, physicalMemory() / 8) / 3;
        diskLimit_ = s.diskBytes;
        while (used_ > ramLimit_ && !memory_.empty()) {
            auto i = std::min_element(memory_.begin(), memory_.end(), [](const auto& a, const auto& b) {
                return a.second.stamp < b.second.stamp;
            });
            used_ -= i->second.bytes->size();
            memory_.erase(i);
        }
    }
    cv_.notify_all();
}
void RawCache::remember(const std::string& k, BytePtr bytes) {
    std::lock_guard lock(mutex_);
    if (bytes->size() > ramLimit_)
        return;
    auto old = memory_.find(k);
    if (old != memory_.end()) {
        old->second.stamp = ++tick_;
        return;
    }
    while (used_ + bytes->size() > ramLimit_ && !memory_.empty()) {
        auto i = std::min_element(memory_.begin(), memory_.end(), [](const auto& a, const auto& b) {
            return a.second.stamp < b.second.stamp;
        });
        used_ -= i->second.bytes->size();
        memory_.erase(i);
    }
    used_ += bytes->size();
    memory_[k] = {std::move(bytes), ++tick_};
}
BytePtr RawCache::get(const std::string& source, uint32_t id) {
    auto k = key(source, id);
    {
        std::lock_guard lock(mutex_);
        auto i = memory_.find(k);
        if (i != memory_.end()) {
            i->second.stamp = ++tick_;
            ++metrics_->cacheHits;
            return i->second.bytes;
        }
        for (auto& w : writes_)
            if (w.key == k) {
                ++metrics_->cacheHits;
                return w.bytes;
            }
        if (!diskLimit_)
            return {};
    }
    try {
        auto p = dir_ / wide(k);
        if (!fs::exists(p))
            return {};
        auto b = readFile(p);
        state_->touchDisk(k);
        remember(k, b);
        ++metrics_->cacheHits;
        return b;
    } catch (...) {
        return {};
    }
}
void RawCache::put(const std::string& source, uint32_t id, BytePtr bytes) {
    auto k = key(source, id);
    remember(k, bytes);
    {
        std::lock_guard lock(mutex_);
        if (!diskLimit_ || diskFailed_ || bytes->size() > diskLimit_ ||
            pending_ + bytes->size() > std::min<uint64_t>(64 * MiB, ramLimit_ / 4))
            return;
        for (auto& w : writes_)
            if (w.key == k)
                return;
        pending_ += bytes->size();
        writes_.push_back({std::move(k), std::move(bytes)});
    }
    cv_.notify_one();
}
uint64_t RawCache::memoryUsage() const {
    std::lock_guard lock(mutex_);
    return used_ + pending_;
}
void RawCache::clear() {
    {
        std::lock_guard lock(mutex_);
        memory_.clear();
        used_ = 0;
        writes_.clear();
        pending_ = 0;
        clear_ = true;
        diskFailed_ = false;
    }
    cv_.notify_one();
}
void RawCache::trimDisk() {
    auto records = state_->diskRecords();
    uint64_t total = 0, limit;
    {
        std::lock_guard lock(mutex_);
        limit = diskLimit_;
    }
    for (auto& r : records)
        total += r.size;
    for (auto& r : records) {
        if (total <= limit && unixTime() - r.accessed <= 7 * 24 * 3600)
            continue;
        std::error_code ec;
        fs::remove(dir_ / wide(r.key), ec);
        if (!ec) {
            total -= r.size;
            state_->removeDisk(r.key);
        }
    }
}
void RawCache::run() {
    try {
        trimDisk();
    } catch (...) {
        diskFailed_ = true;
    }
    for (;;) {
        Write w;
        bool clear = false;
        {
            std::unique_lock lock(mutex_);
            cv_.wait_for(lock, std::chrono::seconds(20),
                         [&] { return stopping_ || clear_ || !writes_.empty(); });
            if (stopping_)
                return;
            clear = std::exchange(clear_, false);
            if (!writes_.empty()) {
                w = std::move(writes_.front());
                writes_.pop_front();
                pending_ -= w.bytes->size();
            }
        }
        try {
            if (clear) {
                for (auto& r : state_->diskRecords()) {
                    std::error_code ec;
                    fs::remove(dir_ / wide(r.key), ec);
                    if (!ec)
                        state_->removeDisk(r.key);
                }
            }
            if (w.bytes) {
                writeFileAtomic(dir_ / wide(w.key), *w.bytes);
                state_->putDisk(w.key, w.bytes->size());
            }
            trimDisk();
        } catch (...) {
            diskFailed_ = true;
        }
    }
}
} // namespace viewer
