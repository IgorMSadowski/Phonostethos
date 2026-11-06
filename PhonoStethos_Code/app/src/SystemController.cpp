#include "phonostethos/SystemController.h"

#include "phonostethos/protocol.h"

#include <libudev.h>

#include <algorithm>
#include <cerrno>
#include <cstdio>
#include <ctime>
#include <iostream>
#include <poll.h>
#include <pthread.h>
#include <sched.h>
#include <sys/resource.h>
#include <cstring>

using namespace std;

namespace phono_app {
SystemController::SystemController(const Options& options)
    : options_(options),
      led_driver_(options.led_device),
      writer_(options, command_queue_, recording_buffer_, playback_buffer_),
      network_(options, command_queue_, data_queue_) {
    initializeMutex(configuration_mutex_);
}

SystemController::~SystemController() {
    stop();
    pthread_mutex_destroy(&configuration_mutex_);
}

// Prepare shared resources first; only then create the three worker pthreads.
bool SystemController::initSystem() {
    if (initialized_) return true;
    set_main_thread_policy();
    if (!initUdevMonitor()) return false;
    if (!led_driver_.openDriver()) {
        led_error_.store(true);
        cerr << "LED status outputs are unavailable; continuing without LEDs\n";
    }
    update_leds();
    // FFTW planning belongs to startup: no worker may execute an unfinished plan.
    if (!dsp_processor_.initialize()) {
        cerr << "Cannot prepare the FFTW plan\n";
        set_error(static_cast<uint8_t>(phono::protocol::Error::DspError));
        return false;
    }
    audio_ = make_unique<AudioEngine>(
        options_, dsp_processor_, mode_, gain_, recording_, error_code_, command_queue_,
        data_queue_, configuration_mutex_, recording_buffer_, playback_buffer_,
        dropped_telemetry_, recording_overruns_);
    if (!audio_->start() || !writer_.start()) return false;

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (!audio_init_complete_.load() &&
           chrono::steady_clock::now() < deadline) {
        MainMessage message;
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) {
            processMessage(message);
        }
    }
    if (!audio_ready_.load()) {
        cerr << "Audio initialization did not complete before timeout\n";
        set_error(static_cast<uint8_t>(phono::protocol::Error::MicLost));
    }
    network_.start();
    initialized_ = true;
    next_storage_check_ = chrono::steady_clock::now();
    return true;
}

// Main coordinates operations. The queue lock is released before any handler runs.
void SystemController::run() {
    if (!initialized_ && !initSystem()) return;
    while (running.load() && !stopping_) {
        MainMessage message;
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) {
            processMessage(message);
        }
        checkUsbEvents();
        dispatch_deferred_operation_request();
        dispatch_deferred_mount();
        const auto now = chrono::steady_clock::now();
        if (now >= next_storage_check_ && !deferred_storage_request_ &&
            !pending_.has_value()) {
            FileIoManager::Request request;
            request.kind = FileIoManager::RequestKind::RefreshStorage;
            writer_.request(move(request));
            next_storage_check_ = now + chrono::milliseconds(250);
        }
    }
    stop();
}

// Developer/service stop: wake workers and join them before shared objects are destroyed.
void SystemController::stop() {
    if (stopping_) return;
    stopping_ = true;
    network_.stop();
    recording_.store(false);
    recording_buffer_.stop_producing();
    playback_buffer_.stop_producing();
    if (audio_) audio_->stop();
    writer_.stop();
    if (udev_monitor_ != nullptr) {
        udev_monitor_unref(udev_monitor_);
        udev_monitor_ = nullptr;
    }
    if (udev_context_ != nullptr) {
        udev_unref(udev_context_);
        udev_context_ = nullptr;
    }
    udev_fd_ = -1;
    led_driver_.closeDriver();
    command_queue_.close();
}

