#include "phonostethos/FileIOManager.h"

#include "phonostethos/protocol.h"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <fstream>
#include <iostream>
#include <pthread.h>
#include <sched.h>
#include <sstream>
#include <sys/mount.h>
#include <sys/stat.h>
#include <unistd.h>
#include <utility>

using namespace std;

namespace phono_app {
namespace {
string unescape_mount_path(string path) {
    const array<pair<const char*, char>, 4> escapes{{
        {"\\040", ' '},
        {"\\011", '\t'},
        {"\\012", '\n'},
        {"\\134", '\\'},
    }};
    for (const auto& escape : escapes) {
        size_t position = 0;
        while ((position = path.find(escape.first, position)) != string::npos) {
            path.replace(position, 4, 1, escape.second);
            ++position;
        }
    }
    return path;
}

string mounted_source(const filesystem::path& path) {
    error_code error;
    const auto canonical_path = filesystem::canonical(path, error);
    if (error) return {};
    ifstream mounts("/proc/self/mountinfo");
    if (!mounts) return {};
    string line;
    while (getline(mounts, line)) {
        istringstream fields(line);
        string field;
        bool valid_line = true;
        for (int index = 0; index < 5; ++index) {
            if (!(fields >> field)) {
                valid_line = false;
                break;
            }
        }
        if (!valid_line ||
            unescape_mount_path(field) != canonical_path.string()) {
            continue;
        }
        while (fields >> field && field != "-") {}
        string filesystem_type;
        string source;
        if (fields >> filesystem_type >> source) return source;
    }
    return {};
}
}  // namespace

bool storage_ready(const Options& options) {
    error_code error;
    if (!filesystem::is_directory(options.storage, error) || error) return false;
    if (!options.require_mounted_storage) return true;

    const auto storage_path = filesystem::canonical(options.storage, error);
    if (error) return false;
    ifstream mounts("/proc/self/mountinfo");
    if (!mounts) return false;

    string line;
    while (getline(mounts, line)) {
        istringstream fields(line);
        string field;
        bool valid_line = true;
        for (int index = 0; index < 5; ++index) {
            if (!(fields >> field)) {
                valid_line = false;
                break;
            }
        }
        if (valid_line &&
            unescape_mount_path(field) == storage_path.string()) {
            return true;
        }
    }
    return false;
}

FileIOManager::FileIOManager(
    const Options& options, CommandQueue& messages,
    CircularBuffer<int16_t, kCircularBufferSamples>& recording,
    CircularBuffer<int16_t, kCircularBufferSamples>& playback)
    : options_(options),
      messages_(messages),
      recording_(recording),
      playback_(playback),
      worker_(&FileIOManager::run, this) {}

FileIOManager::~FileIOManager() { stop(); }

bool FileIOManager::request(Request request) {
    {
        lock_guard<mutex> lock(mailbox_mutex_);
        if (stopping_ || mailbox_.has_value()) return false;
        mailbox_ = move(request);
    }
    mailbox_ready_.notify_one();
    return true;
}

void FileIOManager::stop() {
    {
        lock_guard<mutex> lock(mailbox_mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    recording_.stop_producing();
    playback_.stop_producing();
    mailbox_ready_.notify_all();
    if (worker_.joinable()) worker_.join();
}

void FileIOManager::set_file_thread_policy() {
    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(2, &cpus);
    const int affinity_result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (affinity_result != 0) {
        cerr << "FileIO thread CPU affinity not set: "
                  << strerror(affinity_result) << '\n';
    }
    sched_param priority{};
    priority.sched_priority = 50;
    const int scheduling_result =
        pthread_setschedparam(pthread_self(), SCHED_FIFO, &priority);
    if (scheduling_result != 0) {
        cerr << "FileIO thread SCHED_FIFO unavailable: "
                  << strerror(scheduling_result) << '\n';
    }
}

void FileIOManager::post_result(WorkerResultCode code, uint32_t operation_id,
                                uint8_t error) {
    MainMessage message;
    message.kind = error == 0 ? MessageKind::WorkerResult
                              : MessageKind::WorkerError;
    message.operation_id = operation_id;
    message.result_code = code;
    message.error_code = error;
    messages_.enqueue(move(message));
}

bool FileIOManager::storage_available() const {
    return !storage_removed_ && storage_ready(options_);
}

void FileIOManager::post_storage_status(WorkerResultCode code,
                                        uint32_t operation_id) {
    MainMessage message;
    message.kind = MessageKind::WorkerResult;
    message.operation_id = operation_id;
    message.result_code = code;
    error_code error;
    if (!mounted_device_.empty() && !storage_removed_) {
        message.storage_flags |= phono::protocol::StoragePresent;
    }
    if (storage_available()) {
        message.storage_flags |= phono::protocol::StorageMounted;
        if (::access(options_.storage.c_str(), R_OK) == 0) {
            message.storage_flags |= phono::protocol::StorageReadable;
        }
        if (::access(options_.storage.c_str(), W_OK) == 0) {
            message.storage_flags |= phono::protocol::StorageWritable;
        }
        error.clear();
        const auto space = filesystem::space(options_.storage, error);
        if (!error && space.available >= 1024 * 1024) {
            message.storage_flags |= phono::protocol::StorageSpaceOk;
        }
    }
    messages_.enqueue(move(message));
}

bool FileIOManager::write_recording(SNDFILE* file) {
    if (file == nullptr) return false;
    if (!storage_available()) {
        recording_error_.store(
            static_cast<uint8_t>(phono::protocol::Error::StorageLost));
        recording_.stop_producing();
        return false;
    }
    array<int16_t, kPeriodFrames> samples{};
    const auto count = recording_.pop(samples.data(), samples.size());
    if (count == 0) return true;
    const auto written = sf_write_short(file, samples.data(),
                                        static_cast<sf_count_t>(count));
    if (written != static_cast<sf_count_t>(count)) {
        const auto error = errno == ENOSPC
                               ? phono::protocol::Error::DiskFull
                               : phono::protocol::Error::WriteError;
        cerr << "Audio recording write failed: " << sf_strerror(file) << '\n';
        recording_error_.store(static_cast<uint8_t>(error));
        recording_.stop_producing();
        return false;
    }
    return true;
}

void FileIOManager::read_playback(SNDFILE* file, bool& playback_active,
                                  uint32_t operation_id) {
    if (file == nullptr || !playback_active) return;
    const auto available = playback_.available_capacity();
    if (available == 0) return;
    array<int16_t, kPeriodFrames> samples{};
    const auto requested = min(available, samples.size());
    const auto count = sf_read_short(file, samples.data(),
                                     static_cast<sf_count_t>(requested));
    if (count < 0 || sf_error(file) != SF_ERR_NO_ERROR) {
        cerr << "Playback WAV read failed: " << sf_strerror(file) << '\n';
        playback_.stop_producing();
        playback_active = false;
        post_result(WorkerResultCode::PlaybackFinished, operation_id,
                    static_cast<uint8_t>(
                        phono::protocol::Error::ReadError));
        return;
    }
    if (count == 0) {
        playback_.stop_producing();
        playback_active = false;
        return;
    }
    if (!playback_.push(samples.data(), static_cast<size_t>(count))) {
        cerr << "Playback buffer changed while FileIO was producing samples\n";
    }
}

void FileIOManager::process_request(
    const Request& request, SNDFILE*& recording_file, int& recording_fd,
    SNDFILE*& playback_file, int& playback_fd, bool& recording_active,
    bool& playback_active, uint32_t& recording_operation,
    uint32_t& playback_operation) {
    using phono::protocol::Error;
    switch (request.kind) {
        case RequestKind::OpenRecording: {
            if (!storage_available() || recording_file != nullptr ||
                playback_file != nullptr) {
                post_result(WorkerResultCode::RecordingOpened,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StorageLost));
                return;
            }
            const int descriptor =
                ::open(request.path.c_str(),
                       O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
            if (descriptor < 0) {
                const auto error = errno == ENOSPC ? Error::DiskFull
                                                    : Error::OpenError;
                post_result(WorkerResultCode::RecordingOpened,
                            request.operation_id,
                            static_cast<uint8_t>(error));
                return;
            }
            SF_INFO info{};
            info.samplerate = kSampleRate;
            info.channels = 1;
            info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
            recording_file = sf_open_fd(descriptor, SFM_WRITE, &info, SF_FALSE);
            if (recording_file == nullptr) {
                const int open_error = errno;
                ::close(descriptor);
                error_code remove_error;
                filesystem::remove(request.path, remove_error);
                post_result(
                    WorkerResultCode::RecordingOpened, request.operation_id,
                    static_cast<uint8_t>(open_error == ENOSPC
                                             ? Error::DiskFull
                                             : Error::OpenError));
                return;
            }
            recording_fd = descriptor;
            recording_active = true;
            recording_error_.store(0);
            post_result(WorkerResultCode::RecordingOpened,
                        request.operation_id);
            return;
        }
        case RequestKind::CloseRecording: {
            bool success = true;
            while (!recording_.isEmpty() && success) {
                success = write_recording(recording_file);
            }
            if (!recording_.isEmpty()) recording_.clear();
            if (recording_file != nullptr) {
                sf_write_sync(recording_file);
                if (sf_error(recording_file) != SF_ERR_NO_ERROR) {
                    success = false;
                    if (recording_error_.load() == 0) {
                        recording_error_.store(
                            static_cast<uint8_t>(Error::WriteError));
                    }
                }
                if (sf_close(recording_file) != 0) {
                    success = false;
                    if (recording_error_.load() == 0) {
                        recording_error_.store(
                            static_cast<uint8_t>(Error::FinalizeError));
                    }
                    cerr << "Cannot finalize recording: "
                              << sf_strerror(nullptr) << '\n';
                }
                recording_file = nullptr;
            }
            if (recording_fd >= 0) {
                if (::close(recording_fd) != 0) success = false;
                recording_fd = -1;
            }
            recording_active = false;
            const auto final_error =
                recording_error_.load() != 0
                    ? recording_error_.load()
                    : (success ? 0
                               : static_cast<uint8_t>(Error::FinalizeError));
            post_result(WorkerResultCode::RecordingClosed,
                        request.operation_id, final_error);
            recording_operation = 0;
            return;
        }
        case RequestKind::PreparePlayback: {
            if (!storage_available() || recording_file != nullptr ||
                playback_file != nullptr) {
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StorageLost));
                return;
            }
            const auto path = options_.storage / request.identity.basename;
            struct stat metadata {};
            if (::lstat(path.c_str(), &metadata) != 0 ||
                !same_file(request.identity, metadata)) {
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StaleFileId));
                return;
            }
            const int descriptor =
                ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat opened_metadata {};
            if (descriptor < 0 || ::fstat(descriptor, &opened_metadata) != 0 ||
                !same_file(request.identity, opened_metadata)) {
                if (descriptor >= 0) ::close(descriptor);
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StaleFileId));
                return;
            }
            SF_INFO info{};
            playback_file = sf_open_fd(descriptor, SFM_READ, &info, SF_FALSE);
            if (playback_file == nullptr) {
                ::close(descriptor);
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::ReadError));
                return;
            }
            if (info.channels != 1 || info.samplerate != kSampleRate ||
                (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ||
                (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
                sf_close(playback_file);
                playback_file = nullptr;
                ::close(descriptor);
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::UnsupportedWav));
                return;
            }
            playback_fd = descriptor;
            playback_.clear();
            if (!playback_.begin_session()) {
                sf_close(playback_file);
                playback_file = nullptr;
                ::close(playback_fd);
                playback_fd = -1;
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::Busy));
                return;
            }
            playback_active = true;
            playback_operation = request.operation_id;
            read_playback(playback_file, playback_active, playback_operation);
            post_result(WorkerResultCode::PlaybackReady,
                        request.operation_id);
            return;
        }
        case RequestKind::StopPlayback: {
            playback_.stop_producing();
            playback_.clear();
            bool success = true;
            if (playback_file != nullptr) {
                if (sf_close(playback_file) != 0) success = false;
                playback_file = nullptr;
            }
            if (playback_fd >= 0) {
                if (::close(playback_fd) != 0) success = false;
                playback_fd = -1;
            }
            playback_active = false;
            post_result(WorkerResultCode::PlaybackClosed,
                        request.operation_id,
                        success ? 0
                                : static_cast<uint8_t>(Error::FinalizeError));
            return;
        }
        case RequestKind::List: {
            MainMessage message;
            message.kind = MessageKind::WorkerResult;
            message.operation_id = request.operation_id;
            message.result_code = WorkerResultCode::ArchiveReady;
            error_code error;
            if (!storage_available()) {
                message.kind = MessageKind::WorkerError;
                message.error_code = static_cast<uint8_t>(Error::StorageLost);
                messages_.enqueue(move(message));
                return;
            }
            for (filesystem::directory_iterator it(options_.storage, error), end;
                 !error && it != end; it.increment(error)) {
                if (it->path().extension() != ".wav") continue;
                struct stat file_metadata {};
                if (::lstat(it->path().c_str(), &file_metadata) != 0 ||
                    !S_ISREG(file_metadata.st_mode)) {
                    continue;
                }
                message.archive.push_back(
                    ArchiveIdentity{it->path().filename().string(),
                                    file_metadata.st_dev, file_metadata.st_ino,
                                    file_metadata.st_size,
                                    file_metadata.st_mtim});
            }
            if (error) {
                message.kind = MessageKind::WorkerError;
                message.error_code = static_cast<uint8_t>(Error::ReadError);
            } else {
                sort(message.archive.begin(), message.archive.end(),
                     [](const auto& left, const auto& right) {
                         return left.basename < right.basename;
                     });
            }
            messages_.enqueue(move(message));
            return;
        }
        case RequestKind::Delete: {
            const auto path = options_.storage / request.identity.basename;
            struct stat metadata {};
            if (!storage_available() ||
                ::lstat(path.c_str(), &metadata) != 0 ||
                !same_file(request.identity, metadata)) {
                post_result(WorkerResultCode::RecordingDeleted,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StaleFileId));
                return;
            }
            error_code error;
            if (!filesystem::remove(path, error) || error) {
                post_result(WorkerResultCode::RecordingDeleted,
                            request.operation_id,
                            static_cast<uint8_t>(Error::WriteError));
                return;
            }
            post_result(WorkerResultCode::RecordingDeleted,
                        request.operation_id);
            return;
        }
        case RequestKind::Eject: {
            if (recording_file != nullptr || playback_file != nullptr) {
                post_result(WorkerResultCode::StorageEjected,
                            request.operation_id,
                            static_cast<uint8_t>(Error::Busy));
                return;
            }
            if (!storage_available()) {
                post_result(WorkerResultCode::StorageEjected,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StorageLost));
                return;
            }
            const int storage_fd =
                ::open(options_.storage.c_str(), O_RDONLY | O_DIRECTORY);
            if (storage_fd < 0 || ::syncfs(storage_fd) != 0) {
                if (storage_fd >= 0) ::close(storage_fd);
                post_result(WorkerResultCode::StorageEjected,
                            request.operation_id,
                            static_cast<uint8_t>(Error::UnmountError));
                return;
            }
            const bool closed = ::close(storage_fd) == 0;
            const bool unmounted =
                closed && ::umount2(options_.storage.c_str(), 0) == 0;
            post_result(WorkerResultCode::StorageEjected,
                        request.operation_id,
                        unmounted ? 0
                                  : static_cast<uint8_t>(Error::UnmountError));
            return;
        }
        case RequestKind::Mount:
            if (!options_.require_mounted_storage) {
                error_code mount_error;
                if (filesystem::is_directory(options_.storage, mount_error) &&
                    !mount_error) {
                    mounted_device_ = request.path.string();
                    storage_removed_ = false;
                    post_storage_status(WorkerResultCode::StorageChanged,
                                        request.operation_id);
                    return;
                }
            }
            if (storage_available()) {
                if (mounted_device_.empty()) {
                    mounted_device_ = mounted_source(options_.storage);
                    if (mounted_device_.empty()) {
                        mounted_device_ = request.path.string();
                    }
                }
                post_storage_status(WorkerResultCode::StorageChanged,
                                    request.operation_id);
                return;
            }
            if (storage_removed_ && ::umount2(options_.storage.c_str(), 0) != 0 &&
                errno != EINVAL && errno != ENOENT) {
                cerr << "Cannot release removed storage mount: "
                          << strerror(errno) << '\n';
                post_result(WorkerResultCode::StorageChanged,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StorageLost));
                return;
            }
            if (request.path.empty() || request.filesystem_type.empty()) {
                post_result(WorkerResultCode::StorageChanged,
                            request.operation_id,
                            static_cast<uint8_t>(Error::InvalidArgument));
                return;
            }
            if (::mount(request.path.c_str(), options_.storage.c_str(),
                        request.filesystem_type.c_str(),
                        MS_NODEV | MS_NOSUID | MS_NOEXEC, nullptr) != 0) {
                cerr << "Cannot mount " << request.path << " on "
                          << options_.storage << ": " << strerror(errno)
                          << '\n';
                post_result(WorkerResultCode::StorageChanged,
                            request.operation_id,
                            static_cast<uint8_t>(Error::StorageLost));
                return;
            }
            mounted_device_ = request.path.string();
            storage_removed_ = false;
            post_storage_status(WorkerResultCode::StorageChanged,
                                request.operation_id);
            return;
        case RequestKind::DeviceRemoved: {
            if (request.path.string() != mounted_device_) {
                post_storage_status(WorkerResultCode::StorageChanged,
                                    request.operation_id);
                return;
            }
            storage_removed_ = true;
            recording_.stop_producing();
            const bool had_recording = recording_file != nullptr;
            uint8_t close_error = recording_error_.load();
            if (recording_file != nullptr) {
                while (!recording_.isEmpty() &&
                       write_recording(recording_file)) {}
                if (!recording_.isEmpty()) recording_.clear();
                if (recording_error_.load() != 0) {
                    close_error = recording_error_.load();
                }
                sf_write_sync(recording_file);
                if (sf_error(recording_file) != SF_ERR_NO_ERROR &&
                    close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::WriteError);
                }
                if (sf_close(recording_file) != 0 && close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::FinalizeError);
                }
                recording_file = nullptr;
            }
            if (recording_fd >= 0) {
                if (::close(recording_fd) != 0 && close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::FinalizeError);
                }
                recording_fd = -1;
            }
            recording_active = false;
            if (had_recording) {
                post_result(WorkerResultCode::RecordingClosed,
                            recording_operation, close_error);
                recording_operation = 0;
            }
            playback_.stop_producing();
            playback_.clear();
            const bool had_playback = playback_file != nullptr;
            uint8_t playback_close_error = 0;
            if (playback_file != nullptr) {
                if (sf_close(playback_file) != 0) {
                    playback_close_error =
                        static_cast<uint8_t>(Error::FinalizeError);
                }
                playback_file = nullptr;
            }
            if (playback_fd >= 0) {
                if (::close(playback_fd) != 0 && playback_close_error == 0) {
                    playback_close_error =
                        static_cast<uint8_t>(Error::FinalizeError);
                }
                playback_fd = -1;
            }
            playback_active = false;
            if (had_playback) {
                post_result(WorkerResultCode::PlaybackClosed,
                            playback_operation, playback_close_error);
            }
            playback_operation = 0;
            mounted_device_.clear();
            if (::umount2(options_.storage.c_str(), 0) != 0 && errno != EINVAL &&
                errno != ENOENT) {
                cerr << "Cannot unmount removed storage: " << strerror(errno)
                          << '\n';
            }
            {
                MainMessage message;
                message.kind = MessageKind::WorkerResult;
                message.operation_id = request.operation_id;
                message.result_code = WorkerResultCode::StorageChanged;
                messages_.enqueue(move(message));
            }
            return;
        }
        case RequestKind::RefreshStorage:
            post_storage_status(WorkerResultCode::StorageChanged,
                                request.operation_id);
            return;
    }
}

