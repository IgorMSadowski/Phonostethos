#include "phonostethos/NetworkManager.h"
#include "phonostethos/SystemController.h"
#include "phonostethos/protocol.h"

#include <algorithm>
#include <array>
#include <arpa/inet.h>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <fcntl.h>
#include <filesystem>
#include <iostream>
#include <map>
#include <netinet/in.h>
#include <poll.h>
#include <sstream>
#include <string>
#include <system_error>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/mount.h>
#include <unistd.h>
#include <vector>

using namespace std;

namespace phono_app {
namespace {
using namespace phono_app;
bool send_all(int fd, const vector<uint8_t>& data) {
    size_t sent = 0;
    const auto deadline =
        chrono::steady_clock::now() + chrono::milliseconds(100);
    while (sent < data.size()) {
        const auto count =
            ::send(fd, data.data() + sent, data.size() - sent, MSG_NOSIGNAL);
        if (count < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN || errno == EWOULDBLOCK) {
                const auto now = chrono::steady_clock::now();
                if (now >= deadline) return false;
                const auto remaining =
                    chrono::duration_cast<chrono::milliseconds>(
                        deadline - now)
                        .count();
                pollfd writable{fd, POLLOUT, 0};
                const int ready = ::poll(
                    &writable, 1,
                    static_cast<int>(min<int64_t>(remaining, 10)));
                if (ready < 0 && errno != EINTR) return false;
                if (ready > 0 &&
                    (writable.revents & (POLLERR | POLLHUP | POLLNVAL))) {
                    return false;
                }
                continue;
            }
            return false;
        }
        if (count == 0) return false;
        sent += static_cast<size_t>(count);
    }
    return true;
}

bool storage_has_space(const Options& options) {
    error_code error;
    const auto space = filesystem::space(options.storage, error);
    return !error && space.available >= 1024 * 1024;
}

vector<string> list_recordings(const Options& options) {
    vector<string> files;
    if (!storage_ready(options)) return files;
    error_code error;
    for (filesystem::directory_iterator it(options.storage, error), end;
         !error && it != end; it.increment(error)) {
        if (it->is_regular_file(error) && it->path().extension() == ".wav") {
            files.push_back(it->path().filename().string());
        }
    }
    sort(files.begin(), files.end());
    return files;
}

vector<uint8_t> status_packet(SystemController& runtime) {
    const bool mounted = storage_ready(runtime.options);
    const bool space_ok = mounted && storage_has_space(runtime.options);
    const bool readable =
        mounted && ::access(runtime.options.storage.c_str(), R_OK) == 0;
    const bool writable =
        mounted && ::access(runtime.options.storage.c_str(), W_OK) == 0;
    const auto reported_error = runtime.error_code.load() != 0
                                    ? runtime.error_code.load()
                                    : (runtime.led_error.load()
                                           ? static_cast<unsigned>(
                                                 phono::protocol::Error::LedDriverError)
                                           : 0U);
    const bool healthy = runtime.error_code.load() == 0;
    const phono::protocol::Status status{
        static_cast<uint8_t>(mounted),
        static_cast<uint8_t>(mounted && writable && space_ok && healthy),
        static_cast<uint8_t>(mounted && readable && space_ok && healthy),
        static_cast<uint8_t>(reported_error)};
    return phono::protocol::make_status_packet(status);
}

vector<uint8_t> state_packet(SystemController& runtime) {
    uint8_t storage_flags = 0;
    const bool mounted = storage_ready(runtime.options);
    if (mounted) {
        storage_flags |= phono::protocol::StoragePresent;
        storage_flags |= phono::protocol::StorageMounted;
    }
    if (mounted && ::access(runtime.options.storage.c_str(), R_OK) == 0) {
        storage_flags |= phono::protocol::StorageReadable;
    }
    if (mounted && ::access(runtime.options.storage.c_str(), W_OK) == 0) {
        storage_flags |= phono::protocol::StorageWritable;
    }
    if (mounted && storage_has_space(runtime.options)) {
        storage_flags |= phono::protocol::StorageSpaceOk;
    }
    unsigned mode_value;
    unsigned gain_value;
    {
        lock_guard<mutex> lock(runtime.configuration_mutex);
        mode_value = runtime.mode.load();
        gain_value = runtime.gain.load();
    }
    const auto mode = static_cast<uint8_t>(
        min(mode_value, 2U));
    const phono::protocol::State state{
        runtime.transport.load(),
        mode,
        static_cast<uint8_t>(clamp(gain_value, 1U, 10U)),
        storage_flags,
        static_cast<uint8_t>(runtime.error_code.load() != 0
                                      ? min(runtime.error_code.load(), 255U)
                                      : (runtime.led_error.load()
                                             ? static_cast<unsigned>(
                                                   phono::protocol::Error::LedDriverError)
                                             : 0U)),
        runtime.active_session.load(),
        runtime.dropped_telemetry.load(),
        runtime.recording_overruns.load()};
    return phono::protocol::make_state_packet(state);
}

bool send_archive(int fd, SystemController& runtime, uint32_t cursor,
                  phono::protocol::Error& error_code) {
    error_code = phono::protocol::Error::None;
    const auto names = list_recordings(runtime.options);
    vector<phono::protocol::FileEntry> archive;
    {
        lock_guard<mutex> lock(runtime.archive_mutex);
        for (const auto& name : names) {
            const auto path = runtime.options.storage / name;
            struct stat metadata {};
            if (::lstat(path.c_str(), &metadata) != 0 ||
                !S_ISREG(metadata.st_mode)) {
                continue;
            }
            auto identity = runtime.archive_id_by_name.find(name);
            bool needs_new_id = identity == runtime.archive_id_by_name.end();
            if (!needs_new_id) {
                const auto existing = runtime.archive_by_id.find(identity->second);
                needs_new_id =
                    existing == runtime.archive_by_id.end() ||
                    !same_file(existing->second, metadata);
            }
            if (needs_new_id) {
                if (runtime.next_file_id == 0) {
                    cerr << "Connection file ID space exhausted\n";
                    return false;
                }
                const auto id = runtime.next_file_id++;
                identity = runtime.archive_id_by_name.emplace(name, id).first;
                identity->second = id;
                runtime.archive_by_id.emplace(
                    id, ArchiveIdentity{name, metadata.st_dev, metadata.st_ino,
                                         metadata.st_size, metadata.st_mtim});
            }
            if (name.size() <= phono::protocol::kMaxBasenameBytes &&
                name.find_first_of("\t\r\n/\\") == string::npos) {
                archive.push_back({identity->second, name});
            }
        }
    }

    size_t start = 0;
    if (cursor != 0) {
        const auto found = find_if(
            archive.begin(), archive.end(),
            [cursor](const auto& entry) { return entry.id == cursor; });
        if (found == archive.end()) {
            error_code = phono::protocol::Error::StaleFileId;
            return true;
        }
        start = static_cast<size_t>(found - archive.begin()) + 1;
    }

    vector<phono::protocol::FileEntry> page;
    size_t payload_bytes =
        string("next_cursor=4294967295\n").size();
    for (size_t index = start;
         index < archive.size() && page.size() < phono::protocol::kMaxFileListEntries;
         ++index) {
        const auto& entry = archive[index];
        const auto entry_bytes =
            to_string(entry.id).size() + 1 + entry.basename.size() + 1;
        if (payload_bytes + entry_bytes >
            phono::protocol::kMaxFileListPayloadBytes) {
            break;
        }
        page.push_back(entry);
        payload_bytes += entry_bytes;
    }
    const auto has_more = start + page.size() < archive.size();
    const auto next_cursor = has_more && !page.empty() ? page.back().id : 0;
    try {
        const auto packet =
            phono::protocol::make_file_list_packet(next_cursor, page);
        return send_all(fd, packet);
    } catch (const exception& error) {
        cerr << "Cannot serialize archive page: " << error.what() << '\n';
        return false;
    }
}

pair<filesystem::path, int> reserve_recording_path(SystemController& runtime) {
    const auto now = chrono::system_clock::to_time_t(
        chrono::system_clock::now());
    tm local{};
    localtime_r(&now, &local);
    char timestamp[32]{};
    strftime(timestamp, sizeof(timestamp), "%Y%m%d_%H%M%S", &local);
    const char* modes[] = {"GENERAL", "HEART", "LUNG"};
    const auto mode = min(runtime.mode.load(), 2U);
    for (uint32_t attempts = 0; attempts < 1000000; ++attempts) {
        const auto session_id = runtime.next_session_id++;
        char session[16]{};
        snprintf(session, sizeof(session), "%06u", session_id);
        const auto basename = string(modes[mode]) + "_" + timestamp +
                              "_" + session + ".wav";
        const auto path = runtime.options.storage / basename;
        const int descriptor =
            ::open(path.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0600);
        if (descriptor >= 0) {
            runtime.active_session.store(session_id);
            return {path, descriptor};
        }
        if (errno != EEXIST) {
            throw system_error(errno, generic_category(),
                                    "exclusive recording-file creation");
        }
    }
    throw runtime_error("recording filename counter exhausted");
}

phono::protocol::CommandResult handle_command(
    const phono::protocol::CommandPacket& request, SystemController& runtime) {
    using phono::protocol::Command;
    using phono::protocol::Error;
    using phono::protocol::Result;
    const auto command = request.type;
    const auto value = request.value;
    const auto completed = [command] {
        return phono::protocol::CommandResult{command, Result::Completed, 0};
    };
    const auto rejected = [command](Error error) {
        return phono::protocol::CommandResult{
            command, Result::Rejected, static_cast<uint32_t>(error)};
    };
    const auto failed = [command](Error error) {
        return phono::protocol::CommandResult{
            command, Result::Failed, static_cast<uint32_t>(error)};
    };
    const auto worker_failure = [&runtime](Error fallback) {
        const auto code = runtime.error_code.load();
        return code > 0 && code <=
                               static_cast<unsigned>(Error::LedDriverError)
                   ? static_cast<Error>(code)
                   : fallback;
    };
    if (!phono::protocol::valid_command_value(request)) {
        return rejected(Error::InvalidArgument);
    }

    switch (command) {
        case Command::SetMode:
            {
                lock_guard<mutex> lock(runtime.configuration_mutex);
                runtime.mode.store(value);
            }
            runtime.update_leds();
            return completed();
        case Command::SetGain:
            {
                lock_guard<mutex> lock(runtime.configuration_mutex);
                runtime.gain.store(value);
            }
            return completed();
        case Command::StartRecording:
            if (!runtime.audio->ready()) {
                return rejected(Error::MicLost);
            }
            if (runtime.transport.load() !=
                phono::protocol::Transport::Live) {
                return rejected(Error::Busy);
            }
            if (!storage_ready(runtime.options)) {
                runtime.error_code.store(
                    static_cast<unsigned>(Error::StorageLost));
                return rejected(Error::StorageLost);
            }
            try {
                const auto [path, descriptor] = reserve_recording_path(runtime);
                runtime.error_code.store(0);
                if (!runtime.writer.start(path, descriptor)) {
                    const auto error = runtime.error_code.load();
                    if (error == static_cast<unsigned>(Error::DiskFull) ||
                        error == static_cast<unsigned>(Error::OpenError)) {
                        runtime.active_session.store(0);
                        return failed(static_cast<Error>(error));
                    }
                    ::close(descriptor);
                    error_code remove_error;
                    filesystem::remove(path, remove_error);
                    runtime.active_session.store(0);
                    return rejected(Error::Busy);
                }
            } catch (const system_error& error) {
                cerr << "Cannot create recording path: " << error.what()
                          << '\n';
                const auto failure = error.code().value() == ENOSPC
                                         ? Error::DiskFull
                                         : Error::OpenError;
                runtime.error_code.store(static_cast<unsigned>(failure));
                runtime.active_session.store(0);
                return failed(failure);
            } catch (const runtime_error& error) {
                cerr << "Cannot create recording path: " << error.what()
                          << '\n';
                runtime.error_code.store(
                    static_cast<unsigned>(Error::OpenError));
                runtime.active_session.store(0);
                return failed(Error::OpenError);
            }
            runtime.recording.store(true);
            runtime.transport.store(phono::protocol::Transport::Recording);
            runtime.error_code.store(0);
            runtime.update_leds();
            return completed();
        case Command::StopRecording: {
            if (runtime.transport.load() !=
                phono::protocol::Transport::Recording) {
                return rejected(Error::Busy);
            }
            runtime.transport.store(phono::protocol::Transport::Stopping);
            runtime.recording.store(false);
            const bool stopped = runtime.writer.finish();
            runtime.update_leds();
            runtime.transport.store(phono::protocol::Transport::Live);
            runtime.active_session.store(0);
            return stopped && runtime.error_code.load() == 0
                       ? completed()
                       : failed(worker_failure(Error::FinalizeError));
        }
        case Command::PlayAudio: {
            ArchiveIdentity identity;
            {
                lock_guard<mutex> lock(runtime.archive_mutex);
                const auto found = runtime.archive_by_id.find(value);
                if (found == runtime.archive_by_id.end()) {
                    return rejected(Error::StaleFileId);
                }
                identity = found->second;
            }
            const filesystem::path safe_name(identity.basename);
            struct stat metadata {};
            if (safe_name.filename() != safe_name ||
                safe_name.extension() != ".wav") {
                return rejected(Error::InvalidArgument);
            }
            if (::lstat((runtime.options.storage / safe_name).c_str(),
                        &metadata) != 0 ||
                !same_file(identity, metadata)) {
                return rejected(Error::StaleFileId);
            }
            if (!runtime.audio->ready() ||
                runtime.transport.load() !=
                    phono::protocol::Transport::Live ||
                !storage_ready(runtime.options) ||
                ::access(runtime.options.storage.c_str(), R_OK) != 0 ||
                !storage_has_space(runtime.options) ||
                runtime.error_code.load() != 0) {
                return rejected(Error::StorageLost);
            }
            const auto path = runtime.options.storage / safe_name;
            const int descriptor =
                ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
            struct stat opened_metadata {};
            if (descriptor < 0 ||
                ::fstat(descriptor, &opened_metadata) != 0 ||
                !same_file(identity, opened_metadata)) {
                if (descriptor >= 0) ::close(descriptor);
                return rejected(Error::StaleFileId);
            }
            if (!runtime.audio->start_playback(path, descriptor)) {
                return runtime.error_code.load() ==
                               static_cast<unsigned>(Error::UnsupportedWav)
                           ? failed(Error::UnsupportedWav)
                           : failed(Error::ReadError);
            }
            runtime.transport.store(phono::protocol::Transport::Playback);
            return completed();
        }
        case Command::StopPlayback:
            if (runtime.transport.load() !=
                phono::protocol::Transport::Playback) {
                return rejected(Error::Busy);
            }
            runtime.audio->stop_playback();
            runtime.transport.store(phono::protocol::Transport::Live);
            return completed();
        case Command::EjectStorage: {
            runtime.transport.store(phono::protocol::Transport::Ejecting);
            runtime.recording.store(false);
            runtime.audio->stop_playback();
            const bool recording_closed = runtime.writer.finish_and_wait();
            runtime.update_leds();
            runtime.active_session.store(0);
            if (!recording_closed) {
                runtime.transport.store(phono::protocol::Transport::Live);
                return failed(worker_failure(Error::FinalizeError));
            }
            if (!storage_ready(runtime.options)) {
                runtime.transport.store(phono::protocol::Transport::Live);
                runtime.error_code.store(
                    static_cast<unsigned>(Error::StorageLost));
                return failed(Error::StorageLost);
            }
            const int storage_fd =
                ::open(runtime.options.storage.c_str(), O_RDONLY | O_DIRECTORY);
            if (storage_fd < 0 || ::syncfs(storage_fd) != 0) {
                const int sync_error = errno;
                if (storage_fd >= 0) ::close(storage_fd);
                cerr << "Cannot synchronize storage: "
                          << strerror(sync_error) << '\n';
                runtime.error_code.store(
                    static_cast<unsigned>(Error::UnmountError));
                runtime.transport.store(phono::protocol::Transport::Live);
                return failed(Error::UnmountError);
            }
            if (::close(storage_fd) != 0) {
                cerr << "Cannot close synchronized storage handle: "
                          << strerror(errno) << '\n';
                runtime.error_code.store(
                    static_cast<unsigned>(Error::UnmountError));
                runtime.transport.store(phono::protocol::Transport::Live);
                return failed(Error::UnmountError);
            }
            if (::umount2(runtime.options.storage.c_str(), 0) != 0) {
                cerr << "Cannot eject storage " << runtime.options.storage
                          << ": " << strerror(errno) << '\n';
                runtime.error_code.store(
                    static_cast<unsigned>(Error::UnmountError));
                runtime.transport.store(phono::protocol::Transport::Live);
                return failed(Error::UnmountError);
            }
            runtime.transport.store(phono::protocol::Transport::Live);
            return completed();
        }
        case Command::GetArchive:
            return completed();
        case Command::DeleteRecording: {
            ArchiveIdentity identity;
            {
                lock_guard<mutex> lock(runtime.archive_mutex);
                const auto found = runtime.archive_by_id.find(value);
                if (found == runtime.archive_by_id.end()) {
                    return rejected(Error::StaleFileId);
                }
                identity = found->second;
            }
            const filesystem::path safe_name(identity.basename);
            struct stat metadata {};
            if (safe_name.filename() != safe_name ||
                safe_name.extension() != ".wav") {
                return rejected(Error::InvalidArgument);
            }
            if (::lstat((runtime.options.storage / safe_name).c_str(),
                        &metadata) != 0 ||
                !same_file(identity, metadata)) {
                return rejected(Error::StaleFileId);
            }
            if (runtime.transport.load() !=
                    phono::protocol::Transport::Live ||
                !storage_ready(runtime.options) ||
                ::access(runtime.options.storage.c_str(), W_OK) != 0 ||
                !storage_has_space(runtime.options) ||
                runtime.error_code.load() != 0) {
                return rejected(Error::StorageLost);
            }
            error_code error;
            if (!filesystem::remove(runtime.options.storage / safe_name,
                                         error) ||
                error) {
                runtime.error_code.store(
                    static_cast<unsigned>(Error::WriteError));
                return failed(Error::WriteError);
            }
            return completed();
        }
        default:
            return rejected(Error::InvalidArgument);
    }
}


}

