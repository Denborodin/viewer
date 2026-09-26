#pragma once
#include "state.h"
namespace viewer {
class RawCache {
  public:
    RawCache(std::shared_ptr<StateStore> state, Settings settings, std::shared_ptr<Metrics> metrics);
    ~RawCache();
    BytePtr get(const std::string& source, uint32_t id);
    void put(const std::string& source, uint32_t id, BytePtr bytes);
    void configure(Settings settings);
    void clear();
    uint64_t memoryUsage() const;
    bool diskFailed() const {
        return diskFailed_;
    }
    static std::string key(const std::string& source, uint32_t id);

  private:
    struct Item {
        BytePtr bytes;
        uint64_t stamp;
    };
    struct Write {
        std::string key;
        BytePtr bytes;
    };
    std::shared_ptr<StateStore> state_;
    std::shared_ptr<Metrics> metrics_;
    fs::path dir_;
    mutable std::mutex mutex_;
    std::condition_variable cv_;
    std::unordered_map<std::string, Item> memory_;
    std::deque<Write> writes_;
    uint64_t used_ = 0, pending_ = 0, tick_ = 0, ramLimit_ = 0, diskLimit_ = 0;
    bool stopping_ = false, clear_ = false;
    std::atomic<bool> diskFailed_ = false;
    std::thread io_;
    void remember(const std::string& key, BytePtr bytes);
    void run();
    void trimDisk();
};
} // namespace viewer
