#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"

#include <array>
#include <cassert>

int main() {
    phono_app::CircularBuffer<int, 3> buffer;
    buffer.push(1);
    buffer.push(2);
    buffer.push(3);
    buffer.push(4);
    std::array<int, 3> ordered{};
    buffer.copy_ordered(ordered);
    assert((ordered == std::array<int, 3>{2, 3, 4}));

    phono_app::CommandQueue commands;
    const phono::protocol::CommandPacket command{
        phono::protocol::Command::SetGain, 4};
    assert(commands.push(command));
    phono::protocol::CommandPacket received;
    assert(commands.try_pop(received));
    assert(received.type == command.type && received.value == command.value);

    phono_app::DataQueue data;
    phono_app::PlaybackBlock block;
    block.count = 1;
    block.samples[0] = 123;
    assert(data.push(block));
    phono_app::PlaybackBlock consumed;
    assert(data.try_pop(consumed));
    assert(consumed.count == 1 && consumed.samples[0] == 123);
}
