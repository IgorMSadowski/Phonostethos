#include "phonostethos/SystemController.h"

namespace phono_app {
SystemController::SystemController(const Options& options)
    : options(options),
      led_driver(options.led_device),
      writer(error_code, recording, recording_overruns, options) {}

void SystemController::update_leds() {
    led_error.store(!led_driver.set_mode(static_cast<FilterMode>(mode.load()),
                                       recording.load()));
}

void SystemController::refresh_runtime_state() {
    const auto current_transport = transport.load();
    if (current_transport == phono::protocol::Transport::Playback &&
      !audio->playing()) {
      transport.store(phono::protocol::Transport::Live);
    } else if (current_transport == phono::protocol::Transport::Recording &&
             !recording.load()) {
      transport.store(phono::protocol::Transport::Live);
      active_session.store(0);
      update_leds();
    }
}
}  // namespace phono_app
