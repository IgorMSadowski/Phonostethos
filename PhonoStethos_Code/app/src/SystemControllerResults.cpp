#include "phonostethos/SystemController.h"

#include "phonostethos/protocol.h"

#include <utility>

using namespace std;

namespace phono_app {
void SystemController::handleWorkerResult(const MainMessage& message) {
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
        broadcastStatus();
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
        broadcastStatus();
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
            broadcastStatus();
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
            FileIoManager::Request close_request;
            close_request.kind = FileIoManager::RequestKind::CloseRecording;
            request_file(move(close_request));
        }
        transport_.store(message.error_code == 0 ? Transport::Live
                                                  : Transport::Error);
        update_leds();
        if (message.error_code != 0) set_error(message.error_code);
        broadcastStatus();
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
            if (pending_->stage == WorkflowStage::WaitingForWorkers && pending_->file_done &&
                pending_->audio_done) {
                pending_->stage = WorkflowStage::ResumingCapture;
                if (!request_audio({AudioEngine::RequestKind::ResumeCapture,
                                    pending_->operation_id})) {
                    transport_.store(Transport::Error);
                    complete_operation(false,
                                       static_cast<uint8_t>(Error::Busy));
                }
            } else if (pending_->stage == WorkflowStage::ResumingCapture && pending_->file_done &&
                       pending_->audio_done) {
                transport_.store(Transport::Live);
                complete_operation(false, pending_->error_code);
            }
            return;
        }
        if (pending_->kind == OperationKind::StopPlayback) {
            if (pending_->stage == WorkflowStage::ResumingCapture) {
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
                pending_->stage = WorkflowStage::ResumingCapture;
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
                FileIoManager::Request close_request;
                close_request.kind = FileIoManager::RequestKind::CloseRecording;
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
                FileIoManager::Request eject_request;
                eject_request.kind = FileIoManager::RequestKind::Eject;
                eject_request.operation_id = pending_->operation_id;
                pending_->stage = WorkflowStage::EjectingStorage;
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
        case WorkerResultCode::CaptureSuspended:
            if (pending_->kind != OperationKind::StartPlayback ||
                pending_->error_code != 0) return;
            if (message.result_code == WorkerResultCode::PlaybackReady) {
                pending_->file_done = true;
            } else {
                pending_->audio_done = true;
            }
            // Both workers must be ready before AudioDSP can start playback.
            if (pending_->file_done && pending_->audio_done &&
                !pending_->commit_requested) {
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
                if (pending_->stage != WorkflowStage::WaitingForWorkers) return;
                pending_->file_done = true;
                if (pending_->audio_done) {
                    pending_->stage = WorkflowStage::ResumingCapture;
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
            if (pending_->stage == WorkflowStage::ResumingCapture) {
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
                pending_->stage = WorkflowStage::ResumingCapture;
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
                if (pending_->stage != WorkflowStage::WaitingForWorkers) return;
                pending_->audio_done = true;
                if (pending_->file_done) {
                    pending_->stage = WorkflowStage::ResumingCapture;
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
            if (pending_->stage != WorkflowStage::WaitingForWorkers) return;
            pending_->audio_done = true;
            if (pending_->file_done && !pending_->commit_requested) {
                pending_->commit_requested = true;
                pending_->stage = WorkflowStage::ResumingCapture;
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
                pending_->stage == WorkflowStage::WaitingForWorkers) {
                pending_->audio_done = true;
                if (pending_->file_done) {
                    pending_->stage = WorkflowStage::ResumingCapture;
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
                pending_->stage == WorkflowStage::WaitingForWorkers) {
                pending_->audio_done = true;
                if (pending_->file_done && !pending_->commit_requested) {
                    pending_->commit_requested = true;
                    pending_->stage = WorkflowStage::ResumingCapture;
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
                FileIoManager::Request eject_request;
                eject_request.kind = FileIoManager::RequestKind::Eject;
                eject_request.operation_id = pending_->operation_id;
                pending_->stage = WorkflowStage::EjectingStorage;
                if (!request_file(move(eject_request))) {
                    transport_.store(Transport::Live);
                    complete_operation(false, static_cast<uint8_t>(Error::Busy));
                }
                return;
            }
            if (pending_->kind == OperationKind::StopPlayback) {
                if (pending_->stage != WorkflowStage::ResumingCapture) return;
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

// Clear the pending request and send its final result, capabilities and state to the GUI.
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
    broadcastStatus();
}

// EOF uses the same asynchronous close/resume coordination as an explicit stop.
void SystemController::start_playback_cleanup(uint32_t operation_id,
                                              bool report_result,
                                              uint8_t error_code) {
    if (pending_.has_value()) return;
    pending_ = PendingOperation{OperationKind::StopPlayback,
                                phono::protocol::Command::StopPlayback,
                                operation_id, 0, false, false, error_code,
                                report_result, true, WorkflowStage::ResumingCapture};
    transport_.store(phono::protocol::Transport::Stopping);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::StopPlayback;
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

    FileIoManager::Request stop_request;
    stop_request.kind = FileIoManager::RequestKind::StopPlayback;
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
