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
#include <cstring>

using namespace std;

namespace phono_app {
SystemController::SystemController(const Options& options)
    : options_(options),
      led_driver_(options.led_device),
      writer_(options, command_queue_, recording_buffer_, playback_buffer_),
      network_(options, command_queue_, data_queue_, control_output_) {}

SystemController::~SystemController() { stop(); }

bool SystemController::initSystem() {
    if (initialized_) return true;
    set_main_thread_policy();
    if (!init_udev_monitor()) return false;
    if (!led_driver_.open()) {
        led_error_.store(true);
        cerr << "LED status outputs are unavailable; continuing without LEDs\n";
    }
    update_leds();
    audio_ = make_unique<AudioEngine>(
        options_, mode_, gain_, recording_, error_code_, command_queue_,
        data_queue_, configuration_mutex_, recording_buffer_, playback_buffer_,
        dropped_telemetry_, recording_overruns_);

    const auto deadline = chrono::steady_clock::now() + chrono::seconds(5);
    while (!audio_init_complete_.load() &&
           chrono::steady_clock::now() < deadline) {
        MainMessage message;
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) {
            process_message(message);
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

void SystemController::run() {
    if (!initialized_ && !initSystem()) return;
    while (running.load() && !stopping_) {
        MainMessage message;
        if (command_queue_.dequeue(message, chrono::milliseconds(10))) {
            process_message(message);
        }
        check_usb_events();
        dispatch_deferred_operation_request();
        dispatch_deferred_mount();
        const auto now = chrono::steady_clock::now();
        if (now >= next_storage_check_ && !deferred_storage_request_ &&
            !pending_.has_value()) {
            FileIOManager::Request request;
            request.kind = FileIOManager::RequestKind::RefreshStorage;
            writer_.request(move(request));
            next_storage_check_ = now + chrono::milliseconds(250);
        }
    }
    stop();
}

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
    led_driver_.close();
    command_queue_.close();
}

uint32_t SystemController::begin_operation(
    OperationKind kind, phono::protocol::Command command, uint32_t argument) {
    auto operation_id = next_operation_id_++;
    if (operation_id == 0) operation_id = next_operation_id_++;
    pending_ = PendingOperation{kind, command, operation_id, argument};
    return operation_id;
}

bool SystemController::request_file(FileIOManager::Request request) {
    if (writer_.request(move(request))) return true;
    cerr << "FileIO request rejected because its mailbox is occupied\n";
    return false;
}

bool SystemController::queue_file_request(FileIOManager::Request request) {
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

void SystemController::process_message(const MainMessage& message) {
    if (message.kind == MessageKind::GuiCommand) {
        process_command(message.command);
        return;
    }
    if (message.kind == MessageKind::WorkerError) {
        set_error(message.error_code);
    }
    handle_worker_result(message);
}

void SystemController::process_command(
    const phono::protocol::CommandPacket& request) {
    using namespace phono::protocol;
    const auto command = request.type;
    if (!valid_command_value(request)) {
        publish_command_result(command, Result::Rejected,
                               static_cast<uint32_t>(Error::InvalidArgument));
        return;
    }
    if (pending_.has_value()) {
        publish_command_result(command, Result::Rejected,
                               static_cast<uint32_t>(Error::Busy));
        return;
    }

    switch (command) {
        case Command::SetMode:
            {
                lock_guard<mutex> lock(configuration_mutex_);
                mode_.store(request.value);
            }
            update_leds();
            publish_status();
            publish_state();
            publish_command_result(command, Result::Completed, 0);
            return;
        case Command::SetGain:
            {
                lock_guard<mutex> lock(configuration_mutex_);
                gain_.store(request.value);
            }
            publish_status();
            publish_state();
            publish_command_result(command, Result::Completed, 0);
            return;
        case Command::StartRecording: {
            const auto required =
                StorageMounted | StorageWritable | StorageSpaceOk;
            if (!audio_ready_.load()) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::MicLost));
                return;
            }
            if (transport_.load() != Transport::Live) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            if ((storage_flags_ & required) != required) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::StorageLost));
                return;
            }
            const auto mode =
                static_cast<FilterMode>(mode_.load());
            const auto path = options_.storage / generate_filename(mode);
            const auto session_id = next_session_id_ - 1;
            const auto operation_id =
                begin_operation(OperationKind::StartRecording, command,
                                session_id);
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::OpenRecording;
            file_request.operation_id = operation_id;
            file_request.path = path;
            if (!request_file(move(file_request))) {
                pending_.reset();
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::StopRecording: {
            if (transport_.load() != Transport::Recording) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            const auto operation_id =
                begin_operation(OperationKind::StopRecording, command, 0);
            transport_.store(Transport::Stopping);
            recording_.store(false);
            recording_buffer_.stop_producing();
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::CloseRecording;
            file_request.operation_id = operation_id;
            if (!queue_file_request(move(file_request))) {
                set_error(static_cast<uint8_t>(Error::Busy));
                complete_operation(false, static_cast<uint8_t>(Error::Busy));
                return;
            }
            publish_status();
            publish_state();
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::PlayAudio: {
            const auto found = archive_by_id_.find(request.value);
            if (found == archive_by_id_.end()) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::StaleFileId));
                return;
            }
            const auto required =
                StorageMounted | StorageReadable | StorageSpaceOk;
            if (!audio_ready_.load() || transport_.load() != Transport::Live ||
                (storage_flags_ & required) != required || error_code_.load() != 0) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::StorageLost));
                return;
            }
            const auto operation_id =
                begin_operation(OperationKind::StartPlayback, command,
                                request.value);
            transport_.store(Transport::Stopping);
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::PreparePlayback;
            file_request.operation_id = operation_id;
            file_request.identity = found->second;
            if (!request_file(move(file_request))) {
                transport_.store(Transport::Live);
                pending_.reset();
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            if (!request_audio({AudioEngine::RequestKind::SuspendCapture,
                                operation_id})) {
                fail_start_playback(static_cast<uint8_t>(Error::Busy));
                publish_command_result(command, Result::AcceptedPending, 0);
                return;
            }
            publish_status();
            publish_state();
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::StopPlayback: {
            if (transport_.load() != Transport::Playback) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            const auto operation_id =
                begin_operation(OperationKind::StopPlayback, command, 0);
            transport_.store(Transport::Stopping);
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::StopPlayback;
            file_request.operation_id = operation_id;
            if (!queue_file_request(move(file_request))) {
                transport_.store(Transport::Playback);
                pending_.reset();
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            if (!request_audio({AudioEngine::RequestKind::StopPlayback,
                                operation_id})) {
                pending_->error_code = static_cast<uint8_t>(Error::Busy);
            }
            publish_status();
            publish_state();
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::EjectStorage: {
            const auto previous_transport = transport_.load();
            const auto operation_id =
                begin_operation(OperationKind::EjectStorage, command, 0);
            pending_->argument = static_cast<uint32_t>(previous_transport);
            transport_.store(Transport::Ejecting);
            if (previous_transport == Transport::Recording) {
                recording_.store(false);
                recording_buffer_.stop_producing();
                FileIOManager::Request file_request;
                file_request.kind = FileIOManager::RequestKind::CloseRecording;
                file_request.operation_id = operation_id;
                if (!queue_file_request(move(file_request))) {
                    transport_.store(Transport::Recording);
                    pending_.reset();
                    publish_command_result(command, Result::Rejected,
                                           static_cast<uint32_t>(Error::Busy));
                    return;
                }
            } else if (previous_transport == Transport::Playback) {
                pending_->file_done = false;
                pending_->audio_done = false;
                FileIOManager::Request file_request;
                file_request.kind = FileIOManager::RequestKind::StopPlayback;
                file_request.operation_id = operation_id;
                if (!queue_file_request(move(file_request))) {
                    transport_.store(Transport::Playback);
                    pending_.reset();
                    publish_command_result(command, Result::Rejected,
                                           static_cast<uint32_t>(Error::Busy));
                    return;
                }
                const bool audio_requested = request_audio(
                    {AudioEngine::RequestKind::StopPlayback, operation_id});
                if (!audio_requested) {
                    pending_->error_code = static_cast<uint8_t>(Error::Busy);
                }
                pending_->commit_requested = false;
            } else {
                pending_->phase = 2;
                FileIOManager::Request file_request;
                file_request.kind = FileIOManager::RequestKind::Eject;
                file_request.operation_id = operation_id;
                if (!queue_file_request(move(file_request))) {
                    transport_.store(Transport::Live);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                    return;
                }
            }
            publish_status();
            publish_state();
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::GetArchive: {
            const auto operation_id =
                begin_operation(OperationKind::ListArchive, command,
                                request.value);
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::List;
            file_request.operation_id = operation_id;
            if (!request_file(move(file_request))) {
                pending_.reset();
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
        case Command::DeleteRecording: {
            const auto found = archive_by_id_.find(request.value);
            if (found == archive_by_id_.end()) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::StaleFileId));
                return;
            }
            if (transport_.load() != Transport::Live ||
                (storage_flags_ & (StorageMounted | StorageWritable)) !=
                    (StorageMounted | StorageWritable)) {
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::StorageLost));
                return;
            }
            const auto operation_id =
                begin_operation(OperationKind::DeleteRecording, command,
                                request.value);
            FileIOManager::Request file_request;
            file_request.kind = FileIOManager::RequestKind::Delete;
            file_request.operation_id = operation_id;
            file_request.identity = found->second;
            if (!request_file(move(file_request))) {
                pending_.reset();
                publish_command_result(command, Result::Rejected,
                                       static_cast<uint32_t>(Error::Busy));
                return;
            }
            publish_command_result(command, Result::AcceptedPending, 0);
            return;
        }
    }
}

