#include "phonostethos/FileIoManager.h"

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

FileIoManager::FileIoManager(
    const Options& options, CommandQueue& messages,
    CircularBuffer<int16_t, kCircularBufferSamples>& recording,
    CircularBuffer<int16_t, kCircularBufferSamples>& playback)
    : options_(options),
      messages_(messages),
      recording_(recording),
      playback_(playback) {
    initializeMutex(mailbox_mutex_);
    initializeCondition(mailbox_ready_);
}

FileIoManager::~FileIoManager() {
    stop();
    pthread_cond_destroy(&mailbox_ready_);
    pthread_mutex_destroy(&mailbox_mutex_);
}

void* FileIoManager::threadEntry(void* context) {
    static_cast<FileIoManager*>(context)->run();
    return nullptr;
}

bool FileIoManager::start() {
    if (worker_started_) return true;
    const int result = pthread_create(&worker_, nullptr, &FileIoManager::threadEntry, this);
    if (result != 0) {
        cerr << "Cannot start FileIoManager: " << strerror(result) << '\n';
        return false;
    }
    worker_started_ = true;
    return true;
}

// Publish one request under the mailbox mutex; file I/O happens after unlocking.
bool FileIoManager::request(Request request) {
    {
        PthreadLock lock(mailbox_mutex_);
        if (stopping_ || mailbox_.has_value()) return false;
        mailbox_ = move(request);
    }
    pthread_cond_signal(&mailbox_ready_);
    return true;
}

void FileIoManager::stop() {
    {
        PthreadLock lock(mailbox_mutex_);
        if (stopping_) return;
        stopping_ = true;
    }
    recording_.stop_producing();
    playback_.stop_producing();
    pthread_cond_broadcast(&mailbox_ready_);
    if (worker_started_) {
        pthread_join(worker_, nullptr);
        worker_started_ = false;
    }
}