uint32_t SystemController::begin_operation(
    OperationKind kind, phono::protocol::Command command, uint32_t argument) {
    auto operation_id = next_operation_id_++;
    if (operation_id == 0) operation_id = next_operation_id_++;
    pending_ = PendingOperation{kind, command, operation_id, argument};
    return operation_id;
}

bool SystemController::request_file(FileIoManager::Request request) {
    if (writer_.request(move(request))) return true;
    cerr << "FileIO request rejected because its mailbox is occupied\n";
    return false;
}

bool SystemController::queue_file_request(FileIoManager::Request request) {
    if (writer_.request(request)) return true;
    if (deferred_operation_request_) {
        cerr << "A deferred FileIO operation is already pending\n";
        return false;
    }
    deferred_operation_request_ = move(request);
    return true;
}

bool SystemController::request_audio(AudioEngine::Request request) {
    if (audio_ && audio_->request(request)) return true;
    cerr << "Audio request rejected because its mailbox is occupied\n";
    return false;
}

// These methods enqueue replies. Only th_Network writes to the TCP socket.
void SystemController::broadcastStatus() {
    publish_status();
    publish_state();
}

void SystemController::publish_status() {
    using namespace phono::protocol;
    const bool healthy = error_code_.load() == 0;
    const bool mounted = (storage_flags_ & StorageMounted) != 0;
    const Status status{
        static_cast<uint8_t>(mounted),
        static_cast<uint8_t>(
            mounted && audio_ready_.load() && healthy &&
            (storage_flags_ & (StorageWritable | StorageSpaceOk)) ==
                (StorageWritable | StorageSpaceOk)),
        static_cast<uint8_t>(
            mounted && audio_ready_.load() && healthy &&
            (storage_flags_ & (StorageReadable | StorageSpaceOk)) ==
                (StorageReadable | StorageSpaceOk)),
        static_cast<uint8_t>(error_code_.load() != 0
                                 ? min(error_code_.load(), 255U)
                                 : (led_error_.load()
                                        ? static_cast<unsigned>(Error::LedDriverError)
                                        : 0U))};
    network_.sendStatus(status);
}

void SystemController::publish_state() {
    using namespace phono::protocol;
    unsigned mode;
    unsigned gain;
    {
        PthreadLock lock(configuration_mutex_);
        mode = mode_.load();
        gain = gain_.load();
    }
    const State state{
        transport_.load(),
        static_cast<uint8_t>(min(mode, 2U)),
        static_cast<uint8_t>(gain),
        storage_flags_,
        static_cast<uint8_t>(error_code_.load() != 0
                                 ? min(error_code_.load(), 255U)
                                 : (led_error_.load()
                                        ? static_cast<unsigned>(Error::LedDriverError)
                                        : 0U)),
        active_session_.load(),
        dropped_telemetry_.load(),
        recording_overruns_.load()};
    network_.queue_control(make_state_packet(state));
}

void SystemController::publish_command_result(
    phono::protocol::Command command, phono::protocol::Result result,
    uint32_t detail) {
    network_.queue_control(phono::protocol::make_command_result_packet(
        {command, result, detail}));
}

void SystemController::update_leds() {
    // Write the complete target mask once; never erase the selected mode on STOP.
    uint8_t mask = recording_.load() ? 0x08 : 0;
    const uint8_t mode_masks[] = {0x04, 0x01, 0x02};
    mask |= mode_masks[min(mode_.load(), 2U)];
    const bool ok = led_driver_.writeByte(mask);
    led_error_.store(!ok);
}

