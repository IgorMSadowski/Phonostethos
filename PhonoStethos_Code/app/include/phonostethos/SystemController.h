#pragma once

#include "phonostethos/AudioEngine.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/LedDriver.h"
#include "phonostethos/protocol.h"
#include "phonostethos/RuntimeTypes.h"

#include <map>
#include <memory>
#include <mutex>

namespace phono_app {
struct SystemController {
    explicit SystemController(const Options& options);

    void update_leds();
    void refresh_runtime_state();

    const Options& options;
    std::atomic<unsigned> mode{static_cast<unsigned>(FilterMode::General)};
    std::atomic<unsigned> gain{1};
    std::atomic<bool> recording{false};
    std::atomic<unsigned> error_code{0};
    std::atomic<bool> led_error{false};
    std::atomic<phono::protocol::Transport> transport{
        phono::protocol::Transport::Live};
    std::atomic<std::uint32_t> active_session{0};
    std::atomic<std::uint32_t> dropped_telemetry{0};
    std::atomic<std::uint32_t> recording_overruns{0};
    Telemetry telemetry;
    std::mutex configuration_mutex;
    std::mutex telemetry_mutex;
    std::mutex archive_mutex;
    std::map<std::uint32_t, ArchiveIdentity> archive_by_id;
    std::map<std::string, std::uint32_t> archive_id_by_name;
    std::uint32_t next_file_id = 1;
    std::uint32_t next_session_id = 1;
    LedDriver led_driver;
    FileIOManager writer;
    CommandQueue command_queue;
    std::unique_ptr<AudioEngine> audio;
};
}  // namespace phono_app
