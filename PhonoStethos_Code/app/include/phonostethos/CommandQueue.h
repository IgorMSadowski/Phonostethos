#pragma once

#include "phonostethos/SynchronizedQueue.h"
#include "phonostethos/protocol.h"

namespace phono_app {
class CommandQueue : public SynchronizedQueue<phono::protocol::CommandPacket, 32> {
};
}  // namespace phono_app
