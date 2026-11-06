#include "phonostethos/SystemController.h"

#include "phonostethos/protocol.h"

#include <utility>

using namespace std;

namespace phono_app {
// The same queue carries GUI commands and worker replies; kind tells them apart.
void SystemController::processMessage(const MainMessage& message) {
    if (message.kind == MessageKind::GuiCommand) {
        processCommand(message.command);
        return;
    }
    if (message.kind == MessageKind::WorkerError) {
        set_error(message.error_code);
    }
    handleWorkerResult(message);
}

// Reject invalid/conflicting requests before calling the named diagram operation.
void SystemController::processCommand(
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
        case Command::SetMode: setMode(static_cast<FilterMode>(request.value)); return;
        case Command::SetGain: setGain(static_cast<int>(request.value)); return;
        case Command::StartRecording: startRecording(); return;
        case Command::StopRecording: stopRecording(); return;
        case Command::PlayAudio: startPlayback(request.value); return;
        case Command::StopPlayback: stopPlayback(); return;
        case Command::EjectStorage: ejectStorage(); return;
        case Command::GetArchive: listFiles(request.value); return;
        case Command::DeleteRecording: deleteRecording(request.value); return;
    }
}

void SystemController::setMode(FilterMode mode) {
    using namespace phono::protocol;
    const auto command = Command::SetMode;
    {
        PthreadLock lock(configuration_mutex_);
        mode_.store(static_cast<unsigned>(mode));
    }
    update_leds();
    broadcastStatus();
    publish_command_result(command, Result::Completed, 0);
}

void SystemController::setGain(int gain) {
    using namespace phono::protocol;
    const auto command = Command::SetGain;
    {
        PthreadLock lock(configuration_mutex_);
        gain_.store(static_cast<unsigned>(gain));
    }
    broadcastStatus();
    publish_command_result(command, Result::Completed, 0);
}

// Ask FileIO to open the WAV. Recording starts only after RecordingOpened arrives.
void SystemController::startRecording() {
    using namespace phono::protocol;
    const auto command = Command::StartRecording;
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
    const auto path = options_.storage / generateFilename(mode);
    const auto session_id = next_session_id_ - 1;
    const auto operation_id =
        begin_operation(OperationKind::StartRecording, command,
                        session_id);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::OpenRecording;
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

// Close the producer gate now. FileIO drains accepted samples and closes the WAV later.
void SystemController::stopRecording() {
    using namespace phono::protocol;
    const auto command = Command::StopRecording;
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
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::CloseRecording;
    file_request.operation_id = operation_id;
    if (!queue_file_request(move(file_request))) {
        set_error(static_cast<uint8_t>(Error::Busy));
        complete_operation(false, static_cast<uint8_t>(Error::Busy));
        return;
    }
    broadcastStatus();
    publish_command_result(command, Result::AcceptedPending, 0);
    return;
}

// Prepare the file and suspend microphone capture; both workers must be ready.
void SystemController::startPlayback(uint32_t file_id) {
    using namespace phono::protocol;
    const auto command = Command::PlayAudio;
    const auto found = archive_by_id_.find(file_id);
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
                        file_id);
    transport_.store(Transport::Stopping);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::PreparePlayback;
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
    broadcastStatus();
    publish_command_result(command, Result::AcceptedPending, 0);
    return;
}

// Request both worker transitions; LIVE is published only after their acknowledgements.
void SystemController::stopPlayback() {
    using namespace phono::protocol;
    const auto command = Command::StopPlayback;
    if (transport_.load() != Transport::Playback) {
        publish_command_result(command, Result::Rejected,
                               static_cast<uint32_t>(Error::Busy));
        return;
    }
    const auto operation_id =
        begin_operation(OperationKind::StopPlayback, command, 0);
    transport_.store(Transport::Stopping);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::StopPlayback;
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
    broadcastStatus();
    publish_command_result(command, Result::AcceptedPending, 0);
    return;
}

// Finish active storage work before FileIO checks syncfs() and unmount.
void SystemController::ejectStorage() {
    using namespace phono::protocol;
    const auto command = Command::EjectStorage;
    const auto previous_transport = transport_.load();
    const auto operation_id =
        begin_operation(OperationKind::EjectStorage, command, 0);
    pending_->argument = static_cast<uint32_t>(previous_transport);
    transport_.store(Transport::Ejecting);
    if (previous_transport == Transport::Recording) {
        recording_.store(false);
        recording_buffer_.stop_producing();
        FileIoManager::Request file_request;
        file_request.kind = FileIoManager::RequestKind::CloseRecording;
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
        FileIoManager::Request file_request;
        file_request.kind = FileIoManager::RequestKind::StopPlayback;
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
        pending_->stage = WorkflowStage::EjectingStorage;
        FileIoManager::Request file_request;
        file_request.kind = FileIoManager::RequestKind::Eject;
        file_request.operation_id = operation_id;
        if (!queue_file_request(move(file_request))) {
            transport_.store(Transport::Live);
            complete_operation(false,
                               static_cast<uint8_t>(Error::Busy));
            return;
        }
    }
    broadcastStatus();
    publish_command_result(command, Result::AcceptedPending, 0);
    return;
}

void SystemController::listFiles(uint32_t cursor) {
    using namespace phono::protocol;
    const auto command = Command::GetArchive;
    const auto operation_id =
        begin_operation(OperationKind::ListArchive, command,
                        cursor);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::List;
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

void SystemController::deleteRecording(uint32_t file_id) {
    using namespace phono::protocol;
    const auto command = Command::DeleteRecording;
    const auto found = archive_by_id_.find(file_id);
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
                        file_id);
    FileIoManager::Request file_request;
    file_request.kind = FileIoManager::RequestKind::Delete;
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

// Each result advances one pending transition. operation_id rejects replies from older work.
}  // namespace phono_app
