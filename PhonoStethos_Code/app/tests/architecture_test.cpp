/**
 * architecture_test.cpp - checks architecture with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - main(): run this standalone test and return success only if its checks pass.
 */



#include "phonostethos/CircularBuffer.h" // Include the CircularBuffer class/types used here; implements the shared PCM16 circular sample buffer.
#include "phonostethos/CommandQueue.h" // Include the CommandQueue class/types used here; defines MainMessage and the FIFO consumed by th_Main.
#include "phonostethos/DataQueue.h" // Include the DataQueue class/types used here; implements the latest waveform/FFT snapshot channel.

#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cassert> // Include assertions that stop a test when an expected result is false.

using namespace std; // Allow the names in this namespace without repeating its prefix.

int main() { // Run this standalone test and return success only if its checks pass.
    phono_app::CircularBuffer buffer{3}; // Declare buffer for this operation.
    const array<int16_t, 1> rejected{0}; // Complete this expression or declaration.
    assert(!buffer.push(rejected.data(), rejected.size())); // Verify this expected result; fail the test if it is not satisfied.
    assert(buffer.begin_session()); // Verify this expected result; fail the test if it is not satisfied.
    const array<int16_t, 3> input{1, 2, 3}; // Complete this expression or declaration.
    assert(buffer.push(input.data(), input.size())); // Verify this expected result; fail the test if it is not satisfied.
    assert(!buffer.begin_session()); // Verify this expected result; fail the test if it is not satisfied.
    assert(!buffer.push(input.data(), 1)); // Verify this expected result; fail the test if it is not satisfied.
    buffer.stop_producing(); // Close this sample buffer's production gate while preserving accepted samples for draining.
    array<int16_t, 3> ordered{}; // Complete this expression or declaration.
    assert(buffer.pop(ordered.data(), ordered.size()) == ordered.size()); // Verify this expected result; fail the test if it is not satisfied.
    assert((ordered == input)); // Verify this expected result; fail the test if it is not satisfied.
    assert(buffer.isEmpty()); // Verify this expected result; fail the test if it is not satisfied.
    assert(buffer.begin_session()); // Verify this expected result; fail the test if it is not satisfied.

    phono_app::CommandQueue commands; // Declare commands for this operation.
    const phono::protocol::CommandPacket command{ // Declare command for this operation.
        phono::protocol::CommandType::SET_GAIN, 4}; // Complete this expression or declaration.
    commands.enqueue(command); // Send this record through the named protected queue; the receiver handles it later.
    phono_app::MainMessage received; // Declare received for this operation.
    assert(commands.dequeue(received, chrono::milliseconds(0))); // Verify this expected result; fail the test if it is not satisfied.
    assert(received.kind == phono_app::MessageKind::GUI_COMMAND); // Verify this expected result; fail the test if it is not satisfied.
    assert(received.command.cmd_type == command.cmd_type && // Verify this expected result; fail the test if it is not satisfied.
           received.command.value == command.value); // Continue checking the file/state comparison; no value is assigned here.
    phono_app::MainMessage result; // Declare result for this operation.
    result.kind = phono_app::MessageKind::WORKER_RESULT; // Set result.kind to phono_app::MessageKind::WORKER_RESULT.
    result.result_code = phono_app::WorkerResultCode::OPEN_READY; // Set result.result code to phono_app::WorkerResultCode::OPEN_READY.
    result.operation_id = 17; // Set result.operation id to 17.
    assert(commands.enqueue(result)); // Verify this expected result; fail the test if it is not satisfied.
    assert(commands.dequeue(received, chrono::milliseconds(0))); // Verify this expected result; fail the test if it is not satisfied.
    assert(received.kind == phono_app::MessageKind::WORKER_RESULT); // Verify this expected result; fail the test if it is not satisfied.
    assert(received.operation_id == 17); // Verify this expected result; fail the test if it is not satisfied.
    commands.close(); // Call commands.close with the arguments shown here.
    assert(!commands.enqueue(command)); // Verify this expected result; fail the test if it is not satisfied.

    phono_app::DataQueue data; // Declare data for this operation.
    data.set_connected(true); // Call data.set_connected with the arguments shown here.
    phono_app::DataPacket first; // Declare first for this operation.
    phono_app::DataPacket latest; // Declare latest for this operation.
    first.waveform[0] = 1.0F; // Set first.waveform[0] to 1.0F.
    latest.waveform[0] = 2.0F; // Set latest.waveform[0] to 2.0F.
    assert(data.enqueue(first)); // Verify this expected result; fail the test if it is not satisfied.
    assert(data.enqueue(latest)); // Verify this expected result; fail the test if it is not satisfied.
    phono_app::DataPacket consumed; // Declare consumed for this operation.
    assert(data.dequeue(consumed)); // Verify this expected result; fail the test if it is not satisfied.
    assert(consumed.waveform[0] == 2.0F); // Verify this expected result; fail the test if it is not satisfied.
    data.set_connected(false); // Call data.set_connected with the arguments shown here.
    assert(!data.enqueue(first)); // Verify this expected result; fail the test if it is not satisfied.
    assert(!data.dequeue(consumed)); // Verify this expected result; fail the test if it is not satisfied.
} // End this block, declaration or argument list.
