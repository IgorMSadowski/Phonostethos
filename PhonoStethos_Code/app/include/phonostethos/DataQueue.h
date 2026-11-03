#pragma once

#include "phonostethos/RuntimeTypes.h"
#include "phonostethos/SynchronizedQueue.h"

namespace phono_app {
class DataQueue : public SynchronizedQueue<PlaybackBlock, kPlaybackCapacityBlocks> {
};
}  // namespace phono_app