// Keep stable connection-scoped IDs for unchanged files, using filesystem identity.
void SystemController::handle_archive_result(const MainMessage& message) {
    archive_entries_.clear();
    for (const auto& identity : message.archive) {
        auto known = archive_id_by_name_.find(identity.basename);
        bool unchanged = false;
        if (known != archive_id_by_name_.end()) {
            const auto old = archive_by_id_.find(known->second);
            unchanged =
                old != archive_by_id_.end() &&
                old->second.device == identity.device &&
                old->second.inode == identity.inode &&
                old->second.size == identity.size &&
                old->second.modified.tv_sec == identity.modified.tv_sec &&
                old->second.modified.tv_nsec == identity.modified.tv_nsec;
        }
        uint32_t id = 0;
        if (unchanged) {
            id = known->second;
            archive_by_id_[id] = identity;
        } else {
            if (next_file_id_ == 0) {
                complete_operation(false, static_cast<uint8_t>(
                                             phono::protocol::Error::StaleFileId));
                return;
            }
            id = next_file_id_++;
            archive_id_by_name_[identity.basename] = id;
            archive_by_id_[id] = identity;
        }
        if (identity.basename.size() <=
                phono::protocol::kMaxBasenameBytes &&
            identity.basename.find_first_of("\t\r\n/\\") == string::npos) {
            archive_entries_.push_back({id, identity.basename});
        }
    }
    sort(archive_entries_.begin(), archive_entries_.end(),
         [](const auto& left, const auto& right) {
             return left.basename < right.basename;
         });
    queue_archive_page(pending_->argument);
}

void SystemController::queue_archive_page(uint32_t cursor) {
    using namespace phono::protocol;
    size_t start = 0;
    if (cursor != 0) {
        const auto found = find_if(
            archive_entries_.begin(), archive_entries_.end(),
            [cursor](const auto& entry) { return entry.id == cursor; });
        if (found == archive_entries_.end()) {
            complete_operation(false, static_cast<uint8_t>(Error::StaleFileId));
            return;
        }
        start = static_cast<size_t>(found - archive_entries_.begin()) + 1;
    }
    vector<FileEntry> page;
    size_t payload_size = string("next_cursor=4294967295\n").size();
    for (size_t i = start;
         i < archive_entries_.size() && page.size() < kMaxFileListEntries;
         ++i) {
        const auto& entry = archive_entries_[i];
        const auto entry_size =
            to_string(entry.id).size() + 1 + entry.basename.size() + 1;
        if (payload_size + entry_size > kMaxFileListPayloadBytes) break;
        page.push_back(entry);
        payload_size += entry_size;
    }
    const bool more = start + page.size() < archive_entries_.size();
    const auto next_cursor = more && !page.empty() ? page.back().id : 0;
    try {
        network_.sendFileList(next_cursor, page);
        complete_operation(true);
    } catch (const exception& error) {
        cerr << "Cannot serialize archive page: " << error.what() << '\n';
        complete_operation(false, static_cast<uint8_t>(Error::ProtocolError));
    }
}

string SystemController::generateFilename(FilterMode mode) {
    const auto now =
        chrono::system_clock::to_time_t(chrono::system_clock::now());
    tm local{};
    localtime_r(&now, &local);
    char timestamp[32]{};
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local);
    const char* modes[] = {"GENERAL", "HEART", "LUNG"};
    char session[16]{};
    snprintf(session, sizeof(session), "%06u", next_session_id_++);
    return string(modes[min(static_cast<unsigned>(mode), 2U)]) + "_" +
           timestamp + "_" + session + ".wav";
}

void SystemController::set_error(uint8_t error_code) {
    error_code_.store(error_code);
    if (error_code != 0) {
        led_error_.store(!led_driver_.triggerErrorPulse());
    }
}

void SystemController::set_main_thread_policy() {
    // Main and Network are peers: SCHED_OTHER priority 0, nice 0.
    sched_param normal_priority{};
    const int policy_error = pthread_setschedparam(pthread_self(), SCHED_OTHER, &normal_priority);
    if (policy_error != 0) cerr << "Cannot set normal scheduling: " << strerror(policy_error) << '\n';
    if (setpriority(PRIO_PROCESS, 0, 0) != 0) cerr << "Cannot set nice 0: " << strerror(errno) << '\n';

    cpu_set_t cpus;
    CPU_ZERO(&cpus);
    CPU_SET(0, &cpus);
    const int result =
        pthread_setaffinity_np(pthread_self(), sizeof(cpus), &cpus);
    if (result != 0) {
        cerr << "Main thread CPU affinity not set: " << strerror(result)
                  << '\n';
    }
}