void SystemController::handle_worker_result(const MainMessage& message) {
    using namespace phono::protocol;
    if (message.result_code == WorkerResultCode::NetworkFailed) {
        if (message.error_code != 0) set_error(message.error_code);
        running.store(false);
        return;
    }
    if (message.result_code == WorkerResultCode::NetworkStarted) {
        return;
    }
    if (message.result_code == WorkerResultCode::ClientConnected) {
        archive_by_id_.clear();
        archive_id_by_name_.clear();
        archive_entries_.clear();
        next_file_id_ = 1;
        publish_status();
        publish_state();
        return;
    }
    if (message.result_code == WorkerResultCode::ClientDisconnected) {
        archive_by_id_.clear();
        archive_id_by_name_.clear();
        archive_entries_.clear();
        return;
    }
    if (message.result_code == WorkerResultCode::AudioReady) {
        audio_ready_.store(message.kind == MessageKind::WorkerResult);
        audio_init_complete_.store(true);
        if (message.error_code == 0) error_code_.store(0);
        if (message.error_code != 0) {
            transport_.store(phono::protocol::Transport::Error);
        }
        publish_status();
        publish_state();
        return;
    }
    if (message.result_code == WorkerResultCode::FileIOReady ||
        message.result_code == WorkerResultCode::StorageChanged) {
        const auto changed = storage_flags_ != message.storage_flags;
        storage_flags_ = message.storage_flags;
        const auto storage_usable =
            (storage_flags_ & (StorageMounted | StorageSpaceOk)) ==
                (StorageMounted | StorageSpaceOk) &&
            (storage_flags_ & (StorageReadable | StorageWritable)) != 0;
        const bool recovered =
            message.kind == MessageKind::WorkerResult && storage_usable &&
            error_code_.load() ==
                static_cast<unsigned>(Error::StorageLost);
        if (recovered) {
            error_code_.store(0);
            if (transport_.load() == Transport::Error) {
                transport_.store(Transport::Live);
            }
            update_leds();
        }
        if (changed || message.result_code == WorkerResultCode::FileIOReady ||
            message.kind == MessageKind::WorkerError || recovered) {
            publish_status();
            publish_state();
        }
        return;
    }
    if (message.result_code == WorkerResultCode::PlaybackFinished &&
        !pending_.has_value()) {
        start_playback_cleanup(message.operation_id, false, message.error_code);
        return;
    }
    if (message.result_code == WorkerResultCode::RecordingClosed &&
        !pending_.has_value() &&
        transport_.load() == Transport::Recording) {
        recording_.store(false);
        active_session_.store(0);
        if (message.operation_id == 0) {
            FileIOManager::Request close_request;
            close_request.kind = FileIOManager::RequestKind::CloseRecording;
            request_file(move(close_request));
        }
        transport_.store(message.error_code == 0 ? Transport::Live
                                                  : Transport::Error);
        update_leds();
        if (message.error_code != 0) set_error(message.error_code);
        publish_status();
        publish_state();
        return;
    }
    const bool playback_completion_matches_pending =
        message.result_code == WorkerResultCode::PlaybackFinished &&
        pending_.has_value() &&
        (pending_->kind == OperationKind::StopPlayback ||
         (pending_->kind == OperationKind::EjectStorage &&
          pending_->argument ==
              static_cast<uint32_t>(Transport::Playback)));
    if (!pending_.has_value() ||
        (pending_->operation_id != message.operation_id &&
         !playback_completion_matches_pending)) {
        return;
    }
    if (message.error_code != 0) {
        pending_->error_code = message.error_code;
        if (pending_->kind == OperationKind::StartPlayback) {
            if (message.result_code == WorkerResultCode::CaptureResumed) {
                pending_->audio_done = true;
                transport_.store(phono::protocol::Transport::Error);
                if (pending_->file_done) {
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            if (message.result_code == WorkerResultCode::PlaybackClosed) {
                pending_->file_done = true;
                transport_.store(Transport::Error);
                if (pending_->audio_done) {
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            fail_start_playback(message.error_code);
            return;
        }
        if (pending_->kind == OperationKind::StartRecording ||
            pending_->kind == OperationKind::StopRecording ||
            pending_->kind == OperationKind::ListArchive ||
            pending_->kind == OperationKind::DeleteRecording ||
            (pending_->kind == OperationKind::EjectStorage &&
             pending_->argument !=
                 static_cast<uint32_t>(Transport::Playback))) {
            if (pending_->kind == OperationKind::StartRecording ||
                pending_->kind == OperationKind::StopRecording ||
                pending_->kind == OperationKind::EjectStorage) {
                recording_.store(false);
                active_session_.store(0);
                transport_.store(Transport::Live);
                update_leds();
            }
            complete_operation(false, message.error_code);
            return;
        }
        if (pending_->kind == OperationKind::EjectStorage &&
            pending_->argument ==
                static_cast<uint32_t>(Transport::Playback)) {
            if (message.result_code == WorkerResultCode::PlaybackClosed) {
                pending_->file_done = true;
            } else if (message.result_code == WorkerResultCode::PlaybackStopped ||
                       message.result_code ==
                           WorkerResultCode::PlaybackFinished) {
                pending_->audio_done = true;
            } else if (message.result_code == WorkerResultCode::CaptureResumed) {
                pending_->audio_done = true;
            } else {
                transport_.store(Transport::Error);
                complete_operation(false, message.error_code);
                return;
            }
            if (pending_->phase == 0 && pending_->file_done &&
                pending_->audio_done) {
                pending_->phase = 1;
                if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                    pending_->operation_id})) {
                    transport_.store(Transport::Error);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                }
            } else if (pending_->phase == 1 && pending_->file_done &&
                       pending_->audio_done) {
                transport_.store(Transport::Live);
                complete_operation(false, pending_->error_code);
            }
            return;
        }
        if (pending_->kind == OperationKind::StopPlayback) {
            if (pending_->phase == 1) {
                if (message.result_code == WorkerResultCode::PlaybackClosed) {
                    pending_->file_done = true;
                } else if (message.result_code ==
                           WorkerResultCode::CaptureResumed) {
                    pending_->audio_done = true;
                }
                if (pending_->file_done && pending_->audio_done) {
                    transport_.store(
                        Transport::Error);
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            if (message.result_code == WorkerResultCode::PlaybackClosed) {
                pending_->file_done = true;
            } else if (message.result_code == WorkerResultCode::PlaybackStopped ||
                       message.result_code ==
                           WorkerResultCode::PlaybackFinished) {
                pending_->audio_done = true;
            } else {
                transport_.store(Transport::Error);
                complete_operation(false, message.error_code);
                return;
            }
            if (pending_->file_done && pending_->audio_done) {
                pending_->phase = 1;
                pending_->commit_requested = true;
                if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                    pending_->operation_id})) {
                    transport_.store(Transport::Error);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                }
            }
            return;
        }
    }

    switch (message.result_code) {
        case WorkerResultCode::RecordingOpened:
            if (pending_->kind != OperationKind::StartRecording) return;
            if (!recording_buffer_.begin_session()) {
                pending_->error_code = static_cast<uint8_t>(Error::Busy);
                FileIOManager::Request close_request;
                close_request.kind = FileIOManager::RequestKind::CloseRecording;
                close_request.operation_id = pending_->operation_id;
                if (!request_file(move(close_request))) {
                    transport_.store(Transport::Error);
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            active_session_.store(pending_->argument);
            recording_.store(true);
            transport_.store(Transport::Recording);
            update_leds();
            complete_operation(true);
            return;
        case WorkerResultCode::RecordingClosed:
            if (pending_->kind == OperationKind::StartRecording) {
                recording_.store(false);
                active_session_.store(0);
                transport_.store(Transport::Live);
                update_leds();
                complete_operation(false,
                                   pending_->error_code == 0
                                       ? static_cast<uint8_t>(Error::Busy)
                                       : pending_->error_code);
                return;
            }
            if (pending_->kind == OperationKind::EjectStorage) {
                if (message.error_code != 0) {
                    transport_.store(Transport::Live);
                    complete_operation(false, message.error_code);
                    return;
                }
                recording_.store(false);
                active_session_.store(0);
                FileIOManager::Request eject_request;
                eject_request.kind = FileIOManager::RequestKind::Eject;
                eject_request.operation_id = pending_->operation_id;
                pending_->phase = 2;
                if (!request_file(move(eject_request))) {
                    transport_.store(Transport::Live);
                    complete_operation(false, static_cast<uint8_t>(Error::Busy));
                }
                return;
            }
            if (pending_->kind != OperationKind::StopRecording) return;
            recording_.store(false);
            active_session_.store(0);
            transport_.store(Transport::Live);
            update_leds();
            complete_operation(message.error_code == 0, message.error_code);
            return;
        case WorkerResultCode::PlaybackReady:
            if (pending_->kind != OperationKind::StartPlayback) return;
            if (pending_->error_code != 0) return;
            pending_->file_done = true;
            if (pending_->audio_done && !pending_->commit_requested) {
                pending_->commit_requested = true;
                if (!request_audio({AudioEngine::RequestKind::StartPlayback,
                                    pending_->operation_id})) {
                    fail_start_playback(static_cast<uint8_t>(Error::Busy));
                }
            }
            return;
        case WorkerResultCode::CaptureSuspended:
            if (pending_->kind != OperationKind::StartPlayback) return;
            if (pending_->error_code != 0) return;
            pending_->audio_done = true;
            if (pending_->file_done && !pending_->commit_requested) {
                pending_->commit_requested = true;
                if (!request_audio({AudioEngine::RequestKind::StartPlayback,
                                    pending_->operation_id})) {
                    fail_start_playback(static_cast<uint8_t>(Error::Busy));
                }
            }
            return;
        case WorkerResultCode::PlaybackStarted:
            if (pending_->kind != OperationKind::StartPlayback) return;
            transport_.store(Transport::Playback);
            complete_operation(true);
            return;
        case WorkerResultCode::PlaybackClosed:
            if (pending_->kind == OperationKind::StartPlayback &&
                pending_->error_code != 0) {
                pending_->file_done = true;
                if (pending_->audio_done) {
                    if (transport_.load() != Transport::Error) {
                        transport_.store(Transport::Live);
                    }
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            if (pending_->kind == OperationKind::EjectStorage) {
                if (pending_->phase != 0) return;
                pending_->file_done = true;
                if (pending_->audio_done) {
                    pending_->phase = 1;
                    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                        pending_->operation_id})) {
                        transport_.store(Transport::Live);
                        complete_operation(false,
                                           static_cast<uint8_t>(Error::Busy));
                    }
                }
                return;
            }
            if (pending_->kind != OperationKind::StopPlayback) return;
            pending_->file_done = true;
            if (pending_->phase == 1) {
                if (pending_->audio_done) {
                    transport_.store(pending_->error_code == 0
                                         ? Transport::Live
                                         : Transport::Error);
                    complete_operation(pending_->error_code == 0,
                                       pending_->error_code);
                }
                return;
            }
            if (pending_->audio_done && !pending_->commit_requested) {
                pending_->commit_requested = true;
                pending_->phase = 1;
                if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                    pending_->operation_id})) {
                    transport_.store(Transport::Error);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                }
            }
            return;
        case WorkerResultCode::PlaybackStopped:
            if (pending_->kind == OperationKind::EjectStorage) {
                if (pending_->phase != 0) return;
                pending_->audio_done = true;
                if (pending_->file_done) {
                    pending_->phase = 1;
                    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                        pending_->operation_id})) {
                        transport_.store(Transport::Live);
                        complete_operation(false,
                                           static_cast<uint8_t>(Error::Busy));
                    }
                }
                return;
            }
            if (pending_->kind != OperationKind::StopPlayback) return;
            if (pending_->phase != 0) return;
            pending_->audio_done = true;
            if (pending_->file_done && !pending_->commit_requested) {
                pending_->commit_requested = true;
                pending_->phase = 1;
                if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                    pending_->operation_id})) {
                    transport_.store(Transport::Error);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                }
            }
            return;
        case WorkerResultCode::PlaybackFinished:
            if (pending_->kind == OperationKind::EjectStorage &&
                pending_->argument ==
                    static_cast<uint32_t>(Transport::Playback) &&
                pending_->phase == 0) {
                pending_->audio_done = true;
                if (pending_->file_done) {
                    pending_->phase = 1;
                    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                        pending_->operation_id})) {
                        transport_.store(Transport::Error);
                        complete_operation(false,
                                           static_cast<uint8_t>(Error::Busy));
                    }
                }
                return;
            }
            if (pending_->kind == OperationKind::StopPlayback &&
                pending_->phase == 0) {
                pending_->audio_done = true;
                if (pending_->file_done && !pending_->commit_requested) {
                    pending_->commit_requested = true;
                    pending_->phase = 1;
                    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                        pending_->operation_id})) {
                        transport_.store(Transport::Error);
                        complete_operation(false,
                                           static_cast<uint8_t>(Error::Busy));
                    }
                }
                return;
            }
            return;
        case WorkerResultCode::CaptureResumed:
            if (pending_->kind == OperationKind::StartPlayback &&
                pending_->error_code != 0) {
                pending_->audio_done = true;
                if (pending_->file_done) {
                    if (transport_.load() != Transport::Error) {
                        transport_.store(Transport::Live);
                    }
                    complete_operation(false, pending_->error_code);
                }
                return;
            }
            if (pending_->kind == OperationKind::EjectStorage) {
                if (pending_->error_code != 0) {
                    transport_.store(Transport::Live);
                    complete_operation(false, pending_->error_code);
                    return;
                }
                FileIOManager::Request eject_request;
                eject_request.kind = FileIOManager::RequestKind::Eject;
                eject_request.operation_id = pending_->operation_id;
                pending_->phase = 2;
                if (!request_file(move(eject_request))) {
                    transport_.store(Transport::Live);
                    complete_operation(false, static_cast<uint8_t>(Error::Busy));
                }
                return;
            }
            if (pending_->kind == OperationKind::StopPlayback) {
                if (pending_->phase != 1) return;
                pending_->audio_done = true;
                if (pending_->file_done) {
                    transport_.store(pending_->error_code == 0
                                         ? Transport::Live
                                         : Transport::Error);
                    complete_operation(pending_->error_code == 0,
                                       pending_->error_code);
                }
                return;
            }
            return;
        case WorkerResultCode::ArchiveReady:
            if (pending_->kind == OperationKind::ListArchive) {
                handle_archive_result(message);
            }
            return;
        case WorkerResultCode::RecordingDeleted:
            if (pending_->kind != OperationKind::DeleteRecording) return;
            complete_operation(message.error_code == 0, message.error_code);
            return;
        case WorkerResultCode::StorageEjected:
            if (pending_->kind != OperationKind::EjectStorage) return;
            if (message.error_code == 0) storage_flags_ = 0;
            transport_.store(Transport::Live);
            complete_operation(message.error_code == 0, message.error_code);
            return;
        case WorkerResultCode::AudioReady:
        case WorkerResultCode::FileIOReady:
        case WorkerResultCode::StorageChanged:
        case WorkerResultCode::NetworkStarted:
        case WorkerResultCode::NetworkFailed:
        case WorkerResultCode::ClientConnected:
        case WorkerResultCode::ClientDisconnected:
            return;
    }
}