void FileIoManager::set_file_thread_policy() {
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

void FileIoManager::post_result(WorkerResultCode code, uint32_t operation_id,
                                uint8_t error) {
    MainMessage message;
    message.kind = error == 0 ? MessageKind::WorkerResult
                              : MessageKind::WorkerError;
    message.operation_id = operation_id;
    message.result_code = code;
    message.error_code = error;
    messages_.enqueue(move(message));
}

bool FileIoManager::storage_available() const {
    return !storage_removed_ && storage_ready(options_);
}

void FileIoManager::post_storage_status(WorkerResultCode code,
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

// pop() releases its buffer mutex before the potentially slow disk write.
bool FileIoManager::write_recording() {
    auto* file = recording_file_;
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
    const auto written = writeChunk(samples.data(), static_cast<sf_count_t>(count));
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

// Read only what fits: FileIO produces playback; AudioDSP consumes it.
void FileIoManager::read_playback() {
    auto* file = playback_file_;
    const auto operation_id = playback_operation_;
    if (file == nullptr || !playback_active_) return;
    const auto available = playback_.available_capacity();
    if (available == 0) return;
    array<int16_t, kPeriodFrames> samples{};
    const auto requested = min(available, samples.size());
    const auto count = readChunk(samples.data(), static_cast<sf_count_t>(requested));
    if (count < 0 || sf_error(file) != SF_ERR_NO_ERROR) {
        cerr << "Playback WAV read failed: " << sf_strerror(file) << '\n';
        playback_.stop_producing();
        playback_active_ = false;
        post_result(WorkerResultCode::PlaybackFinished, operation_id,
                    static_cast<uint8_t>(
                        phono::protocol::Error::ReadError));
        return;
    }
    if (count == 0) {
        playback_.stop_producing();
        playback_active_ = false;
        return;
    }
    if (!playback_.push(samples.data(), static_cast<size_t>(count))) {
        cerr << "Playback buffer changed while FileIO was producing samples\n";
    }
}

// Called only by th_FileIO: no other thread touches these SNDFILE handles.
bool FileIoManager::openRecordFile(const string& filename) {
    using phono::protocol::Error;
    last_error_ = static_cast<uint8_t>(Error::StorageLost);
    if (!storage_available() || recording_file_ || playback_file_) return false;
    recording_fd_ = ::open(filename.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
    if (recording_fd_ < 0) {
        last_error_ = static_cast<uint8_t>(errno == ENOSPC ? Error::DiskFull : Error::OpenError);
        return false;
    }
    SF_INFO info{};
    info.samplerate = kSampleRate;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_16;
    recording_file_ = sf_open_fd(recording_fd_, SFM_WRITE, &info, SF_FALSE);
    if (!recording_file_) {
        const int open_error = errno;
        ::close(recording_fd_);
        recording_fd_ = -1;
        error_code ignored;
        filesystem::remove(filename, ignored);
        last_error_ = static_cast<uint8_t>(open_error == ENOSPC ? Error::DiskFull : Error::OpenError);
        return false;
    }
    recording_active_ = true;
    recording_error_.store(0);
    last_error_ = 0;
    return true;
}

bool FileIoManager::openPlaybackFile(const string& filename) {
    using phono::protocol::Error;
    last_error_ = static_cast<uint8_t>(Error::StorageLost);
    if (!storage_available() || recording_file_ || playback_file_) return false;
    playback_fd_ = ::open(filename.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (playback_fd_ < 0) {
        last_error_ = static_cast<uint8_t>(Error::ReadError);
        return false;
    }
    SF_INFO info{};
    playback_file_ = sf_open_fd(playback_fd_, SFM_READ, &info, SF_FALSE);
    last_error_ = static_cast<uint8_t>(Error::ReadError);
    if (!playback_file_) { closeFile(); return false; }
    if (info.channels != 1 || info.samplerate != kSampleRate ||
        (info.format & SF_FORMAT_TYPEMASK) != SF_FORMAT_WAV ||
        (info.format & SF_FORMAT_SUBMASK) != SF_FORMAT_PCM_16) {
        last_error_ = static_cast<uint8_t>(Error::UnsupportedWav);
        closeFile();
        return false;
    }
    last_error_ = 0;
    return true;
}

vector<string> FileIoManager::getFileList() {
    vector<string> names;
    last_error_ = static_cast<uint8_t>(phono::protocol::Error::StorageLost);
    if (!storage_available()) return names;
    error_code error;
    for (filesystem::directory_iterator it(options_.storage, error), end;
         !error && it != end; it.increment(error)) {
        if (it->path().extension() == ".wav") names.push_back(it->path().filename().string());
    }
    if (error) {
        last_error_ = static_cast<uint8_t>(phono::protocol::Error::ReadError);
        return {};
    }
    sort(names.begin(), names.end());
    last_error_ = 0;
    return names;
}

sf_count_t FileIoManager::writeChunk(const int16_t* data, sf_count_t count) {
    return recording_file_ ? sf_write_short(recording_file_, data, count) : 0;
}

sf_count_t FileIoManager::readChunk(int16_t* data, sf_count_t count) {
    return playback_file_ ? sf_read_short(playback_file_, data, count) : 0;
}

// The caller drains recording samples first; this method checks WAV finalization.
bool FileIoManager::closeFile() {
    using phono::protocol::Error;
    bool success = true;
    if (recording_file_) {
        sf_write_sync(recording_file_);
        if (sf_error(recording_file_) != SF_ERR_NO_ERROR) {
            recording_error_.store(static_cast<uint8_t>(Error::WriteError));
            success = false;
        }
        if (sf_close(recording_file_) != 0) {
            if (recording_error_.load() == 0) recording_error_.store(static_cast<uint8_t>(Error::FinalizeError));
            success = false;
        }
        recording_file_ = nullptr;
    }
    if (recording_fd_ >= 0) {
        if (::close(recording_fd_) != 0) success = false;
        recording_fd_ = -1;
    }
    if (playback_file_) {
        if (sf_close(playback_file_) != 0) success = false;
        playback_file_ = nullptr;
    }
    if (playback_fd_ >= 0) {
        if (::close(playback_fd_) != 0) success = false;
        playback_fd_ = -1;
    }
    return success;
}

bool FileIoManager::deleteRecordFile(const string& filename) {
    using phono::protocol::Error;
    last_error_ = static_cast<uint8_t>(Error::StorageLost);
    if (!storage_available() || recording_file_ || playback_file_) return false;
    // A basename is a name, never a path supplied by the GUI.
    if (filename.empty() || filename.find_first_of("/\\") != string::npos || filename == "." || filename == "..") {
        last_error_ = static_cast<uint8_t>(Error::InvalidArgument);
        return false;
    }
    error_code error;
    if (!filesystem::remove(options_.storage / filename, error) || error) {
        last_error_ = static_cast<uint8_t>(Error::WriteError);
        return false;
    }
    last_error_ = 0;
    return true;
}

bool FileIoManager::ejectStorage() {
    using phono::protocol::Error;
    last_error_ = static_cast<uint8_t>(Error::Busy);
    if (recording_file_ || playback_file_) return false;
    if (!storage_available()) {
        last_error_ = static_cast<uint8_t>(Error::StorageLost);
        return false;
    }
    last_error_ = static_cast<uint8_t>(Error::UnmountError);
    const int fd = ::open(options_.storage.c_str(), O_RDONLY | O_DIRECTORY);
    if (fd < 0) return false;
    const bool synced = ::syncfs(fd) == 0;
    const bool closed = ::close(fd) == 0;
    if (!synced || !closed || ::umount2(options_.storage.c_str(), 0) != 0) return false;
    last_error_ = 0;
    return true;
}

// Dispatch control independently of recording data: LIST/EJECT also work while idle.
void FileIoManager::process_request(const Request& request) {
    using phono::protocol::Error;
    switch (request.kind) {
        case RequestKind::OpenRecording: {
            const bool opened = openRecordFile(request.path.string());
            post_result(WorkerResultCode::RecordingOpened, request.operation_id,
                        opened ? 0 : last_error_);
            return;
        }
        case RequestKind::CloseRecording: {
            bool success = true;
            while (!recording_.isEmpty() && success) {
                success = write_recording();
            }
            if (!recording_.isEmpty()) recording_.clear();
            if (!closeFile()) success = false;
            recording_active_ = false;
            const auto final_error =
                recording_error_.load() != 0
                    ? recording_error_.load()
                    : (success ? 0
                               : static_cast<uint8_t>(Error::FinalizeError));
            post_result(WorkerResultCode::RecordingClosed,
                        request.operation_id, final_error);
            recording_operation_ = 0;
            return;
        }
        case RequestKind::PreparePlayback: {
            if (!storage_available() || recording_file_ != nullptr ||
                playback_file_ != nullptr) {
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
            if (!openPlaybackFile(path.string())) {
                post_result(WorkerResultCode::PlaybackReady, request.operation_id, last_error_);
                return;
            }
            struct stat opened_metadata {};
            if (::fstat(playback_fd_, &opened_metadata) != 0 || !same_file(request.identity, opened_metadata)) {
                closeFile();
                post_result(WorkerResultCode::PlaybackReady, request.operation_id,
                            static_cast<uint8_t>(Error::StaleFileId));
                return;
            }
            playback_.clear();
            if (!playback_.begin_session()) {
                sf_close(playback_file_);
                playback_file_ = nullptr;
                ::close(playback_fd_);
                playback_fd_ = -1;
                post_result(WorkerResultCode::PlaybackReady,
                            request.operation_id,
                            static_cast<uint8_t>(Error::Busy));
                return;
            }
            playback_active_ = true;
            playback_operation_ = request.operation_id;
            read_playback();
            post_result(WorkerResultCode::PlaybackReady,
                        request.operation_id);
            return;
        }
        case RequestKind::StopPlayback: {
            playback_.stop_producing();
            playback_.clear();
            const bool success = closeFile();
            playback_active_ = false;
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
            const auto filenames = getFileList();
            if (last_error_ != 0) {
                message.kind = MessageKind::WorkerError;
                message.error_code = last_error_;
            } else {
                for (const auto& filename : filenames) {
                    struct stat metadata {};
                    const auto path = options_.storage / filename;
                    if (::lstat(path.c_str(), &metadata) == 0 && S_ISREG(metadata.st_mode)) {
                        message.archive.push_back({filename, metadata.st_dev, metadata.st_ino,
                                                   metadata.st_size, metadata.st_mtim});
                    }
                }
            }
            messages_.enqueue(move(message));
            return;
        }
        case RequestKind::Delete: {
            struct stat metadata {};
            const auto path = options_.storage / request.identity.basename;
            if (::lstat(path.c_str(), &metadata) != 0 || !same_file(request.identity, metadata)) {
                post_result(WorkerResultCode::RecordingDeleted, request.operation_id,
                            static_cast<uint8_t>(Error::StaleFileId));
                return;
            }
            const bool deleted = deleteRecordFile(request.identity.basename);
            post_result(WorkerResultCode::RecordingDeleted, request.operation_id,
                        deleted ? 0 : last_error_);
            return;
        }

        case RequestKind::Eject: {
            const bool ejected = ejectStorage();
            post_result(WorkerResultCode::StorageEjected, request.operation_id,
                        ejected ? 0 : last_error_);
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
            const bool had_recording = recording_file_ != nullptr;
            uint8_t close_error = recording_error_.load();
            if (recording_file_ != nullptr) {
                while (!recording_.isEmpty() &&
                       write_recording()) {}
                if (!recording_.isEmpty()) recording_.clear();
                if (recording_error_.load() != 0) {
                    close_error = recording_error_.load();
                }
                sf_write_sync(recording_file_);
                if (sf_error(recording_file_) != SF_ERR_NO_ERROR &&
                    close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::WriteError);
                }
                if (sf_close(recording_file_) != 0 && close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::FinalizeError);
                }
                recording_file_ = nullptr;
            }
            if (recording_fd_ >= 0) {
                if (::close(recording_fd_) != 0 && close_error == 0) {
                    close_error = static_cast<uint8_t>(Error::FinalizeError);
                }
                recording_fd_ = -1;
            }
            recording_active_ = false;
            if (had_recording) {
                post_result(WorkerResultCode::RecordingClosed,
                            recording_operation_, close_error);
                recording_operation_ = 0;
            }
            playback_.stop_producing();
            playback_.clear();
            const bool had_playback = playback_file_ != nullptr;
            uint8_t playback_close_error = 0;
            if (playback_file_ != nullptr) {
                if (sf_close(playback_file_) != 0) {
                    playback_close_error =
                        static_cast<uint8_t>(Error::FinalizeError);
                }
                playback_file_ = nullptr;
            }
            if (playback_fd_ >= 0) {
                if (::close(playback_fd_) != 0 && playback_close_error == 0) {
                    playback_close_error =
                        static_cast<uint8_t>(Error::FinalizeError);
                }
                playback_fd_ = -1;
            }
            playback_active_ = false;
            if (had_playback) {
                post_result(WorkerResultCode::PlaybackClosed,
                            playback_operation_, playback_close_error);
            }
            playback_operation_ = 0;
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

// This pthread owns all WAV handles and filesystem operations.
void FileIoManager::run() {
    set_file_thread_policy();
    if (storage_available()) mounted_device_ = mounted_source(options_.storage);
    post_storage_status(WorkerResultCode::FileIOReady, 0);
    while (true) {
        optional<Request> request;
        {
            PthreadLock lock(mailbox_mutex_);
            const auto deadline = waitDeadline(10);
            while (!stopping_ && !mailbox_.has_value() &&
                   !recording_active_ && (!playback_active_ || playback_.isFull())) {
                const int result = pthread_cond_timedwait(
                    &mailbox_ready_, &mailbox_mutex_, &deadline);
                if (result == ETIMEDOUT) break;
                checkPthread(result);
            }
            if (mailbox_.has_value()) {
                request = move(mailbox_);
                mailbox_.reset();
            } else if (stopping_) {
                break;
            }
        }
        if (request.has_value()) {
            if (request->kind == RequestKind::OpenRecording) {
                recording_operation_ = request->operation_id;
            }
            process_request(*request);
        }

        if (recording_active_ && recording_file_ != nullptr) {
            if (!storage_available()) {
                recording_.stop_producing();
                recording_.clear();
                sf_close(recording_file_);
                recording_file_ = nullptr;
                if (recording_fd_ >= 0) ::close(recording_fd_);
                recording_fd_ = -1;
                recording_active_ = false;
                recording_error_.store(
                    static_cast<uint8_t>(phono::protocol::Error::StorageLost));
                post_result(WorkerResultCode::RecordingClosed,
                            recording_operation_,
                            static_cast<uint8_t>(
                                phono::protocol::Error::StorageLost));
            } else {
                if (!write_recording()) {
                    Request close_request;
                    close_request.kind = RequestKind::CloseRecording;
                    close_request.operation_id = recording_operation_;
                    process_request(close_request);
                }
            }
        }
        if (playback_active_) {
            read_playback();
        }
    }
    if (recording_file_ != nullptr) {
        while (!recording_.isEmpty() &&
               write_recording()) {}
        sf_write_sync(recording_file_);
        sf_close(recording_file_);
    }
    if (recording_fd_ >= 0) ::close(recording_fd_);
    if (playback_file_ != nullptr) sf_close(playback_file_);
    if (playback_fd_ >= 0) ::close(playback_fd_);
}
}  // namespace phono_app