void NetworkManager::serve_client(int fd, SystemController& runtime) {
    runtime.refresh_runtime_state();
    timeval io_timeout{};
    io_timeout.tv_sec = 0;
    io_timeout.tv_usec = 10000;
    if (::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &io_timeout,
                     sizeof(io_timeout)) != 0 ||
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &io_timeout,
                     sizeof(io_timeout)) != 0) {
        cerr << "Cannot configure client socket timeouts: "
                  << strerror(errno) << '\n';
        ::close(fd);
        return;
    }
    {
        lock_guard<mutex> lock(runtime.archive_mutex);
        runtime.archive_by_id.clear();
        runtime.archive_id_by_name.clear();
        runtime.next_file_id = 1;
    }
    if (!send_all(fd, status_packet(runtime)) ||
        !send_all(fd, state_packet(runtime))) {
        ::close(fd);
        return;
    }
    vector<uint8_t> receive_buffer;
    auto next_update = chrono::steady_clock::now();
    unsigned last_mode = runtime.mode.load();
    bool last_recording = runtime.recording.load();
    unsigned last_error = runtime.error_code.load();
    while (running.load()) {
        pollfd event{fd, POLLIN, 0};
        const auto ready = ::poll(&event, 1, 25);
        if (ready < 0 && errno != EINTR) break;
        if (ready > 0 && (event.revents & (POLLERR | POLLHUP | POLLNVAL))) break;
        if (ready > 0 && (event.revents & POLLIN)) {
            array<uint8_t, 128> incoming{};
            const auto count = ::recv(fd, incoming.data(), incoming.size(), 0);
            if (count <= 0) {
                if (count < 0 &&
                    (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK)) {
                    continue;
                }
                break;
            }
            receive_buffer.insert(receive_buffer.end(), incoming.begin(),
                                  incoming.begin() + count);
            while (receive_buffer.size() >= 5) {
                array<uint8_t, 5> command{};
                copy_n(receive_buffer.begin(), command.size(), command.begin());
                receive_buffer.erase(receive_buffer.begin(),
                                     receive_buffer.begin() + command.size());
                phono::protocol::CommandPacket decoded;
                if (!phono::protocol::decode_command(
                        command.data(), command.size(), decoded)) {
                    cerr << "Closing client after invalid command frame\n";
                    ::close(fd);
                    return;
                }
                if (!runtime.command_queue.push(decoded)) {
                    ::close(fd);
                    return;
                }
                phono::protocol::CommandPacket request;
                if (!runtime.command_queue.try_pop(request)) {
                    ::close(fd);
                    return;
                }

                auto result = handle_command(request, runtime);
                if (request.type == phono::protocol::Command::GetArchive &&
                    result.result == phono::protocol::Result::Completed) {
                    phono::protocol::Error archive_error;
                    if (!send_archive(fd, runtime, request.value,
                                      archive_error)) {
                        ::close(fd);
                        return;
                    }
                    if (archive_error != phono::protocol::Error::None) {
                        result.result = phono::protocol::Result::Rejected;
                        result.detail =
                            static_cast<uint32_t>(archive_error);
                    }
                }
                const auto response =
                    phono::protocol::make_command_result_packet(result);
                if (!send_all(fd, response)) {
                    ::close(fd);
                    return;
                }
            }
        }

        const auto mode = runtime.mode.load();
        const auto recording = runtime.recording.load();
        const auto error = runtime.error_code.load();
        runtime.refresh_runtime_state();
        if (mode != last_mode || recording != last_recording) {
            runtime.update_leds();
            last_mode = mode;
            last_recording = recording;
        }
        if (error != last_error && error != 0) {
            runtime.led_error.store(!runtime.led_driver.write(0xff));
        }
        last_error = error;

        if (chrono::steady_clock::now() >= next_update) {
            Telemetry telemetry;
            {
                lock_guard<mutex> lock(runtime.telemetry_mutex);
                telemetry = runtime.telemetry;
            }
            if (!send_all(fd, status_packet(runtime)) ||
                !send_all(fd, state_packet(runtime)) ||
                !send_all(fd, phono::protocol::make_data_packet(
                                  telemetry.waveform, telemetry.spectrum))) {
                break;
            }
            next_update = chrono::steady_clock::now() +
                          chrono::milliseconds(64);
        }
    }
    ::close(fd);
}

