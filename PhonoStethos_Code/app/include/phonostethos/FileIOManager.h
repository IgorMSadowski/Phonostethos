#pragma once

#include "phonostethos/RuntimeTypes.h"

#include <condition_variable>
#include <deque>
#include <future>
#include <memory>
#include <mutex>
#include <thread>

using namespace std;

namespace phono_app {
struct FileWork {
    enum class Kind { Start, Samples, Stop, Barrier };
    Kind kind = Kind::Samples;
    filesystem::path path;
    array<int16_t, kPeriodFrames> samples{};
    int descriptor = -1;
    shared_ptr<promise<void>> barrier;
    shared_ptr<promise<bool>> completion;
};

class FileIOManager {
public:
    FileIOManager(atomic<unsigned>& error_code,
                    atomic<bool>& recording,
                    atomic<uint32_t>& recording_overruns,
                    const Options& options);

    ~FileIOManager();

    bool start(const filesystem::path& path, int descriptor);

    bool push(const array<int16_t, kPeriodFrames>& samples);

    bool finish();

    bool finish_and_wait();

    void stop();

private:
    void run();

    atomic<unsigned>& error_code_;
    atomic<bool>& recording_state_;
    atomic<uint32_t>& recording_overruns_;
    const Options& options_;
    mutable mutex mutex_;
    condition_variable ready_;
    deque<FileWork> queue_;
    bool recording_ = false;
    bool stopping_ = false;
    thread worker_;
};
}  // namespace phono_app
