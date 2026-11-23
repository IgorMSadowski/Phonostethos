/**
 * protocol.h - defines and validates the client/server binary protocol.
 *
 * CONNECTIONS: The C++ header is used by all server managers; Dart protocol.dart implements the same byte layouts for the GUI. In-memory records omit framing bytes that serialization adds explicitly. Gain codes 101/102/103 identify 0.5x/0.75x/0.8x; they are not multipliers themselves.
 *
 * FUNCTIONS:
 * - valid_utf8(): check utf8 before the value is accepted.
 * - read_u32_be(): read u32 be from the supplied input.
 * - read_u16_be(): read u16 be from the supplied input.
 * - append_u16_be(): append u16 be to the output byte vector.
 * - append_u32_be(): append u32 be to the output byte vector.
 * - read_float_be(): read float be from the supplied input.
 * - append_float_be(): append float be to the output byte vector.
 * - make_command_packet(): serialize command packet with the explicit network layout.
 * - decode_command(): validate and decode command from received bytes.
 * - valid_gain_code(): check gain code before the value is accepted.
 * - gain_multiplier(): translate a valid integer wire code into its real floating-point volume multiplier.
 * - valid_command_value(): check command value before the value is accepted.
 * - make_status_packet(): serialize status packet with the explicit network layout.
 * - make_frame(): serialize frame with the explicit network layout.
 * - make_state_packet(): serialize state packet with the explicit network layout.
 * - make_command_result_packet(): serialize command result packet with the explicit network layout.
 * - make_file_list_packet(): serialize file list packet with the explicit network layout.
 * - valid_frame_payload(): check frame payload before the value is accepted.
 * - append(): retain received bytes until a complete frame can be parsed.
 * - next(): extract and validate one complete buffered reply frame.
 * - failed(): report whether malformed input has put the decoder into a failed state.
 * - buffered_bytes(): return the received byte count still waiting to form a complete frame.
 * - parse_decimal_id(): validate unsigned decimal digits without overflow.
 * - parse_file_list(): decode validated archive IDs, basenames and the next-page cursor.
 * - decode_state(): validate and decode state from received bytes.
 * - decode_command_result(): validate and decode command result from received bytes.
 * - make_data_packet(): serialize data packet with the explicit network layout.
 */

#pragma once // Compile these declarations once per translation unit, even if included repeatedly.

#include <arpa/inet.h> // Include ntohl, the POSIX network-to-host conversion shown in th_Network.

#include <algorithm> // Include min/max, copying, sorting and other standard algorithms used in this file.
#include <array> // Include fixed-size sample and byte arrays whose capacity is known at compile time.
#include <cmath> // Include trigonometry, square roots, logarithms and finite-value checks.
#include <cstddef> // Include size_t for sizes and array indexes.
#include <cstdint> // Include integer types with exact bit widths used by PCM and the binary protocol.
#include <cstring> // Include byte copying/comparison and readable system-error strings.
#include <limits> // Include numeric bounds and IEEE-754 representation checks.
#include <stdexcept> // Include protocol-validation exceptions.
#include <string> // Include owned filenames, device paths and diagnostic text.
#include <vector> // Include variable-size byte frames and archive entry lists.

using namespace std; // Allow the names in this namespace without repeating its prefix.

