// Test the actual C++ wire helpers; no socket, microphone or GUI is required.
#include "phonostethos/protocol.h"
#include <cstdio>

using namespace phono::protocol;

int main() {
    CommandPacket requested;
    requested.type = Command::SetGain;
    requested.value = GAIN_HALF;
    auto bytes = make_command_packet(requested);
    uint8_t expected[5] = {2, 0, 0, 0, 101};
    if (bytes.size() != 5) return 1;
    for (int i = 0; i < 5; i++) {
        if (bytes[i] != expected[i]) return 1;
    }
    CommandPacket decoded;
    if (!decode_command(bytes.data(), bytes.size(), decoded) ||
        decoded.value != requested.value || decoded.type != requested.type) return 1;
    if (decode_command(bytes.data(), 4, decoded)) return 1;
    std::puts("PASS: five-byte command, big-endian integer and incomplete-command rejection");

    unsigned codes[5] = {1, 20, GAIN_HALF, GAIN_THREE_QUARTERS, GAIN_FOUR_FIFTHS};
    for (unsigned code : codes) {
        State state;
        state.gain = static_cast<uint8_t>(code);
        state.active_session = 0x01020304;
        auto frame_bytes = make_state_packet(state);
        FrameDecoder decoder;
        Frame frame;
        // Mimic a TCP read containing only the beginning of a header.
        if (!decoder.append(frame_bytes.data(), 3) || decoder.next(frame)) return 1;
        if (!decoder.append(frame_bytes.data() + 3, frame_bytes.size() - 3) ||
            !decoder.next(frame) || frame.type != kState) return 1;
        State result;
        if (!decode_state(frame.payload, result) || result.gain != code ||
            result.active_session != 0x01020304) return 1;
    }
    if (valid_gain_code(0) || valid_gain_code(99) || valid_gain_code(104)) return 1;
    std::puts("PASS: fragmented State frame, counters and supported gain codes");

    Status status;
    status.storage_mounted = 1;
    auto status_bytes = make_status_packet(status);
    CommandResult outcome;
    outcome.command = Command::StartRecording;
    outcome.result = Result::AcceptedPending;
    auto result_bytes = make_command_result_packet(outcome);
    status_bytes.insert(status_bytes.end(), result_bytes.begin(), result_bytes.end());
    FrameDecoder decoder;
    Frame frame;
    if (!decoder.append(status_bytes.data(), status_bytes.size()) ||
        !decoder.next(frame) || frame.type != kStatus || frame.payload.size() != 4 ||
        !decoder.next(frame) || frame.type != kCommandResult) return 1;
    CommandResult reply;
    if (!decode_command_result(frame.payload, reply) || reply.result != Result::AcceptedPending ||
        decoder.next(frame) || decoder.failed()) return 1;
    std::puts("PASS: combined TCP frames, fixed-size Status and pending CommandResult");

    std::vector<FileEntry> entries;
    entries.push_back({7, "HEART_20261123_120000_000001.wav"});
    auto archive_bytes = make_file_list_packet(0, entries);
    FrameDecoder archive_decoder;
    if (!archive_decoder.append(archive_bytes.data(), archive_bytes.size()) ||
        !archive_decoder.next(frame)) return 1;
    FileList listing;
    if (!parse_file_list(frame.payload, listing) || listing.entries.size() != 1 ||
        listing.entries[0].id != 7) return 1;
    std::puts("PASS: archive preserves the server's file ID");
    return 0;
}
