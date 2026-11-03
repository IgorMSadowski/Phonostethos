#pragma once

#include "phonostethos/AudioEngine.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/LedDriver.h"
#include "phonostethos/protocol.h"
#include "phonostethos/RuntimeTypes.h"

#include <map>
#include <memory>
#include <mutex>

using namespace std;

namespace phono_app {
struct SystemController {
    explicit SystemController(const Options& options);

    void update_leds();
    void refresh_runtime_state();

    const Options& options;
    atomic<unsigned> mode{static_cast<unsigned>(FilterMode::General)};
    atomic<unsigned> gain{1};
    atomic<bool> recording{false};
    atomic<unsigned> error_code{0};
    atomic<bool> led_error{false};
    atomic<phono::protocol::Transport> transport{
        phono::protocol::Transport::Live};
    atomic<uint32_t> active_session{0};
    atomic<uint32_t> dropped_telemetry{0};
    atomic<uint32_t> recording_overruns{0};
    Telemetry telemetry;
    mutex configuration_mutex;
    mutex telemetry_mutex;
    mutex archive_mutex;
    map<uint32_t, ArchiveIdentity> archive_by_id;
    map<string, uint32_t> archive_id_by_name;
    uint32_t next_file_id = 1;
    uint32_t next_session_id = 1;
    LedDriver led_driver;
    FileIOManager writer;
    CommandQueue command_queue;
    unique_ptr<AudioEngine> audio;
};
}  // namespace phono_app