int NetworkManager::run(SystemController& controller) {
    const auto& options = controller.options;
    const int server_fd = ::socket(AF_INET, SOCK_STREAM, 0);
    if (server_fd < 0) {
        cerr << "socket: " << strerror(errno) << '\n';
        return 1;
    }
    int reuse = 1;
    (void)::setsockopt(server_fd, SOL_SOCKET, SO_REUSEADDR, &reuse,
                       sizeof(reuse));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(options.port);
    if (::inet_pton(AF_INET, options.bind_address.c_str(), &address.sin_addr) !=
        1) {
        cerr << "Invalid IPv4 bind address: " << options.bind_address
                  << '\n';
        ::close(server_fd);
        return 2;
    }
    if (::bind(server_fd, reinterpret_cast<sockaddr*>(&address),
               sizeof(address)) < 0) {
        cerr << "bind " << options.bind_address << ':' << options.port
                  << ": " << strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }
    if (::listen(server_fd, 1) < 0) {
        cerr << "listen: " << strerror(errno) << '\n';
        ::close(server_fd);
        return 1;
    }

    cout << "PhonoStethos Raspberry Pi service listening on "
              << options.bind_address << ':' << options.port << '\n';
    int result = 0;
    while (running.load()) {
        controller.refresh_runtime_state();
        pollfd listener{server_fd, POLLIN, 0};
        const auto ready = ::poll(&listener, 1, 250);
        if (ready < 0) {
            if (errno == EINTR) continue;
            cerr << "poll: " << strerror(errno) << '\n';
            result = 1;
            break;
        }
        if (ready == 0) continue;
        sockaddr_in peer{};
        socklen_t peer_size = sizeof(peer);
        const int client_fd =
            ::accept(server_fd, reinterpret_cast<sockaddr*>(&peer), &peer_size);
        if (client_fd < 0) {
            if (errno == EINTR) continue;
            cerr << "accept: " << strerror(errno) << '\n';
            result = 1;
            break;
        }
        serve_client(client_fd, controller);
    }
    ::close(server_fd);
    return result;
}

}  // namespace phono_app
