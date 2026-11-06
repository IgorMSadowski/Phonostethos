#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string>
#include <vector>

using namespace std;

namespace phono::protocol {

constexpr uint8_t kData = 0x01;
constexpr uint8_t kStatus = 0x02;
constexpr uint8_t kFileList = 0x03;
constexpr uint8_t kState = 0x04;
constexpr uint8_t kCommandResult = 0x05;
constexpr size_t kHeaderBytes = 5;
constexpr size_t kCommandBytes = 5;
constexpr size_t kWaveformSamples = 1024;
constexpr size_t kSpectrumSamples = 512;
constexpr size_t kDataPayloadBytes = (kWaveformSamples + kSpectrumSamples) * sizeof(float);
constexpr size_t kStatusFrameBytes = 5;
constexpr size_t kStatePayloadBytes = 20;
constexpr size_t kCommandResultPayloadBytes = 8;
constexpr size_t kMaxPayloadBytes = 16384;
constexpr size_t kMaxFileListPayloadBytes = 8192;
constexpr size_t kMaxFileListEntries = 32;
constexpr size_t kMaxBasenameBytes = 128;
constexpr uint8_t kProtocolVersion = 1;

enum class Command : uint8_t {
    SetMode = 1,
    SetGain = 2,
    StartRecording = 3,
    StopRecording = 4,
    PlayAudio = 5,
    StopPlayback = 6,
    EjectStorage = 7,
    GetArchive = 8,
    DeleteRecording = 9,
};

enum class Transport : uint8_t {
    Live = 0,
    Recording = 1,
    Playback = 2,
    Stopping = 3,
    Ejecting = 4,
    Error = 5,
};

enum class Result : uint8_t {
    Completed = 0,
    AcceptedPending = 1,
    Rejected = 2,
    Failed = 3,
};

enum class Error : uint8_t {
    None = 0,
    DiskFull = 1,
    StorageLost = 2,
    MicLost = 3,
    OpenError = 4,
    WriteError = 5,
    FinalizeError = 6,
    ReadError = 7,
    UnsupportedWav = 8,
    RecordOverrun = 9,
    NetworkError = 10,
    DspError = 11,
    PowerFault = 12,
    UnmountError = 13,
    ProtocolError = 14,
    OutputError = 15,
    Busy = 16,
    InvalidArgument = 17,
    StaleFileId = 18,
    LedDriverError = 19,
};

enum StorageFlag : uint8_t {
    StoragePresent = 1U << 0,
    StorageMounted = 1U << 1,
    StorageReadable = 1U << 2,
    StorageWritable = 1U << 3,
    StorageSpaceOk = 1U << 4,
};

struct CommandPacket {
    Command type{};
    uint32_t value = 0;
};

struct Status {
    uint8_t storage_mounted = 0;
    uint8_t allow_record = 0;
    uint8_t allow_playback = 0;
    uint8_t error_code = 0;
};

struct State {
    Transport transport = Transport::Live;
    uint8_t filter = 0;
    uint8_t gain = 1;
    uint8_t storage_flags = 0;
    uint8_t error_code = 0;
    uint32_t active_session = 0;
    uint32_t dropped_telemetry = 0;
    uint32_t recording_overruns = 0;
};

struct CommandResult {
    Command command{};
    Result result = Result::Completed;
    uint32_t detail = 0;
};

struct FileEntry {
    uint32_t id = 0;
    string basename;
};

struct FileList {
    uint32_t next_cursor = 0;
    vector<FileEntry> entries;
};

struct Frame {
    uint8_t type = 0;
    vector<uint8_t> payload;
};

inline bool valid_utf8(const string& value);

inline uint32_t read_u32_be(const uint8_t* bytes) {
    return (static_cast<uint32_t>(bytes[0]) << 24) |
           (static_cast<uint32_t>(bytes[1]) << 16) |
           (static_cast<uint32_t>(bytes[2]) << 8) |
           static_cast<uint32_t>(bytes[3]);
}

inline uint16_t read_u16_be(const uint8_t* bytes) {
    return static_cast<uint16_t>(
        (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]);
}

inline void append_u16_be(vector<uint8_t>& out,
                          uint16_t value) {
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

inline void append_u32_be(vector<uint8_t>& out,
                          uint32_t value) {
    out.push_back(static_cast<uint8_t>(value >> 24));
    out.push_back(static_cast<uint8_t>(value >> 16));
    out.push_back(static_cast<uint8_t>(value >> 8));
    out.push_back(static_cast<uint8_t>(value));
}

inline float read_float_be(const uint8_t* bytes) {
    static_assert(sizeof(float) == 4 && numeric_limits<float>::is_iec559,
                  "protocol requires IEEE-754 binary32");
    const uint32_t bits = read_u32_be(bytes);
    float value = 0.0F;
    memcpy(&value, &bits, sizeof(value));
    return value;
}

inline void append_float_be(vector<uint8_t>& out, float value) {
    static_assert(sizeof(float) == 4 && numeric_limits<float>::is_iec559,
                  "protocol requires IEEE-754 binary32");
    uint32_t bits = 0;
    memcpy(&bits, &value, sizeof(bits));
    append_u32_be(out, bits);
}

inline vector<uint8_t> make_command_packet(CommandPacket command) {
    vector<uint8_t> packet;
    packet.reserve(kCommandBytes);
    packet.push_back(static_cast<uint8_t>(command.type));
    append_u32_be(packet, command.value);
    return packet;
}

inline bool decode_command(const uint8_t* bytes, size_t size,
                           CommandPacket& command) {
    if (size != kCommandBytes || bytes[0] < 1 || bytes[0] > 9) return false;
    command.type = static_cast<Command>(bytes[0]);
    command.value = read_u32_be(bytes + 1);
    return true;
}

// Keep the existing integer wire values and reserve three codes for attenuation.
inline constexpr bool valid_gain_code(uint32_t code) {
    return (code >= 1 && code <= 20) || (code >= 101 && code <= 103);
}

inline constexpr float gain_multiplier(uint32_t code) {
    switch (code) {
        case 101: return 0.5F;
        case 102: return 0.75F;
        case 103: return 0.8F;
        default: return valid_gain_code(code) ? static_cast<float>(code) : 1.0F;
    }
}

inline bool valid_command_value(const CommandPacket& command) {
    switch (command.type) {
        case Command::SetMode: return command.value <= 2;
        case Command::SetGain: return valid_gain_code(command.value);
        case Command::PlayAudio:
        case Command::GetArchive:
        case Command::DeleteRecording:
            return true;
        case Command::StartRecording:
        case Command::StopRecording:
        case Command::StopPlayback:
        case Command::EjectStorage:
            return command.value == 0;
    }
    return false;
}

inline vector<uint8_t> make_status_packet(const Status& status) {
    return {kStatus, status.storage_mounted, status.allow_record,
            status.allow_playback, status.error_code};
}

inline vector<uint8_t> make_frame(
    uint8_t type, const vector<uint8_t>& payload);

inline vector<uint8_t> make_state_packet(const State& state) {
    vector<uint8_t> payload;
    payload.reserve(kStatePayloadBytes);
    payload.push_back(kProtocolVersion);
    payload.push_back(static_cast<uint8_t>(state.transport));
    payload.push_back(state.filter);
    payload.push_back(state.gain);
    payload.push_back(state.storage_flags);
    payload.push_back(state.error_code);
    append_u16_be(payload, 0);
    append_u32_be(payload, state.active_session);
    append_u32_be(payload, state.dropped_telemetry);
    append_u32_be(payload, state.recording_overruns);
    return make_frame(kState, payload);
}

inline vector<uint8_t> make_command_result_packet(
    const CommandResult& result) {
    vector<uint8_t> payload;
    payload.reserve(kCommandResultPayloadBytes);
    payload.push_back(static_cast<uint8_t>(result.command));
    payload.push_back(static_cast<uint8_t>(result.result));
    append_u16_be(payload, 0);
    append_u32_be(payload, result.detail);
    return make_frame(kCommandResult, payload);
}

inline vector<uint8_t> make_frame(
    uint8_t type, const vector<uint8_t>& payload) {
    if (type == kStatus || payload.size() > kMaxPayloadBytes) {
        throw invalid_argument("invalid length-prefixed frame");
    }
    vector<uint8_t> packet;
    packet.reserve(kHeaderBytes + payload.size());
    packet.push_back(type);
    append_u32_be(packet, static_cast<uint32_t>(payload.size()));
    packet.insert(packet.end(), payload.begin(), payload.end());
    return packet;
}

inline vector<uint8_t> make_file_list_packet(
    uint32_t next_cursor, const vector<FileEntry>& entries) {
    if (entries.size() > kMaxFileListEntries) {
        throw invalid_argument("too many file-list entries");
    }
    string text = "next_cursor=" + to_string(next_cursor) + "\n";
    for (const auto& entry : entries) {
        if (entry.id == 0 || entry.basename.empty() ||
            entry.basename.size() > kMaxBasenameBytes ||
            !valid_utf8(entry.basename) ||
            entry.basename.size() < 4 ||
            entry.basename.compare(entry.basename.size() - 4, 4, ".wav") != 0 ||
            entry.basename.find_first_of("\t\r\n/\\") != string::npos ||
            entry.basename == "." || entry.basename == "..") {
            throw invalid_argument("invalid archive basename");
        }
        text += to_string(entry.id) + "\t" + entry.basename + "\n";
        if (text.size() > kMaxFileListPayloadBytes) {
            throw invalid_argument("file-list page exceeds payload limit");
        }
    }
    return make_frame(
        kFileList,
        vector<uint8_t>(text.begin(), text.end()));
}

inline bool valid_frame_payload(uint8_t type, size_t length) {
    switch (type) {
        case kData: return length == kDataPayloadBytes;
        case kFileList: return length <= kMaxFileListPayloadBytes;
        case kState: return length == kStatePayloadBytes;
        case kCommandResult: return length == kCommandResultPayloadBytes;
        default: return false;
    }
}

inline bool valid_utf8(const string& value) {
    size_t index = 0;
    while (index < value.size()) {
        const auto first = static_cast<unsigned char>(value[index]);
        size_t continuation_count = 0;
        uint32_t code_point = 0;
        if (first <= 0x7f) {
            ++index;
            continue;
        }
        if (first >= 0xc2 && first <= 0xdf) {
            continuation_count = 1;
            code_point = first & 0x1f;
        } else if (first >= 0xe0 && first <= 0xef) {
            continuation_count = 2;
            code_point = first & 0x0f;
        } else if (first >= 0xf0 && first <= 0xf4) {
            continuation_count = 3;
            code_point = first & 0x07;
        } else {
            return false;
        }
        if (index + continuation_count >= value.size()) return false;
        for (size_t offset = 1; offset <= continuation_count; ++offset) {
            const auto next = static_cast<unsigned char>(value[index + offset]);
            if ((next & 0xc0) != 0x80) return false;
            code_point = (code_point << 6) | (next & 0x3f);
        }
        if ((continuation_count == 2 && code_point < 0x800) ||
            (continuation_count == 3 && code_point < 0x10000) ||
            (code_point >= 0xd800 && code_point <= 0xdfff) ||
            code_point > 0x10ffff) {
            return false;
        }
        index += continuation_count + 1;
    }
    return true;
}

class FrameDecoder {
public:
    bool append(const uint8_t* bytes, size_t size) {
        constexpr size_t kMaxBufferedBytes = 1024 * 1024;
        if (size > kMaxBufferedBytes ||
            buffer_.size() > kMaxBufferedBytes - size) {
            failed_ = true;
            return false;
        }
        buffer_.insert(buffer_.end(), bytes, bytes + size);
        return true;
    }

    bool next(Frame& frame) {
        if (failed_ || buffer_.empty()) return false;
        const auto type = buffer_[0];
        if (type == kStatus) {
            if (buffer_.size() < kStatusFrameBytes) return false;
            frame.type = type;
            frame.payload.assign(buffer_.begin() + 1,
                                 buffer_.begin() + kStatusFrameBytes);
            buffer_.erase(buffer_.begin(),
                          buffer_.begin() + kStatusFrameBytes);
            return true;
        }
        if (buffer_.size() < kHeaderBytes) return false;
        const auto length = read_u32_be(buffer_.data() + 1);
        if (length > kMaxPayloadBytes || !valid_frame_payload(type, length)) {
            failed_ = true;
            return false;
        }
        const auto frame_size = kHeaderBytes + static_cast<size_t>(length);
        if (buffer_.size() < frame_size) return false;
        frame.type = type;
        frame.payload.assign(buffer_.begin() + kHeaderBytes,
                             buffer_.begin() + frame_size);
        buffer_.erase(buffer_.begin(), buffer_.begin() + frame_size);
        return true;
    }

    bool failed() const { return failed_; }
    size_t buffered_bytes() const { return buffer_.size(); }

private:
    vector<uint8_t> buffer_;
    bool failed_ = false;
};

inline bool parse_file_list(const vector<uint8_t>& payload,
                            FileList& result) {
    if (payload.size() > kMaxFileListPayloadBytes) return false;
    const string text(payload.begin(), payload.end());
    if (!valid_utf8(text)) return false;
    const auto first_newline = text.find('\n');
    if (first_newline == string::npos ||
        text.compare(0, 12, "next_cursor=") != 0) {
        return false;
    }
    try {
        const auto cursor_text = text.substr(12, first_newline - 12);
        if (cursor_text.empty() ||
            !all_of(cursor_text.begin(), cursor_text.end(),
                         [](unsigned char digit) {
                             return digit >= '0' && digit <= '9';
                         })) {
            return false;
        }
        size_t consumed = 0;
        const auto cursor = stoull(cursor_text, &consumed);
        if (consumed != cursor_text.size() ||
            cursor > numeric_limits<uint32_t>::max()) {
            return false;
        }
        result.next_cursor = static_cast<uint32_t>(cursor);
    } catch (const exception&) {
        return false;
    }

    result.entries.clear();
    vector<uint32_t> seen_ids;
    size_t start = first_newline + 1;
    while (start < text.size()) {
        const auto end = text.find('\n', start);
        if (end == string::npos) return false;
        const auto tab = text.find('\t', start);
        if (tab == string::npos || tab >= end) return false;
        uint32_t id = 0;
        try {
            const auto id_text = text.substr(start, tab - start);
            if (id_text.empty() ||
                !all_of(id_text.begin(), id_text.end(),
                             [](unsigned char digit) {
                                 return digit >= '0' && digit <= '9';
                             })) {
                return false;
            }
            size_t consumed = 0;
            const auto parsed = stoull(id_text, &consumed);
            if (parsed == 0 || consumed != id_text.size() ||
                parsed > numeric_limits<uint32_t>::max()) {
                return false;
            }
            id = static_cast<uint32_t>(parsed);
        } catch (const exception&) {
            return false;
        }
        const auto name = text.substr(tab + 1, end - tab - 1);
        if (name.empty() || name.size() > kMaxBasenameBytes ||
            name.find_first_of("\t\r/\\") != string::npos ||
            name == "." || name == ".." ||
            result.entries.size() == kMaxFileListEntries ||
            find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end()) {
            return false;
        }
        seen_ids.push_back(id);
        result.entries.push_back({id, name});
        start = end + 1;
    }
    return true;
}

inline bool decode_state(const vector<uint8_t>& payload,
                         State& state) {
    if (payload.size() != kStatePayloadBytes ||
        payload[0] != kProtocolVersion || payload[1] > 5 ||
        payload[2] > 2 || !valid_gain_code(payload[3]) ||
        payload[4] > 0x1f || payload[5] > 19 ||
        read_u16_be(payload.data() + 6) != 0) {
        return false;
    }
    state.transport = static_cast<Transport>(payload[1]);
    state.filter = payload[2];
    state.gain = payload[3];
    state.storage_flags = payload[4];
    state.error_code = payload[5];
    state.active_session = read_u32_be(payload.data() + 8);
    state.dropped_telemetry = read_u32_be(payload.data() + 12);
    state.recording_overruns = read_u32_be(payload.data() + 16);
    return true;
}

inline bool decode_command_result(const vector<uint8_t>& payload,
                                  CommandResult& result) {
    if (payload.size() != kCommandResultPayloadBytes ||
        payload[0] < 1 || payload[0] > 9 || payload[1] > 3 ||
        read_u16_be(payload.data() + 2) != 0) {
        return false;
    }
    result.command = static_cast<Command>(payload[0]);
    result.result = static_cast<Result>(payload[1]);
    result.detail = read_u32_be(payload.data() + 4);
    return true;
}

inline vector<uint8_t> make_data_packet(
    const array<float, kWaveformSamples>& waveform,
    const array<float, kSpectrumSamples>& spectrum) {
    vector<uint8_t> payload;
    payload.reserve(kDataPayloadBytes);
    for (const auto sample : waveform) {
        if (!isfinite(sample)) {
            throw invalid_argument("non-finite waveform sample");
        }
        append_float_be(payload, sample);
    }
    for (const auto magnitude : spectrum) {
        if (!isfinite(magnitude) || magnitude < 0.0F) {
            throw invalid_argument("invalid spectrum magnitude");
        }
        append_float_be(payload, magnitude);
    }
    return make_frame(kData, payload);
}

}  // namespace phono::protocol
