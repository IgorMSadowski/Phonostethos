#include "phonostethos/protocol.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <limits>
#include <vector>

using namespace std;

int main() {
    using namespace phono::protocol;

    const uint8_t length_bytes[] = {0, 0, 0x18, 0};
    if (read_u32_be(length_bytes) != 6144) return 1;

    vector<uint8_t> integer;
    append_u32_be(integer, 0x12345678);
    if (integer != vector<uint8_t>{0x12, 0x34, 0x56, 0x78}) {
        return 2;
    }

    CommandPacket command{};
    const array<uint8_t, 5> gain_command{2, 0, 0, 0, 2};
    if (!decode_command(gain_command.data(), gain_command.size(), command) ||
        command.type != Command::SetGain || command.value != 2) {
        return 3;
    }
    if (decode_command(gain_command.data(), gain_command.size() - 1, command)) {
        return 4;
    }

    const auto status = make_status_packet(Status{1, 1, 0, 0});
    if (status != vector<uint8_t>{kStatus, 1, 1, 0, 0}) return 5;

    const auto files = make_file_list_packet(
        0, {{12, "HEART_20261101_143022_000001.wav"}});
    if (files[0] != kFileList || read_u32_be(files.data() + 1) != files.size() - 5) {
        return 6;
    }
    FileList parsed_files;
    FrameDecoder file_decoder;
    if (!file_decoder.append(files.data(), files.size())) return 7;
    Frame file_frame;
    if (!file_decoder.next(file_frame) || file_frame.type != kFileList ||
        !parse_file_list(file_frame.payload, parsed_files) ||
        parsed_files.next_cursor != 0 || parsed_files.entries.size() != 1 ||
        parsed_files.entries[0].id != 12 ||
        parsed_files.entries[0].basename !=
            "HEART_20261101_143022_000001.wav") {
        return 8;
    }

    array<float, kWaveformSamples> waveform{};
    array<float, kSpectrumSamples> spectrum{};
    waveform[0] = 1.0F;
    waveform[1] = -1.0F;
    spectrum[0] = 0.5F;
    const auto data = make_data_packet(waveform, spectrum);
    if (data.size() != kHeaderBytes + kDataPayloadBytes ||
        read_u32_be(data.data() + 1) != kDataPayloadBytes ||
        data[5] != 0x3f || data[6] != 0x80 || data[7] != 0 ||
        data[8] != 0 || data[9] != 0xbf || data[10] != 0x80 ||
        data[11] != 0 || data[12] != 0) {
        return 9;
    }

    State state;
    state.transport = Transport::Recording;
    state.filter = 1;
    state.gain = 3;
    state.storage_flags = StoragePresent | StorageMounted | StorageWritable |
                          StorageSpaceOk;
    state.active_session = 0x12345678;
    state.recording_overruns = 9;
    const auto state_packet = make_state_packet(state);
    if (state_packet.size() != 25 || state_packet[0] != kState ||
        read_u32_be(state_packet.data() + 1) != kStatePayloadBytes) {
        return 10;
    }
    FrameDecoder state_decoder;
    Frame state_frame;
    for (size_t offset = 0; offset < state_packet.size();) {
        const size_t count = offset == 0 ? 2 : 3;
        const auto copied = min(count, state_packet.size() - offset);
        if (!state_decoder.append(state_packet.data() + offset, copied)) return 11;
        offset += copied;
    }
    if (!state_decoder.next(state_frame) || state_frame.type != kState ||
        !decode_state(state_frame.payload, state) ||
        state.transport != Transport::Recording || state.filter != 1 ||
        state.gain != 3 || state.active_session != 0x12345678 ||
        state.recording_overruns != 9) {
        return 12;
    }

    for (const uint8_t gain : {11, 20, 101, 102, 103}) {
        state_frame.payload[3] = gain;
        if (!decode_state(state_frame.payload, state) || state.gain != gain) {
            return 18;
        }
    }
    state_frame.payload[3] = 21;
    if (decode_state(state_frame.payload, state)) return 19;

    const auto result_packet = make_command_result_packet(
        {Command::StartRecording, Result::AcceptedPending, 0});
    if (result_packet.size() != 13 || result_packet[0] != kCommandResult ||
        read_u32_be(result_packet.data() + 1) != 8) {
        return 13;
    }
    FrameDecoder result_decoder;
    Frame result_frame;
    if (!result_decoder.append(result_packet.data(), result_packet.size()) ||
        !result_decoder.next(result_frame)) {
        return 14;
    }
    CommandResult result;
    if (!decode_command_result(result_frame.payload, result) ||
        result.command != Command::StartRecording ||
        result.result != Result::AcceptedPending) {
        return 15;
    }

    const auto invalid_float = numeric_limits<float>::infinity();
    waveform[0] = invalid_float;
    try {
        (void)make_data_packet(waveform, spectrum);
        return 16;
    } catch (const invalid_argument&) {
    }

    const vector<uint8_t> malformed{kState, 0, 0, 0, 19};
    FrameDecoder malformed_decoder;
    Frame malformed_frame;
    if (!malformed_decoder.append(malformed.data(), malformed.size()) ||
        malformed_decoder.next(malformed_frame) || !malformed_decoder.failed()) {
        return 17;
    }

    return 0;
}