void SystemController::complete_operation(bool success, uint8_t error_code) {
    if (!pending_.has_value()) return;
    const auto operation = *pending_;
    pending_.reset();
    if (!success && error_code != 0) set_error(error_code);
    if (operation.report_result) {
        publish_command_result(
            operation.command,
            success ? phono::protocol::Result::Completed
                    : phono::protocol::Result::Failed,
            success ? 0U : static_cast<uint32_t>(error_code));
    }
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
    network_.queue_control(make_status_packet(status));
}

void SystemController::publish_state() {
    using namespace phono::protocol;
    unsigned mode;
    unsigned gain;
    {
        lock_guard<mutex> lock(configuration_mutex_);
        mode = mode_.load();
        gain = gain_.load();
    }
    const State state{
        transport_.load(),
        static_cast<uint8_t>(min(mode, 2U)),
        static_cast<uint8_t>(clamp(gain, 1U, 10U)),
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
    const bool ok =
        led_driver_.set_mode(static_cast<FilterMode>(mode_.load()),
                             recording_.load());
    led_error_.store(!ok);
}

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
        network_.queue_control(make_file_list_packet(next_cursor, page));
        complete_operation(true);
    } catch (const exception& error) {
        cerr << "Cannot serialize archive page: " << error.what() << '\n';
        complete_operation(false, static_cast<uint8_t>(Error::ProtocolError));
    }
}