namespace phono::protocol { // Keep these declarations in the project namespace and avoid global name collisions.

constexpr uint8_t kData = 0x01; // Wire type 0x01 identifies a waveform/FFT frame.
constexpr uint8_t kStatus = 0x02; // Wire type 0x02 identifies the fixed five-byte capability frame.
constexpr uint8_t kFileList = 0x03; // Wire type 0x03 identifies an archive page.
constexpr uint8_t kState = 0x04; // Wire type 0x04 identifies authoritative server state.
constexpr uint8_t kCommandResult = 0x05; // Wire type 0x05 identifies a command acknowledgement/result.
constexpr size_t kHeaderBytes = 5; // A framed reply has a one-byte type followed by a four-byte payload length.
constexpr size_t kCommandBytes = 5; // Every GUI command has exactly one type byte and four big-endian argument bytes.
constexpr size_t kWaveformSamples = 1024; // Initialize k Waveform Samples to 1024.
constexpr size_t kSpectrumSamples = 512; // Initialize k Spectrum Samples to 512.
constexpr size_t kDataPayloadBytes = (kWaveformSamples + kSpectrumSamples) * sizeof(float); // Initialize k Data Payload Bytes to (kWaveformSamples + kSpectrumSamples) * sizeof(float).
constexpr size_t kStatusFrameBytes = 5; // Status is a special fixed five-byte frame with no length field.
constexpr size_t kStatePayloadBytes = 20; // The versioned State payload contains 20 bytes, excluding its five-byte header.
constexpr size_t kCommandResultPayloadBytes = 8; // CommandResult contains eight payload bytes, excluding framing.
constexpr size_t kMaxPayloadBytes = 16384; // Reject an oversized payload before retaining/decoding more data.
constexpr size_t kMaxFileListPayloadBytes = 8192; // Bound the encoded archive page so it fits one control frame.
constexpr size_t kMaxFileListEntries = 32; // Send at most 32 file-ID/name records in one archive page.
constexpr size_t kMaxBasenameBytes = 128; // Limit each encoded basename to 128 UTF-8 bytes.
constexpr uint8_t kProtocolVersion = 1; // State version 1; the GUI rejects unsupported layouts.

// SET_GAIN has an unsigned integer value and State stores the gain in one byte.
// The original codes 1..20 already mean 1x..20x. They cannot represent 0.5/0.75/0.8.
// We reserve unused codes 101..103 for those three choices without changing any
// packet layout. They are identifiers, NOT amplification factors of 101x..103x.
// gain_multiplier() below translates them to 0.5f, 0.75f and 0.8f before DSP.
// The GUI uses the same codes but displays the actual multipliers on the slider.
constexpr uint8_t GAIN_HALF = 101;           // 0.5x volume.
constexpr uint8_t GAIN_THREE_QUARTERS = 102; // 0.75x volume.
constexpr uint8_t GAIN_FOUR_FIFTHS = 103;    // 0.8x volume.


enum class CommandType : uint8_t { // Give the following named choices distinct typed values.
    SET_MODE = 1, // Set SET MODE to 1,.
    SET_GAIN = 2, // Set SET GAIN to 2,.
    START_REC = 3, // Set START REC to 3,.
    STOP_REC = 4, // Set STOP REC to 4,.
    PLAY_AUDIO = 5, // Set PLAY AUDIO to 5,.
    STOP_PLAYBACK = 6, // Set STOP PLAYBACK to 6,.
    EJECT_USB = 7, // Set EJECT USB to 7,.
    GET_ARCHIVE = 8, // Set GET ARCHIVE to 8,.
    DELETE_RECORDING = 9, // Set DELETE RECORDING to 9,.
}; // End this block, declaration or argument list.

enum class Transport : uint8_t { // Give the following named choices distinct typed values.
    Live = 0, // Set Live to 0,.
    Recording = 1, // Set Recording to 1,.
    Playback = 2, // Set Playback to 2,.
    Stopping = 3, // Set Stopping to 3,.
    Ejecting = 4, // Set Ejecting to 4,.
    Error = 5, // Set Error to 5,.
}; // End this block, declaration or argument list.

enum class Result : uint8_t { // Give the following named choices distinct typed values.
    Completed = 0, // Set Completed to 0,.
    AcceptedPending = 1, // Set Accepted Pending to 1,.
    Rejected = 2, // Set Rejected to 2,.
    Failed = 3, // Set Failed to 3,.
}; // End this block, declaration or argument list.

enum class Error : uint8_t { // Give the following named choices distinct typed values.
    None = 0, // Set None to 0,.
    DiskFull = 1, // Set Disk Full to 1,.
    StorageLost = 2, // Set Storage Lost to 2,.
    MicLost = 3, // Set Mic Lost to 3,.
    OpenError = 4, // Set Open Error to 4,.
    WriteError = 5, // Set Write Error to 5,.
    FinalizeError = 6, // Set Finalize Error to 6,.
    ReadError = 7, // Set Read Error to 7,.
    UnsupportedWav = 8, // Set Unsupported Wav to 8,.
    RecordOverrun = 9, // Set Record Overrun to 9,.
    NetworkError = 10, // Set Network Error to 10,.
    DspError = 11, // Set Dsp Error to 11,.
    PowerFault = 12, // Set Power Fault to 12,.
    UnmountError = 13, // Set Unmount Error to 13,.
    ProtocolError = 14, // Set Protocol Error to 14,.
    OutputError = 15, // Set Output Error to 15,.
    Busy = 16, // Set Busy to 16,.
    InvalidArgument = 17, // Set Invalid Argument to 17,.
    StaleFileId = 18, // Set Stale File Id to 18,.
    LedDriverError = 19, // Set Led Driver Error to 19,.
}; // End this block, declaration or argument list.

enum StorageFlag : uint8_t { // Give the following named choices distinct typed values.
    StoragePresent = 1U << 0, // Set Storage Present to 1U << 0,.
    StorageMounted = 1U << 1, // Set Storage Mounted to 1U << 1,.
    StorageReadable = 1U << 2, // Set Storage Readable to 1U << 2,.
    StorageWritable = 1U << 3, // Set Storage Writable to 1U << 3,.
    StorageSpaceOk = 1U << 4, // Set Storage Space Ok to 1U << 4,.
}; // End this block, declaration or argument list.

struct CommandPacket { // Group the related fields in one plain data record.
    CommandType cmd_type{}; // Declare cmd type for this operation.
    uint32_t value = 0; // Initialize value to 0.
}; // End this block, declaration or argument list.

struct StatusPacket { // Group the related fields in one plain data record.
    uint8_t storage_mounted = 0; // Initialize storage mounted to 0.
    uint8_t allow_record = 0; // Initialize allow record to 0.
    uint8_t allow_playback = 0; // Initialize allow playback to 0.
    uint8_t error_code = 0; // Initialize error code to 0.
}; // End this block, declaration or argument list.

struct State { // Group the related fields in one plain data record.
    Transport transport = Transport::Live; // Initialize transport to Transport::Live.
    uint8_t filter = 0; // Initialize filter to 0.
    uint8_t gain = 1; // Initialize gain to 1.
    uint8_t storage_flags = 0; // Initialize storage flags to 0.
    uint8_t error_code = 0; // Initialize error code to 0.
    uint32_t active_session = 0; // Initialize active session to 0.
    uint32_t dropped_telemetry = 0; // Initialize dropped telemetry to 0.
    uint32_t recording_overruns = 0; // Initialize recording overruns to 0.
}; // End this block, declaration or argument list.

struct CommandResult { // Group the related fields in one plain data record.
    CommandType command{}; // Declare command for this operation.
    Result result = Result::Completed; // Initialize result to Result::Completed.
    uint32_t detail = 0; // Initialize detail to 0.
}; // End this block, declaration or argument list.

struct FileEntry { // Group the related fields in one plain data record.
    uint32_t id = 0; // Initialize id to 0.
    string basename; // Declare basename for this operation.
}; // End this block, declaration or argument list.

struct FileListPacket { // Group the related fields in one plain data record.
    uint32_t next_cursor = 0; // Initialize next cursor to 0.
    vector<FileEntry> entries; // Declare entries for this operation.
}; // End this block, declaration or argument list.

struct Frame { // Group the related fields in one plain data record.
    uint8_t type = 0; // Initialize type to 0.
    vector<uint8_t> payload; // Declare payload for this operation.
}; // End this block, declaration or argument list.

inline bool valid_utf8(const string& value); // Complete the call above with these remaining arguments.

inline uint32_t read_u32_be(const uint8_t* bytes) { // Decode the four-byte network-order value named in the Network flowchart.
    uint32_t network_value = 0; // Local storage; the incoming byte address need not be aligned.
    /* API: memcpy(destination, source, byte_count)
     * Copy the four bytes into an aligned integer without dereferencing an
     * unaligned integer pointer or violating the C++ aliasing rules.
     */
    memcpy(&network_value, bytes, sizeof(network_value)); // Preserve the received bit pattern.
    /* API: ntohl(network_value)
     * Convert an unsigned 32-bit integer from network (big-endian) byte order
     * to this CPU's byte order. It is a pure conversion and reports no error.
     */
    return ntohl(network_value); // Return the same numeric value on either CPU byte order.
} // End the big-endian 32-bit decoder. // End this block, declaration or argument list.

inline uint16_t read_u16_be(const uint8_t* bytes) { // Finish the parameters/condition and enter the block below.
    return static_cast<uint16_t>( // Return static_cast<uint16_t>( to the caller.
        (static_cast<uint16_t>(bytes[0]) << 8) | bytes[1]); // Complete the call above with these remaining arguments.
} // End this block, declaration or argument list.

inline void append_u16_be(vector<uint8_t>& out, // Pass this next argument/value to the call or initializer above.
                          uint16_t value) { // Declare value for this operation.
    out.push_back(static_cast<uint8_t>(value >> 8)); // Append the owned item at the back of this container.
    out.push_back(static_cast<uint8_t>(value)); // Append the owned item at the back of this container.
} // End this block, declaration or argument list.

inline void append_u32_be(vector<uint8_t>& out, // Pass this next argument/value to the call or initializer above.
                          uint32_t value) { // Declare value for this operation.
    out.push_back(static_cast<uint8_t>(value >> 24)); // Append the owned item at the back of this container.
    out.push_back(static_cast<uint8_t>(value >> 16)); // Append the owned item at the back of this container.
    out.push_back(static_cast<uint8_t>(value >> 8)); // Append the owned item at the back of this container.
    out.push_back(static_cast<uint8_t>(value)); // Append the owned item at the back of this container.
} // End this block, declaration or argument list.

// Read the IEEE-754 bit pattern without converting the integer numerically.
// memcpy copies the bits safely; a float pointer cast would not do that reliably.
inline float read_float_be(const uint8_t* bytes) { // Finish the parameters/condition and enter the block below.
    static_assert(sizeof(float) == 4 && numeric_limits<float>::is_iec559, // Call static_assert with the arguments shown here.
                  "protocol requires IEEE-754 binary32"); // Complete the call above with these remaining arguments.
    const uint32_t bits = read_u32_be(bytes); // Initialize bits to read_u32_be(bytes).
    float value = 0.0F; // Initialize value to 0.0F.
    memcpy(&value, &bits, sizeof(value)); // Call memcpy with the arguments shown here.
    return value; // Return value to the caller.
} // End this block, declaration or argument list.

inline void append_float_be(vector<uint8_t>& out, float value) { // Finish the parameters/condition and enter the block below.
    static_assert(sizeof(float) == 4 && numeric_limits<float>::is_iec559, // Call static_assert with the arguments shown here.
                  "protocol requires IEEE-754 binary32"); // Complete the call above with these remaining arguments.
    uint32_t bits = 0; // Initialize bits to 0.
    memcpy(&bits, &value, sizeof(bits)); // Call memcpy with the arguments shown here.
    append_u32_be(out, bits); // Call append_u32_be with the arguments shown here.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_command_packet(CommandPacket command) { // Finish the parameters/condition and enter the block below.
    vector<uint8_t> packet; // Declare packet for this operation.
    packet.reserve(kCommandBytes); // Call packet.reserve with the arguments shown here.
    packet.push_back(static_cast<uint8_t>(command.cmd_type)); // Append the owned item at the back of this container.
    append_u32_be(packet, command.value); // Call append_u32_be with the arguments shown here.
    return packet; // Return packet to the caller.
} // End this block, declaration or argument list.

inline bool decode_command(const uint8_t* bytes, size_t size, // Pass this next argument/value to the call or initializer above.
                           CommandPacket& command) { // Declare command for this operation.
    if (size != kCommandBytes || bytes[0] < 1 || bytes[0] > 9) return false; // Test the condition size != kCommandBytes || bytes[0] < 1 || bytes[0] > 9. If true, report failure or no available item to the caller.
    command.cmd_type = static_cast<CommandType>(bytes[0]); // Set command.cmd type to static_cast<CommandType>(bytes[0]).
    command.value = read_u32_be(bytes + 1); // Set command.value to read_u32_be(bytes + 1).
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

// Keep the existing integer wire values and reserve three codes for attenuation.
inline constexpr bool valid_gain_code(uint32_t code) { // Finish the parameters/condition and enter the block below.
    return (code >= 1 && code <= 20) || code == GAIN_HALF || // Return (code >= 1 && code <= 20) || code == GAIN_HALF || to the caller.
           code == GAIN_THREE_QUARTERS || code == GAIN_FOUR_FIFTHS; // Continue checking the file/state comparison; no value is assigned here.
} // End this block, declaration or argument list.

inline constexpr float gain_multiplier(uint32_t code) { // Finish the parameters/condition and enter the block below.
    switch (code) { // Choose one branch from the supplied command/state value.
        case GAIN_HALF: return 0.5F; // Handle the GAIN_HALF choice in this switch.
        case GAIN_THREE_QUARTERS: return 0.75F; // Handle the GAIN_THREE_QUARTERS choice in this switch.
        case GAIN_FOUR_FIFTHS: return 0.8F; // Handle the GAIN_FOUR_FIFTHS choice in this switch.
        default: // Handle values not matched by an earlier case.
            if (valid_gain_code(code)) return static_cast<float>(code); // Test the condition valid_gain_code(code). If true, return static_cast<float>(code) to the caller.
            return 1.0F; // Return 1.0F to the caller.
    } // End this block, declaration or argument list.
} // End this block, declaration or argument list.

inline bool valid_command_value(const CommandPacket& command) { // Finish the parameters/condition and enter the block below.
    switch (command.cmd_type) { // Choose one branch from the supplied command/state value.
        case CommandType::SET_MODE: return command.value <= 2; // Handle the CommandType::SET_MODE choice in this switch.
        case CommandType::SET_GAIN: return valid_gain_code(command.value); // Handle the CommandType::SET_GAIN choice in this switch.
        case CommandType::PLAY_AUDIO: // Handle the CommandType::PLAY_AUDIO choice in this switch.
        case CommandType::GET_ARCHIVE: // Handle the CommandType::GET_ARCHIVE choice in this switch.
        case CommandType::DELETE_RECORDING: // Handle the CommandType::DELETE_RECORDING choice in this switch.
            return true; // Report success, readiness or an available result to the caller.
        case CommandType::START_REC: // Handle the CommandType::START_REC choice in this switch.
        case CommandType::STOP_REC: // Handle the CommandType::STOP_REC choice in this switch.
        case CommandType::STOP_PLAYBACK: // Handle the CommandType::STOP_PLAYBACK choice in this switch.
        case CommandType::EJECT_USB: // Handle the CommandType::EJECT_USB choice in this switch.
            return command.value == 0; // Return command.value == 0 to the caller.
    } // End this block, declaration or argument list.
    return false; // Report failure or no available item to the caller.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_status_packet(const StatusPacket& status) { // Finish the parameters/condition and enter the block below.
    return {kStatus, status.storage_mounted, status.allow_record, // Return {kStatus, status.storage_mounted, status.allow_record, to the caller.
            status.allow_playback, status.error_code}; // Complete this expression or declaration.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_frame( // Begin this function/widget call; its arguments follow below.
    uint8_t type, const vector<uint8_t>& payload); // Declare type for this operation.

inline vector<uint8_t> make_state_packet(const State& state) { // Finish the parameters/condition and enter the block below.
    vector<uint8_t> payload; // Declare payload for this operation.
    payload.reserve(kStatePayloadBytes); // Call payload.reserve with the arguments shown here.
    payload.push_back(kProtocolVersion); // Append the owned item at the back of this container.
    payload.push_back(static_cast<uint8_t>(state.transport)); // Append the owned item at the back of this container.
    payload.push_back(state.filter); // Append the owned item at the back of this container.
    payload.push_back(state.gain); // Append the owned item at the back of this container.
    payload.push_back(state.storage_flags); // Append the owned item at the back of this container.
    payload.push_back(state.error_code); // Append the owned item at the back of this container.
    append_u16_be(payload, 0); // Call append_u16_be with the arguments shown here.
    append_u32_be(payload, state.active_session); // Call append_u32_be with the arguments shown here.
    append_u32_be(payload, state.dropped_telemetry); // Call append_u32_be with the arguments shown here.
    append_u32_be(payload, state.recording_overruns); // Call append_u32_be with the arguments shown here.
    return make_frame(kState, payload); // Return make_frame(kState, payload) to the caller.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_command_result_packet( // Begin this function/widget call; its arguments follow below.
    const CommandResult& result) { // Declare result for this operation.
    vector<uint8_t> payload; // Declare payload for this operation.
    payload.reserve(kCommandResultPayloadBytes); // Call payload.reserve with the arguments shown here.
    payload.push_back(static_cast<uint8_t>(result.command)); // Append the owned item at the back of this container.
    payload.push_back(static_cast<uint8_t>(result.result)); // Append the owned item at the back of this container.
    append_u16_be(payload, 0); // Call append_u16_be with the arguments shown here.
    append_u32_be(payload, result.detail); // Call append_u32_be with the arguments shown here.
    return make_frame(kCommandResult, payload); // Return make_frame(kCommandResult, payload) to the caller.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_frame( // Begin this function/widget call; its arguments follow below.
    uint8_t type, const vector<uint8_t>& payload) { // Declare type for this operation.
    if (type == kStatus || payload.size() > kMaxPayloadBytes) { // Test the condition type == kStatus || payload.size() > kMaxPayloadBytes.
        throw invalid_argument("invalid length-prefixed frame"); // Reject the invalid input by reporting this exception.
    } // End this block, declaration or argument list.
    vector<uint8_t> packet; // Declare packet for this operation.
    packet.reserve(kHeaderBytes + payload.size()); // Call packet.reserve with the arguments shown here.
    packet.push_back(type); // Append the owned item at the back of this container.
    append_u32_be(packet, static_cast<uint32_t>(payload.size())); // Call append_u32_be with the arguments shown here.
    packet.insert(packet.end(), payload.begin(), payload.end()); // Call packet.insert with the arguments shown here.
    return packet; // Return packet to the caller.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_file_list_packet( // Begin this function/widget call; its arguments follow below.
    uint32_t next_cursor, const vector<FileEntry>& entries) { // Declare next cursor for this operation.
    if (entries.size() > kMaxFileListEntries) { // Test the condition entries.size() > kMaxFileListEntries.
        throw invalid_argument("too many file-list entries"); // Reject the invalid input by reporting this exception.
    } // End this block, declaration or argument list.
    string text = "next_cursor=" + to_string(next_cursor) + "\n"; // Initialize text to "next_cursor=" + to_string(next_cursor) + "\n".
    for (const auto& entry : entries) { // Visit the specified items/indexes in order.
        if (entry.id == 0 || entry.basename.empty() || // Test the condition entry.id == 0 || entry.basename.empty() ||.
            entry.basename.size() > kMaxBasenameBytes || // Continue this calculation/condition with the next line.
            !valid_utf8(entry.basename) || // Continue this calculation/condition with the next line.
            entry.basename.size() < 4 || // Continue this calculation/condition with the next line.
            entry.basename.compare(entry.basename.size() - 4, 4, ".wav") != 0 || // Continue this calculation/condition with the next line.
            entry.basename.find_first_of("\t\r\n/\\") != string::npos || // Continue this calculation/condition with the next line.
            entry.basename == "." || entry.basename == "..") { // Continue checking the file/state comparison; no value is assigned here.
            throw invalid_argument("invalid archive basename"); // Reject the invalid input by reporting this exception.
        } // End this block, declaration or argument list.
        text += to_string(entry.id) + "\t" + entry.basename + "\n"; // Update text using += to_string(entry.id) + "\t" + entry.basename + "\n".
        if (text.size() > kMaxFileListPayloadBytes) { // Test the condition text.size() > kMaxFileListPayloadBytes.
            throw invalid_argument("file-list page exceeds payload limit"); // Reject the invalid input by reporting this exception.
        } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return make_frame( // Return make_frame( to the caller.
        kFileList, // Name this field, choice or argument: k File List.
        vector<uint8_t>(text.begin(), text.end())); // Complete the call above with these remaining arguments.
} // End this block, declaration or argument list.

inline bool valid_frame_payload(uint8_t type, size_t length) { // Finish the parameters/condition and enter the block below.
    switch (type) { // Choose one branch from the supplied command/state value.
        case kData: return length == kDataPayloadBytes; // Handle the kData choice in this switch.
        case kFileList: return length <= kMaxFileListPayloadBytes; // Handle the kFileList choice in this switch.
        case kState: return length == kStatePayloadBytes; // Handle the kState choice in this switch.
        case kCommandResult: return length == kCommandResultPayloadBytes; // Handle the kCommandResult choice in this switch.
        default: return false; // Handle values not matched by an earlier case.
    } // End this block, declaration or argument list.
} // End this block, declaration or argument list.

inline bool valid_utf8(const string& value) { // Finish the parameters/condition and enter the block below.
    size_t index = 0; // Initialize index to 0.
    while (index < value.size()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        const auto first = static_cast<unsigned char>(value[index]); // Initialize first to static_cast<unsigned char>(value[index]).
        size_t continuation_count = 0; // Initialize continuation count to 0.
        uint32_t code_point = 0; // Initialize code point to 0.
        if (first <= 0x7f) { // Test the condition first <= 0x7f.
            ++index; // Advance this index/counter for the next item.
            continue; // Skip the rest of this pass and check the next loop iteration.
        } // End this block, declaration or argument list.
        if (first >= 0xc2 && first <= 0xdf) { // Test the condition first >= 0xc2 && first <= 0xdf.
            continuation_count = 1; // Set continuation count to 1.
            code_point = first & 0x1f; // Set code point to first & 0x1f.
        } else if (first >= 0xe0 && first <= 0xef) { // Follow the alternative path when the previous condition is false.
            continuation_count = 2; // Set continuation count to 2.
            code_point = first & 0x0f; // Set code point to first & 0x0f.
        } else if (first >= 0xf0 && first <= 0xf4) { // Follow the alternative path when the previous condition is false.
            continuation_count = 3; // Set continuation count to 3.
            code_point = first & 0x07; // Set code point to first & 0x07.
        } else { // Follow the alternative path when the previous condition is false.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        if (index + continuation_count >= value.size()) return false; // Test the condition index + continuation_count >= value.size(). If true, report failure or no available item to the caller.
        for (size_t offset = 1; offset <= continuation_count; ++offset) { // Visit the specified items/indexes in order.
            const auto next = static_cast<unsigned char>(value[index + offset]); // Initialize next to static_cast<unsigned char>(value[index + offset]).
            if ((next & 0xc0) != 0x80) return false; // Test the condition (next & 0xc0) != 0x80. If true, report failure or no available item to the caller.
            code_point = (code_point << 6) | (next & 0x3f); // Set code point to (code_point << 6) | (next & 0x3f).
        } // End this block, declaration or argument list.
        if ((continuation_count == 2 && code_point < 0x800) || // Test the condition (continuation_count == 2 && code_point < 0x800) ||.
            (continuation_count == 3 && code_point < 0x10000) || // Continue this calculation/condition with the next line.
            (code_point >= 0xd800 && code_point <= 0xdfff) || // Continue this calculation/condition with the next line.
            code_point > 0x10ffff) { // Finish the parameters/condition and enter the block below.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        index += continuation_count + 1; // Update index using += continuation_count + 1.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

class FrameDecoder { // Declare FrameDecoder and its related operations/state.
public: // The class operations callable by other code.
    bool append(const uint8_t* bytes, size_t size) { // Finish the parameters/condition and enter the block below.
        constexpr size_t kMaxBufferedBytes = 1024 * 1024; // Initialize k Max Buffered Bytes to 1024 * 1024.
        if (size > kMaxBufferedBytes || // Test the condition size > kMaxBufferedBytes ||.
            buffer_.size() > kMaxBufferedBytes - size) { // Call buffer_.size with the arguments shown here.
            failed_ = true; // Set failed to true.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        buffer_.insert(buffer_.end(), bytes, bytes + size); // Call buffer_.insert with the arguments shown here.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    bool next(Frame& frame) { // Finish the parameters/condition and enter the block below.
        if (failed_ || buffer_.empty()) return false; // Test the condition failed_ || buffer_.empty(). If true, report failure or no available item to the caller.
        const auto type = buffer_[0]; // Initialize type to buffer_[0].
        if (type == kStatus) { // Test the condition type == kStatus.
            if (buffer_.size() < kStatusFrameBytes) return false; // Test the condition buffer_.size() < kStatusFrameBytes. If true, report failure or no available item to the caller.
            frame.type = type; // Set frame.type to type.
            frame.payload.assign(buffer_.begin() + 1, // Pass this next argument/value to the call or initializer above.
                                 buffer_.begin() + kStatusFrameBytes); // Call buffer_.begin with the arguments shown here.
            buffer_.erase(buffer_.begin(), // Call buffer_.erase with the arguments shown here.
                          buffer_.begin() + kStatusFrameBytes); // Call buffer_.begin with the arguments shown here.
            return true; // Report success, readiness or an available result to the caller.
        } // End this block, declaration or argument list.
        if (buffer_.size() < kHeaderBytes) return false; // Test the condition buffer_.size() < kHeaderBytes. If true, report failure or no available item to the caller.
        const auto length = read_u32_be(buffer_.data() + 1); // Initialize length to read_u32_be(buffer_.data() + 1).
        if (length > kMaxPayloadBytes || !valid_frame_payload(type, length)) { // Test the condition length > kMaxPayloadBytes || !valid_frame_payload(type, length).
            failed_ = true; // Set failed to true.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        const auto frame_size = kHeaderBytes + static_cast<size_t>(length); // Initialize frame size to kHeaderBytes + static_cast<size_t>(length).
        if (buffer_.size() < frame_size) return false; // Test the condition buffer_.size() < frame_size. If true, report failure or no available item to the caller.
        frame.type = type; // Set frame.type to type.
        frame.payload.assign(buffer_.begin() + kHeaderBytes, // Pass this next argument/value to the call or initializer above.
                             buffer_.begin() + frame_size); // Call buffer_.begin with the arguments shown here.
        buffer_.erase(buffer_.begin(), buffer_.begin() + frame_size); // Call buffer_.erase with the arguments shown here.
        return true; // Report success, readiness or an available result to the caller.
    } // End this block, declaration or argument list.

    bool failed() const { return failed_; } // Continue the expression or parameter list from the preceding line.
    size_t buffered_bytes() const { return buffer_.size(); } // Continue the expression or parameter list from the preceding line.

private: // Internal helpers and state used by this class.
    vector<uint8_t> buffer_; // Declare buffer for this class instance.
    bool failed_ = false; // Initialize failed to false.
}; // End this block, declaration or argument list.

// Parse a decimal protocol ID without exceptions or temporary integer types.
// Test before multiplying so a hostile long number cannot overflow uint32_t.
inline bool parse_decimal_id(const string& text, uint32_t& value) { // Finish the parameters/condition and enter the block below.
    if (text.empty()) return false; // Test the condition text.empty(). If true, report failure or no available item to the caller.
    value = 0; // Set value to 0.
    for (char character : text) { // Visit the specified items/indexes in order.
        if (character < '0' || character > '9') return false; // Test the condition character < '0' || character > '9'. If true, report failure or no available item to the caller.
        unsigned digit = character - '0'; // Initialize digit to character - '0'.
        if (value > (UINT32_MAX - digit) / 10) return false; // Test the condition value > (UINT32_MAX - digit) / 10. If true, report failure or no available item to the caller.
        value = value * 10 + digit; // Set value to value * 10 + digit.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

inline bool parse_file_list(const vector<uint8_t>& payload, // Pass this next argument/value to the call or initializer above.
                            FileListPacket& result) { // Declare result for this operation.
    if (payload.size() > kMaxFileListPayloadBytes) return false; // Test the condition payload.size() > kMaxFileListPayloadBytes. If true, report failure or no available item to the caller.
    const string text(payload.begin(), payload.end()); // Complete the call above with these remaining arguments.
    if (!valid_utf8(text)) return false; // Test the condition !valid_utf8(text). If true, report failure or no available item to the caller.
    const auto first_newline = text.find('\n'); // Initialize first newline to text.find('\n').
    if (first_newline == string::npos || // Test the condition first_newline == string::npos ||.
        text.compare(0, 12, "next_cursor=") != 0) { // Call text.compare with the arguments shown here.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    string cursor_text = text.substr(12, first_newline - 12); // Initialize cursor text to text.substr(12, first_newline - 12).
    if (!parse_decimal_id(cursor_text, result.next_cursor)) return false; // Test the condition !parse_decimal_id(cursor_text, result.next_cursor). If true, report failure or no available item to the caller.

    result.entries.clear(); // Discard the previous contents/state of this object.
    vector<uint32_t> seen_ids; // Declare seen ids for this operation.
    size_t start = first_newline + 1; // Initialize start to first_newline + 1.
    while (start < text.size()) { // Repeat this block while its condition remains true; recheck shared state on each pass.
        const auto end = text.find('\n', start); // Initialize end to text.find('\n', start).
        if (end == string::npos) return false; // Test the condition end == string::npos. If true, report failure or no available item to the caller.
        const auto tab = text.find('\t', start); // Initialize tab to text.find('\t', start).
        if (tab == string::npos || tab >= end) return false; // Test the condition tab == string::npos || tab >= end. If true, report failure or no available item to the caller.
        uint32_t id = 0; // Initialize id to 0.
        string id_text = text.substr(start, tab - start); // Initialize id text to text.substr(start, tab - start).
        if (!parse_decimal_id(id_text, id) || id == 0) return false; // Test the condition !parse_decimal_id(id_text, id) || id == 0. If true, report failure or no available item to the caller.
        const auto name = text.substr(tab + 1, end - tab - 1); // Initialize name to text.substr(tab + 1, end - tab - 1).
        if (name.empty() || name.size() > kMaxBasenameBytes || // Test the condition name.empty() || name.size() > kMaxBasenameBytes ||.
            name.find_first_of("\t\r/\\") != string::npos || // Call name.find_first_of with the arguments shown here.
            name == "." || name == ".." || // Continue checking the file/state comparison; no value is assigned here.
            result.entries.size() == kMaxFileListEntries || // Continue this calculation/condition with the next line.
            find(seen_ids.begin(), seen_ids.end(), id) != seen_ids.end()) { // Call find with the arguments shown here.
            return false; // Report failure or no available item to the caller.
        } // End this block, declaration or argument list.
        seen_ids.push_back(id); // Append the owned item at the back of this container.
        result.entries.push_back({id, name}); // Append the owned item at the back of this container.
        start = end + 1; // Set start to end + 1.
    } // End this block, declaration or argument list.
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

inline bool decode_state(const vector<uint8_t>& payload, // Pass this next argument/value to the call or initializer above.
                         State& state) { // Declare state for this operation.
    if (payload.size() != kStatePayloadBytes || // Test the condition payload.size() != kStatePayloadBytes ||.
        payload[0] != kProtocolVersion || payload[1] > 5 || // Continue this calculation/condition with the next line.
        payload[2] > 2 || !valid_gain_code(payload[3]) || // Continue this calculation/condition with the next line.
        payload[4] > 0x1f || payload[5] > 19 || // Continue this calculation/condition with the next line.
        read_u16_be(payload.data() + 6) != 0) { // Call read_u16_be with the arguments shown here.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    state.transport = static_cast<Transport>(payload[1]); // Set state.transport to static_cast<Transport>(payload[1]).
    state.filter = payload[2]; // Set state.filter to payload[2].
    state.gain = payload[3]; // Set state.gain to payload[3].
    state.storage_flags = payload[4]; // Set state.storage flags to payload[4].
    state.error_code = payload[5]; // Set state.error code to payload[5].
    state.active_session = read_u32_be(payload.data() + 8); // Set state.active session to read_u32_be(payload.data() + 8).
    state.dropped_telemetry = read_u32_be(payload.data() + 12); // Set state.dropped telemetry to read_u32_be(payload.data() + 12).
    state.recording_overruns = read_u32_be(payload.data() + 16); // Set state.recording overruns to read_u32_be(payload.data() + 16).
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

inline bool decode_command_result(const vector<uint8_t>& payload, // Pass this next argument/value to the call or initializer above.
                                  CommandResult& result) { // Declare result for this operation.
    if (payload.size() != kCommandResultPayloadBytes || // Test the condition payload.size() != kCommandResultPayloadBytes ||.
        payload[0] < 1 || payload[0] > 9 || payload[1] > 3 || // Continue this calculation/condition with the next line.
        read_u16_be(payload.data() + 2) != 0) { // Call read_u16_be with the arguments shown here.
        return false; // Report failure or no available item to the caller.
    } // End this block, declaration or argument list.
    result.command = static_cast<CommandType>(payload[0]); // Set result.command to static_cast<CommandType>(payload[0]).
    result.result = static_cast<Result>(payload[1]); // Set result.result to static_cast<Result>(payload[1]).
    result.detail = read_u32_be(payload.data() + 4); // Set result.detail to read_u32_be(payload.data() + 4).
    return true; // Report success, readiness or an available result to the caller.
} // End this block, declaration or argument list.

inline vector<uint8_t> make_data_packet( // Begin this function/widget call; its arguments follow below.
    const array<float, kWaveformSamples>& waveform, // Pass this next argument/value to the call or initializer above.
    const array<float, kSpectrumSamples>& spectrum) { // Finish the parameters/condition and enter the block below.
    vector<uint8_t> payload; // Declare payload for this operation.
    payload.reserve(kDataPayloadBytes); // Call payload.reserve with the arguments shown here.
    for (const auto sample : waveform) { // Visit the specified items/indexes in order.
        if (!isfinite(sample)) { // Test the condition !isfinite(sample).
            throw invalid_argument("non-finite waveform sample"); // Reject the invalid input by reporting this exception.
        } // End this block, declaration or argument list.
        append_float_be(payload, sample); // Call append_float_be with the arguments shown here.
    } // End this block, declaration or argument list.
    for (const auto magnitude : spectrum) { // Visit the specified items/indexes in order.
        if (!isfinite(magnitude) || magnitude < 0.0F) { // Test the condition !isfinite(magnitude) || magnitude < 0.0F.
            throw invalid_argument("invalid spectrum magnitude"); // Reject the invalid input by reporting this exception.
        } // End this block, declaration or argument list.
        append_float_be(payload, magnitude); // Call append_float_be with the arguments shown here.
    } // End this block, declaration or argument list.
    return make_frame(kData, payload); // Return make_frame(kData, payload) to the caller.
} // End this block, declaration or argument list.

}  // namespace phono::protocol
