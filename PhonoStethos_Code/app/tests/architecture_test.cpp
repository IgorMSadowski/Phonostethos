#include "phonostethos/CircularBuffer.h"
#include "phonostethos/CommandQueue.h"
#include "phonostethos/DataQueue.h"

#include <array>
#include <cassert>

using namespace std;

int main() {
    phono_app::CircularBuffer<int, 3> buffer;
    const array<int, 1> rejected{0};
    assert(!buffer.push(rejected.data(), rejected.size()));
    assert(buffer.begin_session());
    const array<int, 3> input{1, 2, 3};
    assert(buffer.push(input.data(), input.size()));
    assert(!buffer.begin_session());
    assert(!buffer.push(input.data(), 1));
    buffer.stop_producing();
    array<int, 3> ordered{};
    assert(buffer.pop(ordered.data(), ordered.size()) == ordered.size());
    assert((ordered == input));
    assert(buffer.isEmpty());
    assert(buffer.begin_session());

    phono_app::CommandQueue commands;
    const phono::protocol::CommandPacket command{
        phono::protocol::Command::SetGain, 4};
    commands.enqueue(command);
    phono_app::MainMessage received;
    assert(commands.dequeue(received, chrono::milliseconds(0)));
    assert(received.kind == phono_app::MessageKind::GuiCommand);
    assert(received.command.type == command.type &&
           received.command.value == command.value);
    phono_app::MainMessage result;
    result.kind = phono_app::MessageKind::WorkerResult;
    result.result_code = phono_app::WorkerResultCode::RecordingOpened;
    result.operation_id = 17;
    assert(commands.enqueue(result));
    assert(commands.dequeue(received, chrono::milliseconds(0)));
    assert(received.kind == phono_app::MessageKind::WorkerResult);
    assert(received.operation_id == 17);
    commands.close();
    assert(!commands.enqueue(command));

    phono_app::DataQueue data;
    data.set_connected(true);
    phono_app::Telemetry first;
    phono_app::Telemetry latest;
    first.waveform[0] = 1.0F;
    latest.waveform[0] = 2.0F;
    assert(data.try_enqueue(first));
    assert(data.try_enqueue(latest));
    const auto consumed = data.try_dequeue();
    assert(consumed.has_value() && consumed->waveform[0] == 2.0F);
    data.set_connected(false);
    assert(!data.try_enqueue(first));
    assert(!data.try_dequeue().has_value());
}
