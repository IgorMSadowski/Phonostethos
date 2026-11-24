/**
 * protocol_test.cpp - checks protocol with a small repeatable program.
 *
 * CONNECTIONS: The includes below identify the real project APIs/libraries under test. Its Makefile or app/CMakeLists.txt builds this test separately from the production server. Read the neighboring README before running a hardware test.
 *
 * FUNCTIONS:
 * - main(): run this standalone test and return success only if its checks pass.
 */



#include "phonostethos/protocol.h" // Include the protocol class/types used here; defines and validates the client/server binary protocol.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.
#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.
#include <limits> // Include numeric bounds and IEEE-754 representation checks.
#include <vector> // Include variable-size byte frames and archive entry lists.

using namespace std; // Allow the names in this namespace without repeating its prefix.

int main() { // Run this standalone test and return success only if its checks pass.
    uint32_t parsed_id = 0; // Initialize parsed id to 0.
    if (!phono::protocol::parse_decimal_id("4294967295", parsed_id) || // Test the condition !phono::protocol::parse_decimal_id("4294967295", parsed_id) ||.
        parsed_id != UINT32_MAX) return 30; // Complete this expression or declaration.
    if (phono::protocol::parse_decimal_id("4294967296", parsed_id) || // Test the condition phono::protocol::parse_decimal_id("4294967296", parsed_id) ||.
        phono::protocol::parse_decimal_id("999999999999999999999999", parsed_id) || // Continue this calculation/condition with the next line.
        phono::protocol::parse_decimal_id("-1", parsed_id) || // Continue this calculation/condition with the next line.
        phono::protocol::parse_decimal_id("", parsed_id)) return 31; // Complete this expression or declaration.

    using namespace phono::protocol; // Allow the names in this namespace without repeating its prefix.

    const uint8_t length_bytes[] = {0, 0, 0x18, 0}; // Initialize length bytes to {0, 0, 0x18, 0}.
    if (read_u32_be(length_bytes) != 6144) return 1; // Test the condition read_u32_be(length_bytes) != 6144. If true, return 1 to the caller.

    vector<uint8_t> integer; // Declare integer for this operation.
    append_u32_be(integer, 0x12345678); // Call append_u32_be with the arguments shown here.
    if (integer != vector<uint8_t>{0x12, 0x34, 0x56, 0x78}) { // Test the condition integer != vector<uint8_t>{0x12, 0x34, 0x56, 0x78}.
        return 2; // Return 2 to the caller.
    } // End this block, declaration or argument list.

    CommandPacket command{}; // Declare command for this operation.
    const array<uint8_t, 5> gain_command{2, 0, 0, 0, 2}; // Complete this expression or declaration.
    if (!decode_command(gain_command.data(), gain_command.size(), command) || // Test the condition !decode_command(gain_command.data(), gain_command.size(), command) ||.
        command.cmd_type != CommandType::SET_GAIN || command.value != 2) { // Finish the parameters/condition and enter the block below.
        return 3; // Return 3 to the caller.
    } // End this block, declaration or argument list.
    if (decode_command(gain_command.data(), gain_command.size() - 1, command)) { // Test the condition decode_command(gain_command.data(), gain_command.size() - 1, command).
        return 4; // Return 4 to the caller.
    } // End this block, declaration or argument list.

    const auto status = make_status_packet(StatusPacket{1, 1, 0, 0}); // Initialize status to make_status_packet(StatusPacket{1, 1, 0, 0}).
    if (status != vector<uint8_t>{kStatus, 1, 1, 0, 0}) return 5; // Test the condition status != vector<uint8_t>{kStatus, 1, 1, 0, 0}. If true, return 5 to the caller.

    const auto files = make_file_list_packet( // Initialize files to make_file_list_packet(.
        0, {{12, "HEART_20261101_143022_000001.wav"}}); // Complete the call above with these remaining arguments.
    if (files[0] != kFileList || read_u32_be(files.data() + 1) != files.size() - 5) { // Test the condition files[0] != kFileList || read_u32_be(files.data() + 1) != files.size() - 5.
        return 6; // Return 6 to the caller.
    } // End this block, declaration or argument list.
    FileListPacket parsed_files; // Declare parsed files for this operation.
    FrameDecoder file_decoder; // Declare file decoder for this operation.
    if (!file_decoder.append(files.data(), files.size())) return 7; // Test the condition !file_decoder.append(files.data(), files.size()). If true, return 7 to the caller.
    Frame file_frame; // Declare file frame for this operation.
    if (!file_decoder.next(file_frame) || file_frame.type != kFileList || // Test the condition !file_decoder.next(file_frame) || file_frame.type != kFileList ||.
        !parse_file_list(file_frame.payload, parsed_files) || // Continue this calculation/condition with the next line.
        parsed_files.next_cursor != 0 || parsed_files.entries.size() != 1 || // Continue this calculation/condition with the next line.
        parsed_files.entries[0].id != 12 || // Continue this calculation/condition with the next line.
        parsed_files.entries[0].basename != // Continue the expression or parameter list from the preceding line.
            "HEART_20261101_143022_000001.wav") { // Finish the parameters/condition and enter the block below.
        return 8; // Return 8 to the caller.
    } // End this block, declaration or argument list.

    array<float, kWaveformSamples> waveform{}; // Complete this expression or declaration.
    array<float, kSpectrumSamples> spectrum{}; // Complete this expression or declaration.
    waveform[0] = 1.0F; // Set waveform[0] to 1.0F.
    waveform[1] = -1.0F; // Set waveform[1] to -1.0F.
    spectrum[0] = 0.5F; // Set spectrum[0] to 0.5F.
    const auto data = make_data_packet(waveform, spectrum); // Initialize data to make_data_packet(waveform, spectrum).
    if (data.size() != kHeaderBytes + kDataPayloadBytes || // Test the condition data.size() != kHeaderBytes + kDataPayloadBytes ||.
        read_u32_be(data.data() + 1) != kDataPayloadBytes || // Call read_u32_be with the arguments shown here.
        data[5] != 0x3f || data[6] != 0x80 || data[7] != 0 || // Continue this calculation/condition with the next line.
        data[8] != 0 || data[9] != 0xbf || data[10] != 0x80 || // Continue this calculation/condition with the next line.
        data[11] != 0 || data[12] != 0) { // Finish the parameters/condition and enter the block below.
        return 9; // Return 9 to the caller.
    } // End this block, declaration or argument list.

    State state; // Declare state for this operation.
    state.transport = Transport::Recording; // Set state.transport to Transport::Recording.
    state.filter = 1; // Set state.filter to 1.
    state.gain = 3; // Set state.gain to 3.
    state.storage_flags = StoragePresent | StorageMounted | StorageWritable | // Set state.storage flags to StoragePresent | StorageMounted | StorageWritable |.
                          StorageSpaceOk; // Complete this expression or declaration.
    state.active_session = 0x12345678; // Set state.active session to 0x12345678.
    state.recording_overruns = 9; // Set state.recording overruns to 9.
    const auto state_packet = make_state_packet(state); // Initialize state packet to make_state_packet(state).
    if (state_packet.size() != 25 || state_packet[0] != kState || // Test the condition state_packet.size() != 25 || state_packet[0] != kState ||.
        read_u32_be(state_packet.data() + 1) != kStatePayloadBytes) { // Call read_u32_be with the arguments shown here.
        return 10; // Return 10 to the caller.
    } // End this block, declaration or argument list.
    FrameDecoder state_decoder; // Declare state decoder for this operation.
    Frame state_frame; // Declare state frame for this operation.
    for (size_t offset = 0; offset < state_packet.size();) { // Visit the specified items/indexes in order.
        const size_t count = offset == 0 ? 2 : 3; // Initialize count to offset == 0 ? 2 : 3.
        const auto copied = min(count, state_packet.size() - offset); // Initialize copied to min(count, state_packet.size() - offset).
        if (!state_decoder.append(state_packet.data() + offset, copied)) return 11; // Test the condition !state_decoder.append(state_packet.data() + offset, copied). If true, return 11 to the caller.
        offset += copied; // Update offset using += copied.
    } // End this block, declaration or argument list.
    if (!state_decoder.next(state_frame) || state_frame.type != kState || // Test the condition !state_decoder.next(state_frame) || state_frame.type != kState ||.
        !decode_state(state_frame.payload, state) || // Continue this calculation/condition with the next line.
        state.transport != Transport::Recording || state.filter != 1 || // Continue this calculation/condition with the next line.
        state.gain != 3 || state.active_session != 0x12345678 || // Continue this calculation/condition with the next line.
        state.recording_overruns != 9) { // Finish the parameters/condition and enter the block below.
        return 12; // Return 12 to the caller.
    } // End this block, declaration or argument list.

    for (const uint8_t gain : {11, 20, 101, 102, 103}) { // Visit the specified items/indexes in order.
        state_frame.payload[3] = gain; // Set state frame.payload[3] to gain.
        if (!decode_state(state_frame.payload, state) || state.gain != gain) { // Test the condition !decode_state(state_frame.payload, state) || state.gain != gain.
            return 18; // Return 18 to the caller.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    state_frame.payload[3] = 21; // Set state frame.payload[3] to 21.
    if (decode_state(state_frame.payload, state)) return 19; // Test the condition decode_state(state_frame.payload, state). If true, return 19 to the caller.

    const auto result_packet = make_command_result_packet( // Initialize result packet to make_command_result_packet(.
        {CommandType::START_REC, Result::AcceptedPending, 0}); // Complete the call above with these remaining arguments.
    if (result_packet.size() != 13 || result_packet[0] != kCommandResult || // Test the condition result_packet.size() != 13 || result_packet[0] != kCommandResult ||.
        read_u32_be(result_packet.data() + 1) != 8) { // Call read_u32_be with the arguments shown here.
        return 13; // Return 13 to the caller.
    } // End this block, declaration or argument list.
    FrameDecoder result_decoder; // Declare result decoder for this operation.
    Frame result_frame; // Declare result frame for this operation.
    if (!result_decoder.append(result_packet.data(), result_packet.size()) || // Test the condition !result_decoder.append(result_packet.data(), result_packet.size()) ||.
        !result_decoder.next(result_frame)) { // Finish the parameters/condition and enter the block below.
        return 14; // Return 14 to the caller.
    } // End this block, declaration or argument list.
    CommandResult result; // Declare result for this operation.
    if (!decode_command_result(result_frame.payload, result) || // Test the condition !decode_command_result(result_frame.payload, result) ||.
        result.command != CommandType::START_REC || // Continue this calculation/condition with the next line.
        result.result != Result::AcceptedPending) { // Finish the parameters/condition and enter the block below.
        return 15; // Return 15 to the caller.
    } // End this block, declaration or argument list.

    const auto invalid_float = numeric_limits<float>::infinity(); // Initialize invalid float to numeric_limits<float>::infinity().
    waveform[0] = invalid_float; // Set waveform[0] to invalid_float.
    try { // Catch failures from the operations in this block.
        (void)make_data_packet(waveform, spectrum); // Call  with the arguments shown here.
        return 16; // Return 16 to the caller.
    } catch (const invalid_argument&) { // Handle the stated exception instead of continuing with an invalid result.
    } // End this block, declaration or argument list.

    const vector<uint8_t> malformed{kState, 0, 0, 0, 19}; // Declare malformed for this operation.
    FrameDecoder malformed_decoder; // Declare malformed decoder for this operation.
    Frame malformed_frame; // Declare malformed frame for this operation.
    if (!malformed_decoder.append(malformed.data(), malformed.size()) || // Test the condition !malformed_decoder.append(malformed.data(), malformed.size()) ||.
        malformed_decoder.next(malformed_frame) || !malformed_decoder.failed()) { // Call malformed_decoder.next with the arguments shown here.
        return 17; // Return 17 to the caller.
    } // End this block, declaration or argument list.

    return 0; // Return 0 to the caller.
} // End this block, declaration or argument list.