bool SystemController::initUdevMonitor() {
    udev_context_ = udev_new();
    if (udev_context_ == nullptr) {
        cerr << "Cannot initialize udev context\n";
        return false;
    }
    udev_monitor_ = udev_monitor_new_from_netlink(udev_context_, "udev");
    if (udev_monitor_ == nullptr ||
        udev_monitor_filter_add_match_subsystem_devtype(
            udev_monitor_, "block", nullptr) < 0 ||
        udev_monitor_enable_receiving(udev_monitor_) < 0) {
        cerr << "Cannot initialize udev block-device monitor\n";
        if (udev_monitor_ != nullptr) {
            udev_monitor_unref(udev_monitor_);
            udev_monitor_ = nullptr;
        }
        udev_unref(udev_context_);
        udev_context_ = nullptr;
        return false;
    }
    udev_fd_ = udev_monitor_get_fd(udev_monitor_);
    if (udev_fd_ < 0) {
        cerr << "Cannot get udev monitor descriptor\n";
        udev_monitor_unref(udev_monitor_);
        udev_monitor_ = nullptr;
        udev_unref(udev_context_);
        udev_context_ = nullptr;
        return false;
    }
    return true;
}

// Poll udev without blocking Main; mount/unmount/file work stays in FileIO.
void SystemController::checkUsbEvents() {
    if (udev_fd_ < 0) return;
    pollfd event{udev_fd_, POLLIN, 0};
    const int ready = ::poll(&event, 1, 0);
    if (ready < 0) {
        if (errno != EINTR) {
            cerr << "udev monitor poll: " << strerror(errno) << '\n';
        }
        return;
    }
    if (ready == 0 || !(event.revents & POLLIN)) return;

    while (true) {
        errno = 0;
        udev_device* device = udev_monitor_receive_device(udev_monitor_);
        if (device == nullptr) {
            if (errno != 0 && errno != EAGAIN && errno != EWOULDBLOCK &&
                errno != EINTR) {
                cerr << "Cannot read udev event: " << strerror(errno) << '\n';
            }
            return;
        }
        const auto* usb_parent =
            udev_device_get_parent_with_subsystem_devtype(
                device, "usb", "usb_device");
        const char* action = udev_device_get_action(device);
        const char* device_node = udev_device_get_devnode(device);
        const char* filesystem_type =
            udev_device_get_property_value(device, "ID_FS_TYPE");
        if (usb_parent != nullptr && action != nullptr) {
            FileIoManager::Request request;
            request.path = device_node != nullptr ? device_node : "";
            if (strcmp(action, "remove") == 0 && device_node != nullptr) {
                request.kind = FileIoManager::RequestKind::DeviceRemoved;
                deferred_storage_request_ = move(request);
            } else if ((strcmp(action, "add") == 0 ||
                        strcmp(action, "change") == 0) &&
                       device_node != nullptr && filesystem_type != nullptr) {
                request.kind = FileIoManager::RequestKind::Mount;
                request.filesystem_type = filesystem_type;
                deferred_storage_request_ = move(request);
            } else {
                request.kind = FileIoManager::RequestKind::RefreshStorage;
                deferred_storage_request_ = move(request);
            }
        }
        udev_device_unref(device);
    }
}

void SystemController::dispatch_deferred_mount() {
    if (!deferred_storage_request_) return;
    if (writer_.request(*deferred_storage_request_)) {
        deferred_storage_request_.reset();
    }
}

void SystemController::dispatch_deferred_operation_request() {
    if (!deferred_operation_request_) return;
    if (writer_.request(*deferred_operation_request_)) {
        deferred_operation_request_.reset();
    }
}
}  // namespace phono_app
