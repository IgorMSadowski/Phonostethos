#pragma once

#include "phonostethos/RuntimeTypes.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

namespace phono_app {
struct FileWork {
    enum class Kind { Start, Samples, Stop, Barrier };
    Kind kind = Kind::Samples;
    std::filesystem::path path;
    std::array<std::int16_t, kPeriodFrames> samples{};
    int descriptor = -1;
    std::shared_ptr<std::promise<void>> barrier;
    std::shared_ptr<std::promise<bool>> completion;
};

class FileIOManager {
public:
    FileIOManager(std::atomic<unsigned>& error_code,
                    std::atomic<bool>& recording,
                    std::atomic<std::uint32_t>& recording_overruns,
                    const Options& options);

    ~FileIOManager();

    bool start(const std::filesystem::path& path, int descriptor);

    bool push(const std::array<std::int16_t, kPeriodFrames>& samples);

    bool finish();

    bool finish_and_wait();

    void stop();

private:
    void run();

    std::atomic<unsigned>& error_code_;
    std::atomic<bool>& recording_state_;
    std::atomic<std::uint32_t>& recording_overruns_;
    const Options& options_;
    mutable std::mutex mutex_;
    std::condition_variable ready_;
    std::deque<FileWork> queue_;
    bool recording_ = false;
    bool stopping_ = false;
    std::thread worker_;
};
}  // namespace phono_app
