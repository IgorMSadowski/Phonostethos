#include "phonostethos/FileIOManager.h"

#include "phonostethos/protocol.h"

#include <sndfile.h>

#include <cerrno>
#include <iostream>
#include <system_error>
#include <unistd.h>

namespace phono_app {
FileIOManager::FileIOManager(std::atomic<unsigned>& error_code,
                            std::atomic<bool>& recording,
                            std::atomic<std::uint32_t>& recording_overruns,
                            const Options& options)
    : error_code_(error_code),
      recording_state_(recording),
      recording_overruns_(recording_overruns),
      options_(options),
      worker_(&FileIOManager::run, this) {}

FileIOManager::~FileIOManager() { stop(); }

bool FileIOManager::start(const std::filesystem::path& path, int descriptor) {
    auto completion = std::make_shared<std::promise<bool>>();
    auto completed = completion->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (recording_ || queue_.size() >= kMaxPendingAudioBlocks) {
            return false;
        }
        FileWork item;
        item.kind = FileWork::Kind::Start;
        item.path = path;
        item.descriptor = descriptor;
        item.completion = std::move(completion);
        queue_.push_back(std::move(item));
    }
    ready_.notify_one();
    return completed.get();
}

bool FileIOManager::push(const std::array<std::int16_t, kPeriodFrames>& samples) {
    std::lock_guard<std::mutex> lock(mutex_);
    if (!recording_) return false;
    if (queue_.size() >= kMaxPendingAudioBlocks) {
        recording_ = false;
        recording_overruns_.fetch_add(1);
        FileWork stop;
        stop.kind = FileWork::Kind::Stop;
        queue_.push_back(std::move(stop));
        error_code_.store(
            static_cast<unsigned>(phono::protocol::Error::RecordOverrun));
        ready_.notify_one();
        return false;
    }
    FileWork item;
    item.kind = FileWork::Kind::Samples;
    item.samples = samples;
    queue_.push_back(std::move(item));
    ready_.notify_one();
    return true;
}

bool FileIOManager::finish() {
    auto completion = std::make_shared<std::promise<bool>>();
    auto completed = completion->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!recording_) return true;
        recording_ = false;
        FileWork item;
        item.kind = FileWork::Kind::Stop;
        item.completion = std::move(completion);
        queue_.push_back(std::move(item));
    }
    ready_.notify_one();
    return completed.get();
}

bool FileIOManager::finish_and_wait() {
    const bool finished = finish();
    auto barrier = std::make_shared<std::promise<void>>();
    auto completed = barrier->get_future();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        FileWork item;
        item.kind = FileWork::Kind::Barrier;
        item.barrier = std::move(barrier);
        queue_.push_back(std::move(item));
    }
    ready_.notify_one();
    completed.wait();
    return finished;
}

void FileIOManager::stop() {
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (stopping_) return;
        stopping_ = true;
        if (recording_) {
            recording_ = false;
            FileWork item;
            item.kind = FileWork::Kind::Stop;
            queue_.push_back(std::move(item));
        }
    }
    ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FileIOManager::run() {
    SNDFILE* file = nullptr;
    int file_descriptor = -1;
    while (true) {
        FileWork item;
        {
            std::unique_lock<std::mutex> lock(mutex_);
            ready_.wait(lock, [&] { return stopping_ || !queue_.empty(); });
            if (queue_.empty() && stopping_) break;
            item = std::move(queue_.front());
            queue_.pop_front();
        }

        if (item.kind == FileWork::Kind::Start) {
            if (file != nullptr) {
                (void)sf_close(file);
                if (file_descriptor >= 0) ::close(file_descriptor);
                file = nullptr;
                file_descriptor = -1;
            }
            SF_INFO info{};
            info.samplerate = kSampleRate;
            info.channels = 1;
            info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
            file = sf_open_fd(item.descriptor, SFM_WRITE, &info, SF_FALSE);
            if (file == nullptr) {
                const int open_error = errno;
                std::cerr << "Cannot open recording " << item.path << ": "
                          << sf_strerror(nullptr) << '\n';
                ::close(item.descriptor);
                std::error_code remove_error;
                std::filesystem::remove(item.path, remove_error);
                error_code_.store(static_cast<unsigned>(
                    open_error == ENOSPC
                        ? phono::protocol::Error::DiskFull
                        : phono::protocol::Error::OpenError));
                std::lock_guard<std::mutex> lock(mutex_);
                recording_ = false;
                recording_state_.store(false);
                item.completion->set_value(false);
            } else {
                file_descriptor = item.descriptor;
                error_code_.store(0);
                {
                    std::lock_guard<std::mutex> lock(mutex_);
                    recording_ = true;
                    recording_state_.store(true);
                }
                item.completion->set_value(true);
            }
        } else if (item.kind == FileWork::Kind::Samples) {
            if (file != nullptr && !storage_ready(options_)) {
                error_code_.store(
                    static_cast<unsigned>(phono::protocol::Error::StorageLost));
                (void)sf_close(file);
                ::close(file_descriptor);
                file = nullptr;
                file_descriptor = -1;
                std::lock_guard<std::mutex> lock(mutex_);
                recording_ = false;
                recording_state_.store(false);
            } else if (file != nullptr &&
                sf_write_short(file, item.samples.data(), item.samples.size()) !=
                    static_cast<sf_count_t>(item.samples.size())) {
                const int write_error = errno;
                std::cerr << "Audio recording write failed: "
                          << sf_strerror(file) << '\n';
                error_code_.store(static_cast<unsigned>(
                    write_error == ENOSPC
                        ? phono::protocol::Error::DiskFull
                        : phono::protocol::Error::WriteError));
                (void)sf_close(file);
                ::close(file_descriptor);
                file = nullptr;
                file_descriptor = -1;
                std::lock_guard<std::mutex> lock(mutex_);
                recording_ = false;
                recording_state_.store(false);
            }
        } else if (item.kind == FileWork::Kind::Stop) {
            bool success = true;
            if (file != nullptr) {
                sf_write_sync(file);
                if (sf_close(file) != 0) {
                    error_code_.store(static_cast<unsigned>(
                        errno == ENOSPC
                            ? phono::protocol::Error::DiskFull
                            : phono::protocol::Error::FinalizeError));
                    success = false;
                }
                if (file_descriptor >= 0 && ::close(file_descriptor) != 0) {
                    error_code_.store(static_cast<unsigned>(
                        phono::protocol::Error::FinalizeError));
                    success = false;
                }
                file = nullptr;
                file_descriptor = -1;
            }
            recording_state_.store(false);
            if (item.completion != nullptr) {
                item.completion->set_value(success);
            }
        } else if (item.barrier != nullptr) {
            item.barrier->set_value();
        }
    }
    if (file != nullptr) {
        sf_write_sync(file);
        (void)sf_close(file);
        if (file_descriptor >= 0) ::close(file_descriptor);
    }
}
}  // namespace phono_app
