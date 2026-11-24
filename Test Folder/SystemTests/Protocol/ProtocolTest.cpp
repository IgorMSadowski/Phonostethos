/**
 * ProtocolTest.cpp - checks Protocol Test with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - main(): run this standalone test and return success only if its checks pass.
 */



// Test the actual C++ wire helpers; no socket, microphone or GUI is required.
#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.
#include <cstdio> // Include C-style file/console formatting used by this file.

using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.

int main() { // Run this standalone test and return success only if its checks pass.
    CommandPacket requested; // Declare requested for this operation.
    requested.cmd_type = CommandType::SET_GAIN; // Set requested.cmd type to CommandType::SET_GAIN.
    requested.value = GAIN_HALF; // Set requested.value to GAIN_HALF.
    auto bytes = make_command_packet(requested); // Initialize bytes to make_command_packet(requested).
    uint8_t expected[5] = {2, 0, 0, 0, 101}; // Initialize expected to {2, 0, 0, 0, 101}.
    if (bytes.size() != 5) return 1; // Test the condition bytes.size() != 5. If true, return 1 to the caller.
    for (int i = 0; i < 5; i++) { // Visit the specified items/indexes in order.
        if (bytes[i] != expected[i]) return 1; // Test the condition bytes[i] != expected[i]. If true, return 1 to the caller.
    } // End this block, declaration or argument list.
    CommandPacket decoded; // Declare decoded for this operation.
    if (!decode_command(bytes.data(), bytes.size(), decoded) || // Test the condition !decode_command(bytes.data(), bytes.size(), decoded) ||.
        decoded.value != requested.value || decoded.cmd_type != requested.cmd_type) return 1; // Complete this expression or declaration.
    if (decode_command(bytes.data(), 4, decoded)) return 1; // Test the condition decode_command(bytes.data(), 4, decoded). If true, return 1 to the caller.
    std::puts("PASS: five-byte command, big-endian integer and incomplete-command rejection"); // Complete the call above with these remaining arguments.

    unsigned codes[5] = {1, 20, GAIN_HALF, GAIN_THREE_QUARTERS, GAIN_FOUR_FIFTHS}; // Initialize codes to {1, 20, GAIN_HALF, GAIN_THREE_QUARTERS, GAIN_FOUR_FIFTHS}.
    for (unsigned code : codes) { // Visit the specified items/indexes in order.
        State state; // Declare state for this operation.
        state.gain = static_cast<uint8_t>(code); // Set state.gain to static_cast<uint8_t>(code).
        state.active_session = 0x01020304; // Set state.active session to 0x01020304.
        auto frame_bytes = make_state_packet(state); // Initialize frame bytes to make_state_packet(state).
        FrameDecoder decoder; // Declare decoder for this operation.
        Frame frame; // Declare frame for this operation.
        // Mimic a TCP read containing only the beginning of a header.
        if (!decoder.append(frame_bytes.data(), 3) || decoder.next(frame)) return 1; // Test the condition !decoder.append(frame_bytes.data(), 3) || decoder.next(frame). If true, return 1 to the caller.
        if (!decoder.append(frame_bytes.data() + 3, frame_bytes.size() - 3) || // Test the condition !decoder.append(frame_bytes.data() + 3, frame_bytes.size() - 3) ||.
            !decoder.next(frame) || frame.type != kState) return 1; // Complete this expression or declaration.
        State result; // Declare result for this operation.
        if (!decode_state(frame.payload, result) || result.gain != code || // Test the condition !decode_state(frame.payload, result) || result.gain != code ||.
            result.active_session != 0x01020304) return 1; // Complete this expression or declaration.
    } // End this block, declaration or argument list.
    if (valid_gain_code(0) || valid_gain_code(99) || valid_gain_code(104)) return 1; // Test the condition valid_gain_code(0) || valid_gain_code(99) || valid_gain_code(104). If true, return 1 to the caller.
    std::puts("PASS: fragmented State frame, counters and supported gain codes"); // Complete the call above with these remaining arguments.

    StatusPacket status; // Declare status for this operation.
    status.storage_mounted = 1; // Set status.storage mounted to 1.
    auto status_bytes = make_status_packet(status); // Initialize status bytes to make_status_packet(status).
    CommandResult outcome; // Declare outcome for this operation.
    outcome.command = CommandType::START_REC; // Set outcome.command to CommandType::START_REC.
    outcome.result = Result::AcceptedPending; // Set outcome.result to Result::AcceptedPending.
    auto result_bytes = make_command_result_packet(outcome); // Initialize result bytes to make_command_result_packet(outcome).
    status_bytes.insert(status_bytes.end(), result_bytes.begin(), result_bytes.end()); // Call status_bytes.insert with the arguments shown here.
    FrameDecoder decoder; // Declare decoder for this operation.
    Frame frame; // Declare frame for this operation.
    if (!decoder.append(status_bytes.data(), status_bytes.size()) || // Test the condition !decoder.append(status_bytes.data(), status_bytes.size()) ||.
        !decoder.next(frame) || frame.type != kStatus || frame.payload.size() != 4 || // Continue this calculation/condition with the next line.
        !decoder.next(frame) || frame.type != kCommandResult) return 1; // Complete this expression or declaration.
    CommandResult reply; // Declare reply for this operation.
    if (!decode_command_result(frame.payload, reply) || reply.result != Result::AcceptedPending || // Test the condition !decode_command_result(frame.payload, reply) || reply.result != Result::AcceptedPending ||.
        decoder.next(frame) || decoder.failed()) return 1; // Call decoder.next with the arguments shown here.
    std::puts("PASS: combined TCP frames, fixed-size StatusPacket and pending CommandResult"); // Complete the call above with these remaining arguments.

    std::vector<FileEntry> entries; // Declare entries for this operation.
    entries.push_back({7, "HEART_20261123_120000_000001.wav"}); // Append the owned item at the back of this container.
    auto archive_bytes = make_file_list_packet(0, entries); // Initialize archive bytes to make_file_list_packet(0, entries).
    FrameDecoder archive_decoder; // Declare archive decoder for this operation.
    if (!archive_decoder.append(archive_bytes.data(), archive_bytes.size()) || // Test the condition !archive_decoder.append(archive_bytes.data(), archive_bytes.size()) ||.
        !archive_decoder.next(frame)) return 1; // Complete this expression or declaration.
    FileListPacket listing; // Declare listing for this operation.
    if (!parse_file_list(frame.payload, listing) || listing.entries.size() != 1 || // Test the condition !parse_file_list(frame.payload, listing) || listing.entries.size() != 1 ||.
        listing.entries[0].id != 7) return 1; // Complete this expression or declaration.
    std::puts("PASS: archive preserves the server's file ID"); // Complete the call above with these remaining arguments.
    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