void FileIOManager::run() {
    set_file_thread_policy();
    SNDFILE* recording_file = nullptr;
    SNDFILE* playback_file = nullptr;
    int recording_fd = -1;
    int playback_fd = -1;
    bool recording_active = false;
    bool playback_active = false;
    uint32_t recording_operation = 0;
    uint32_t playback_operation = 0;
    if (storage_available()) mounted_device_ = mounted_source(options_.storage);
    post_storage_status(WorkerResultCode::FileIOReady, 0);
    while (true) {
        optional<Request> request;
        {
            unique_lock<mutex> lock(mailbox_mutex_);
            mailbox_ready_.wait_for(lock, chrono::milliseconds(10), [&] {
                return stopping_ || mailbox_.has_value();
            });
            if (mailbox_.has_value()) {
                request = move(mailbox_);
                mailbox_.reset();
            } else if (stopping_) {
                break;
            }
        }
        if (request.has_value()) {
            if (request->kind == RequestKind::OpenRecording) {
                recording_operation = request->operation_id;
            }
            process_request(*request, recording_file, recording_fd,
                            playback_file, playback_fd, recording_active,
                            playback_active, recording_operation,
                            playback_operation);
        }

        if (recording_active && recording_file != nullptr) {
            if (!storage_available()) {
                recording_.stop_producing();
                recording_.clear();
                sf_close(recording_file);
                recording_file = nullptr;
                if (recording_fd >= 0) ::close(recording_fd);
                recording_fd = -1;
                recording_active = false;
                recording_error_.store(
                    static_cast<uint8_t>(phono::protocol::Error::StorageLost));
                post_result(WorkerResultCode::RecordingClosed,
                            recording_operation,
                            static_cast<uint8_t>(
                                phono::protocol::Error::StorageLost));
            } else {
                (void)write_recording(recording_file);
            }
        }
        if (playback_active) {
            read_playback(playback_file, playback_active, playback_operation);
        }
    }
    if (recording_file != nullptr) {
        while (!recording_.isEmpty() &&
               write_recording(recording_file)) {}
        sf_write_sync(recording_file);
        sf_close(recording_file);
    }
    if (recording_fd >= 0) ::close(recording_fd);
    if (playback_file != nullptr) sf_close(playback_file);
    if (playback_fd >= 0) ::close(playback_fd);
}
}  // namespace phono_app