string SystemController::generate_filename(FilterMode mode) {
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
        led_error_.store(!led_driver_.write(0xff));
    }
}

void SystemController::set_main_thread_policy() {
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

bool SystemController::init_udev_monitor() {
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

void SystemController::check_usb_events() {
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
            FileIOManager::Request request;
            request.path = device_node != nullptr ? device_node : "";
            if (strcmp(action, "remove") == 0 && device_node != nullptr) {
                request.kind = FileIOManager::RequestKind::DeviceRemoved;
                deferred_storage_request_ = move(request);
            } else if ((strcmp(action, "add") == 0 ||
                        strcmp(action, "change") == 0) &&
                       device_node != nullptr && filesystem_type != nullptr) {
                request.kind = FileIOManager::RequestKind::Mount;
                request.filesystem_type = filesystem_type;
                deferred_storage_request_ = move(request);
            } else {
                request.kind = FileIOManager::RequestKind::RefreshStorage;
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

void SystemController::start_playback_cleanup(uint32_t operation_id,
                                              bool report_result,
                                              uint8_t error_code) {
    if (pending_.has_value()) return;
    pending_ = PendingOperation{OperationKind::StopPlayback,
                                phono::protocol::Command::StopPlayback,
                                operation_id, 0, false, false, error_code,
                                report_result, true, 1};
    transport_.store(phono::protocol::Transport::Stopping);
    FileIOManager::Request file_request;
    file_request.kind = FileIOManager::RequestKind::StopPlayback;
    file_request.operation_id = operation_id;
    if (!request_file(move(file_request))) {
        pending_->file_done = true;
        pending_->error_code = static_cast<uint8_t>(
            phono::protocol::Error::Busy);
        transport_.store(phono::protocol::Transport::Error);
    }
    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                        operation_id})) {
        pending_->audio_done = true;
        pending_->error_code = static_cast<uint8_t>(
            phono::protocol::Error::Busy);
        transport_.store(phono::protocol::Transport::Error);
    }
    if (pending_->file_done && pending_->audio_done) {
        complete_operation(false, pending_->error_code);
    }
}

void SystemController::fail_start_playback(uint8_t error_code) {
    if (!pending_.has_value() ||
        pending_->kind != OperationKind::StartPlayback) {
        return;
    }
    pending_->error_code = error_code;
    pending_->commit_requested = true;
    pending_->file_done = false;
    pending_->audio_done = false;

    FileIOManager::Request stop_request;
    stop_request.kind = FileIOManager::RequestKind::StopPlayback;
    stop_request.operation_id = pending_->operation_id;
    if (!request_file(move(stop_request))) {
        pending_->file_done = true;
        transport_.store(phono::protocol::Transport::Error);
    }
    if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                        pending_->operation_id})) {
        pending_->audio_done = true;
        transport_.store(phono::protocol::Transport::Error);
    }
    if (pending_->file_done && pending_->audio_done) {
        complete_operation(false, pending_->error_code);
    }
}
}  // namespace phono_app
