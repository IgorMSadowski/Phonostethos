/*
 * protocol.dart - implements the Dart side of the binary TCP protocol.
 *
 * CONNECTIONS: main.dart feeds socket chunks to FrameDecoder and uses encodeCommand before Socket.add. This file mirrors app/include/phonostethos/protocol.h; it does not open sockets or perform DSP filtering.
 *
 * FUNCTIONS:
 * - _readUint32(): read Uint32.
 * - Command(): Command.
 * - ServerMessage(): Server Message.
 * - clear(): discard waiting channel data and reset its state under its owner's rules.
 * - add(): retain incomplete TCP bytes and return only complete decoded messages.
 * - _fail(): mark the frame decoder failed and reject the malformed stream.
 * - _decodeStatus(): validate and decode the fixed-size capability frame.
 * - _decodeState(): validate and decode the versioned transport/configuration state frame.
 * - _decodeCommandResult(): validate and decode the server's command acknowledgement.
 * - _decodeFileList(): validate archive text records, unique IDs, names and pagination.
 * - _decodeData(): decode finite big-endian waveform samples and nonnegative FFT magnitudes.
 * - encodeCommand(): encode a command ID and unsigned big-endian argument into exactly five bytes.
 * - spectrumDbfs(): convert linear peak amplitude to dBFS with a -120 dBFS display floor.
 */

// API DOCUMENTATION FOR THIS FILE:
// Dart ByteData: explicit-width integers/floats and byte order:
// https://api.dart.dev/dart-typed_data/ByteData-class.html
// Dart UTF-8 encoding/decoding:
// https://api.dart.dev/dart-convert/Utf8Codec-class.html
// Dart math: sqrt/log and numeric calculations:
// https://api.dart.dev/dart-math/dart-math-library.html
// These links document external library/OS APIs; project class methods are
// implemented in the corresponding project headers and source files.


import 'dart:convert'; // Import strict UTF-8 decoding of archive names.
import 'dart:math' as math; // Import square-root/logarithm functions used to display spectrum amplitudes.
import 'dart:typed_data'; // Import byte arrays and explicit big-endian integer/float access.

const int dataType = 0x01; // Initialize data Type to 0x01.
const int statusType = 0x02; // Initialize status Type to 0x02.
const int fileListType = 0x03; // Initialize file List Type to 0x03.
const int stateType = 0x04; // Initialize state Type to 0x04.
const int commandResultType = 0x05; // Initialize command Result Type to 0x05.
const int maxPayloadBytes = 16384; // Initialize max Payload Bytes to 16384.
const int maxFileListPayloadBytes = 8192; // Initialize max File List Payload Bytes to 8192.
const int waveformLength = 1024; // Initialize waveform Length to 1024.
const int spectrumLength = 512; // Initialize spectrum Length to 512.

// The TCP gain field is an integer; its existing codes 1..20 mean 1x..20x.
// Fractional values cannot be put directly in that field, so unused codes
// 101..103 identify 0.5x, 0.75x and 0.8x without changing the packet layout.
// These match protocol.h on the Pi. Its gain_multiplier() converts the code
// before processing audio. _gainLabel() in main.dart shows the actual volume.
const int gainHalf = 101; // 0.5x.
const int gainThreeQuarters = 102; // 0.75x.
const int gainFourFifths = 103; // 0.8x.

int _readUint32(List<int> bytes, int offset) => // Continue the expression or parameter list from the preceding line.
    ((bytes[offset] & 0xff) << 24) | // Continue this calculation/condition with the next line.
    ((bytes[offset + 1] & 0xff) << 16) | // Continue this calculation/condition with the next line.
    ((bytes[offset + 2] & 0xff) << 8) | // Continue this calculation/condition with the next line.
    (bytes[offset + 3] & 0xff); // Complete the call above with these remaining arguments.

enum Command { // Give the following named choices distinct typed values.
  setMode(1), // Call setMode with the arguments shown here.
  setGain(2), // Call setGain with the arguments shown here.
  startRecording(3), // Call startRecording with the arguments shown here.
  stopRecording(4), // Call stopRecording with the arguments shown here.
  playAudio(5), // Call playAudio with the arguments shown here.
  stopPlayback(6), // Call stopPlayback with the arguments shown here.
  ejectStorage(7), // Call ejectStorage with the arguments shown here.
  getArchive(8), // Call getArchive with the arguments shown here.
  deleteRecording(9); // Resolve the selected file ID and request deletion in FileIO.

  const Command(this.value); // Complete the call above with these remaining arguments.
  final int value; // Declare value for this operation.
} // End this block, declaration or argument list.

enum Transport { live, recording, playback, stopping, ejecting, error } // Give the following named choices distinct typed values.

enum CommandOutcome { completed, acceptedPending, rejected, failed } // Give the following named choices distinct typed values.

abstract class ServerMessage { // Continue the expression or parameter list from the preceding line.
  const ServerMessage(); // Complete the call above with these remaining arguments.
} // End this block, declaration or argument list.

class StatusMessage extends ServerMessage { // Declare StatusMessage and its related operations/state.
  const StatusMessage({ // Continue the expression or parameter list from the preceding line.
    required this.storageMounted, // Pass this next argument/value to the call or initializer above.
    required this.allowRecord, // Pass this next argument/value to the call or initializer above.
    required this.allowPlayback, // Pass this next argument/value to the call or initializer above.
    required this.errorCode, // Pass this next argument/value to the call or initializer above.
  }); // End this block, declaration or argument list.

  final bool storageMounted; // Declare storage Mounted for this operation.
  final bool allowRecord; // Declare allow Record for this operation.
  final bool allowPlayback; // Declare allow Playback for this operation.
  final int errorCode; // Declare error Code for this operation.
} // End this block, declaration or argument list.

class StateMessage extends ServerMessage { // Declare StateMessage and its related operations/state.
  const StateMessage({ // Continue the expression or parameter list from the preceding line.
    required this.transport, // Pass this next argument/value to the call or initializer above.
    required this.filter, // Pass this next argument/value to the call or initializer above.
    required this.gain, // Pass this next argument/value to the call or initializer above.
    required this.storageFlags, // Pass this next argument/value to the call or initializer above.
    required this.errorCode, // Pass this next argument/value to the call or initializer above.
    required this.activeSession, // Pass this next argument/value to the call or initializer above.
    required this.droppedTelemetry, // Pass this next argument/value to the call or initializer above.
    required this.recordingOverruns, // Pass this next argument/value to the call or initializer above.
  }); // End this block, declaration or argument list.

  final Transport transport; // Declare transport for this operation.
  final int filter; // Declare filter for this operation.
  final int gain; // Declare gain for this operation.
  final int storageFlags; // Declare storage Flags for this operation.
  final int errorCode; // Declare error Code for this operation.
  final int activeSession; // Declare active Session for this operation.
  final int droppedTelemetry; // Declare dropped Telemetry for this operation.
  final int recordingOverruns; // Declare recording Overruns for this operation.
} // End this block, declaration or argument list.

class CommandResultMessage extends ServerMessage { // Declare CommandResultMessage and its related operations/state.
  const CommandResultMessage({ // Continue the expression or parameter list from the preceding line.
    required this.command, // Pass this next argument/value to the call or initializer above.
    required this.outcome, // Pass this next argument/value to the call or initializer above.
    required this.detail, // Pass this next argument/value to the call or initializer above.
  }); // End this block, declaration or argument list.

  final Command command; // Declare command for this operation.
  final CommandOutcome outcome; // Declare outcome for this operation.
  final int detail; // Declare detail for this operation.
} // End this block, declaration or argument list.

class FileEntry { // Declare FileEntry and its related operations/state.
  const FileEntry({required this.id, required this.basename}); // Complete the call above with these remaining arguments.

  final int id; // Declare id for this operation.
  final String basename; // Declare basename for this operation.
} // End this block, declaration or argument list.

class FileListMessage extends ServerMessage { // Declare FileListMessage and its related operations/state.
  const FileListMessage({required this.nextCursor, required this.entries}); // Complete the call above with these remaining arguments.

  final int nextCursor; // Declare next Cursor for this operation.
  final List<FileEntry> entries; // Declare entries for this operation.
} // End this block, declaration or argument list.

class DataMessage extends ServerMessage { // Declare DataMessage and its related operations/state.
  const DataMessage({required this.waveform, required this.spectrum}); // Complete the call above with these remaining arguments.

  final List<double> waveform; // Declare waveform for this operation.
  final List<double> spectrum; // Declare spectrum for this operation.
} // End this block, declaration or argument list.

// TCP may split a frame or put several frames in one chunk.
// Keep bytes until the complete header and payload can be decoded.
class FrameDecoder { // Declare FrameDecoder and its related operations/state.
  final List<int> _buffer = <int>[]; // Initialize buffer to <int>[].
  bool _failed = false; // Initialize failed to false.

  void clear() { // Discard waiting channel data and reset its state under its owner's rules.
    _buffer.clear(); // Discard the previous contents/state of this object.
    _failed = false; // Set failed to false.
  } // End this block, declaration or argument list.

  // Returns only complete messages. Unfinished bytes stay in _buffer.
  List<ServerMessage> add(List<int> bytes) { // Retain incomplete TCP bytes and return only complete decoded messages.
    if (_failed) { // Test the condition _failed.
      throw const FormatException( // Reject the invalid input by reporting this exception.
          'Protocol decoder is already in a failed state'); // Complete the call above with these remaining arguments.
    } // End this block, declaration or argument list.
    if (_buffer.length + bytes.length > 1024 * 1024) { // Test the condition _buffer.length + bytes.length > 1024 * 1024.
      _failed = true; // Set failed to true.
      throw const FormatException('Receive buffer exceeds its safety limit'); // Reject the invalid input by reporting this exception.
    } // End this block, declaration or argument list.
    _buffer.addAll(bytes); // Call _buffer.addAll with the arguments shown here.
    final messages = <ServerMessage>[]; // Initialize messages to <ServerMessage>[].
    while (_buffer.isNotEmpty) { // Repeat this block while its condition remains true; recheck shared state on each pass.
      final type = _buffer.first; // Initialize type to _buffer.first.
      if (type == statusType) { // Test the condition type == statusType.
        if (_buffer.length < 5) break; // Test the condition _buffer.length < 5. If true, leave the current loop or switch branch.
        final payload = _buffer.sublist(1, 5); // Initialize payload to _buffer.sublist(1, 5).
        _buffer.removeRange(0, 5); // Call _buffer.removeRange with the arguments shown here.
        messages.add(_decodeStatus(payload)); // Call messages.add with the arguments shown here.
        continue; // Skip the rest of this pass and check the next loop iteration.
      } // End this block, declaration or argument list.
      if (type < dataType || type > commandResultType) { // Test the condition type < dataType || type > commandResultType.
        _fail('Unknown server frame type: $type'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      if (_buffer.length < 5) break; // Test the condition _buffer.length < 5. If true, leave the current loop or switch branch.
      final length = _readUint32(_buffer, 1); // Initialize length to _readUint32(_buffer, 1).
      if (length > maxPayloadBytes) { // Test the condition length > maxPayloadBytes.
        _fail('Frame payload exceeds the protocol maximum'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      int expected = -1; // Initialize expected to -1.
      switch (type) { // Choose one branch from the supplied command/state value.
        case dataType: // Handle the dataType choice in this switch.
          expected = (waveformLength + spectrumLength) * 4; // Set expected to (waveformLength + spectrumLength) * 4.
          break; // Leave the current loop or switch branch.
        case fileListType: // Handle the fileListType choice in this switch.
          expected = maxFileListPayloadBytes; // Set expected to maxFileListPayloadBytes.
          break; // Leave the current loop or switch branch.
        case stateType: // Handle the stateType choice in this switch.
          expected = 20; // Set expected to 20.
          break; // Leave the current loop or switch branch.
        case commandResultType: // Handle the commandResultType choice in this switch.
          expected = 8; // Set expected to 8.
          break; // Leave the current loop or switch branch.
      } // End this block, declaration or argument list.
      bool wrongLength = length != expected; // Initialize wrong Length to length != expected.
      if (type == fileListType) { // Test the condition type == fileListType.
        wrongLength = // Continue the expression or parameter list from the preceding line.
            length > expected; // Archive payloads have variable length.
      } // End this block, declaration or argument list.
      if (expected < 0 || wrongLength) { // Test the condition expected < 0 || wrongLength.
        _fail('Invalid payload length for frame type $type'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      final frameLength = 5 + length; // Initialize frame Length to 5 + length.
      if (_buffer.length < frameLength) break; // Test the condition _buffer.length < frameLength. If true, leave the current loop or switch branch.
      final payload = Uint8List.fromList(_buffer.sublist(5, frameLength)); // Initialize payload to Uint8List.fromList(_buffer.sublist(5, frameLength)).
      _buffer.removeRange(0, frameLength); // Call _buffer.removeRange with the arguments shown here.
      switch (type) { // Choose one branch from the supplied command/state value.
        case dataType: // Handle the dataType choice in this switch.
          messages.add(_decodeData(payload)); // Call messages.add with the arguments shown here.
          break; // Leave the current loop or switch branch.
        case fileListType: // Handle the fileListType choice in this switch.
          messages.add(_decodeFileList(payload)); // Call messages.add with the arguments shown here.
          break; // Leave the current loop or switch branch.
        case stateType: // Handle the stateType choice in this switch.
          messages.add(_decodeState(payload)); // Call messages.add with the arguments shown here.
          break; // Leave the current loop or switch branch.
        case commandResultType: // Handle the commandResultType choice in this switch.
          messages.add(_decodeCommandResult(payload)); // Call messages.add with the arguments shown here.
          break; // Leave the current loop or switch branch.
        default: // Handle values not matched by an earlier case.
          _fail('Unsupported server frame'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
    } // End this block, declaration or argument list.
    return messages; // Return messages to the caller.
  } // End this block, declaration or argument list.

  Never _fail(String message) { // Mark the frame decoder failed and reject the malformed stream.
    _failed = true; // Set failed to true.
    throw FormatException(message); // Reject the invalid input by reporting this exception.
  } // End this block, declaration or argument list.

  StatusMessage _decodeStatus(List<int> bytes) { // Validate and decode the fixed-size capability frame.
    if (bytes[0] > 1 || bytes[1] > 1 || bytes[2] > 1 || bytes[3] > 19) { // Test the condition bytes[0] > 1 || bytes[1] > 1 || bytes[2] > 1 || bytes[3] > 19.
      _fail('Invalid Status payload'); // Mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    return StatusMessage( // Return StatusMessage( to the caller.
      storageMounted: bytes[0] == 1, // Continue checking the file/state comparison; no value is assigned here.
      allowRecord: bytes[1] == 1, // Continue checking the file/state comparison; no value is assigned here.
      allowPlayback: bytes[2] == 1, // Continue checking the file/state comparison; no value is assigned here.
      errorCode: bytes[3], // Set the error Code property of this widget/record.
    ); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  StateMessage _decodeState(Uint8List bytes) { // Validate and decode the versioned transport/configuration state frame.
    final data = ByteData.sublistView(bytes); // Initialize data to ByteData.sublistView(bytes).
    bool integerGain = bytes[3] >= 1 && bytes[3] <= 20; // Initialize integer Gain to bytes[3] >= 1 && bytes[3] <= 20.
    bool fractionalGain = bytes[3] == gainHalf || // Initialize fractional Gain to bytes[3] == gainHalf ||.
        bytes[3] == gainThreeQuarters || // Continue checking the file/state comparison; no value is assigned here.
        bytes[3] == gainFourFifths; // Continue checking the file/state comparison; no value is assigned here.
    bool validGain = integerGain || fractionalGain; // Initialize valid Gain to integerGain || fractionalGain.
    if (bytes[0] != 1 || // Test the condition bytes[0] != 1 ||.
        bytes[1] >= Transport.values.length || // Continue this calculation/condition with the next line.
        bytes[2] > 2 || // Continue this calculation/condition with the next line.
        !validGain || // Continue this calculation/condition with the next line.
        bytes[4] > 0x1f || // Continue this calculation/condition with the next line.
        bytes[5] > 19 || // Continue this calculation/condition with the next line.
        data.getUint16(6, Endian.big) != 0) { // Call data.getUint16 with the arguments shown here.
      _fail('Invalid State payload'); // Mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    return StateMessage( // Return StateMessage( to the caller.
      transport: Transport.values[bytes[1]], // Set the transport property of this widget/record.
      filter: bytes[2], // Set the filter property of this widget/record.
      gain: bytes[3], // Set the gain property of this widget/record.
      storageFlags: bytes[4], // Set the storage Flags property of this widget/record.
      errorCode: bytes[5], // Set the error Code property of this widget/record.
      activeSession: data.getUint32(8, Endian.big), // Set the active Session property of this widget/record.
      droppedTelemetry: data.getUint32(12, Endian.big), // Set the dropped Telemetry property of this widget/record.
      recordingOverruns: data.getUint32(16, Endian.big), // Set the recording Overruns property of this widget/record.
    ); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  CommandResultMessage _decodeCommandResult(Uint8List bytes) { // Validate and decode the server's command acknowledgement.
    final data = ByteData.sublistView(bytes); // Initialize data to ByteData.sublistView(bytes).
    if (bytes[0] < 1 || // Test the condition bytes[0] < 1 ||.
        bytes[0] > Command.values.length || // Continue this calculation/condition with the next line.
        bytes[1] >= CommandOutcome.values.length || // Continue this calculation/condition with the next line.
        data.getUint16(2, Endian.big) != 0) { // Call data.getUint16 with the arguments shown here.
      _fail('Invalid CommandResult payload'); // Mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    return CommandResultMessage( // Return CommandResultMessage( to the caller.
      command: Command.values[bytes[0] - 1], // Set the command property of this widget/record.
      outcome: CommandOutcome.values[bytes[1]], // Set the outcome property of this widget/record.
      detail: data.getUint32(4, Endian.big), // Set the detail property of this widget/record.
    ); // End this block, declaration or argument list.
  } // End this block, declaration or argument list.

  FileListMessage _decodeFileList(Uint8List payload) { // Validate archive text records, unique IDs, names and pagination.
    final text = utf8.decode(payload, allowMalformed: false); // Initialize text to utf8.decode(payload, allowMalformed: false).
    final lines = text.split('\n'); // Initialize lines to text.split('\n').
    if (lines.isEmpty || !lines.first.startsWith('next_cursor=')) { // Test the condition lines.isEmpty || !lines.first.startsWith('next_cursor=').
      _fail('Invalid FileList header'); // Mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    final cursorText = lines.first.substring('next_cursor='.length); // Initialize cursor Text to lines.first.substring('next_cursor='.length).
    int? cursor; // Declare cursor for this operation.
    if (RegExp(r'^[0-9]+$').hasMatch(cursorText)) { // Test the condition RegExp(r'^[0-9]+$').hasMatch(cursorText).
      cursor = int.tryParse(cursorText); // Set cursor to int.tryParse(cursorText).
    } // End this block, declaration or argument list.
    if (cursor == null || cursor < 0 || cursor > 0xffffffff) { // Test the condition cursor == null || cursor < 0 || cursor > 0xffffffff.
      _fail('Invalid FileList cursor'); // Mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    final entries = <FileEntry>[]; // Initialize entries to <FileEntry>[].
    final seenIds = <int>{}; // Initialize seen Ids to <int>{}.
    final records = lines.skip(1).toList(); // Initialize records to lines.skip(1).toList().
    if (records.isNotEmpty && records.last.isEmpty) records.removeLast(); // Test the condition records.isNotEmpty && records.last.isEmpty. If true, call records.removelast with the arguments shown here.
    for (final line in records) { // Visit the specified items/indexes in order.
      final separator = line.indexOf('\t'); // Initialize separator to line.indexOf('\t').
      if (separator <= 0 || // Test the condition separator <= 0 ||.
          separator == line.length - 1 || // Continue checking the file/state comparison; no value is assigned here.
          line.indexOf('\t', separator + 1) >= 0) { // Call line.indexOf with the arguments shown here.
        _fail('Invalid FileList entry'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      final idText = line.substring(0, separator); // Initialize id Text to line.substring(0, separator).
      int? id; // Declare id for this operation.
      if (RegExp(r'^[0-9]+$').hasMatch(idText)) { // Test the condition RegExp(r'^[0-9]+$').hasMatch(idText).
        id = int.tryParse(idText); // Set id to int.tryParse(idText).
      } // End this block, declaration or argument list.
      final name = line.substring(separator + 1); // Initialize name to line.substring(separator + 1).
      if (id == null || // Test the condition id == null ||.
          id == 0 || // Continue checking the file/state comparison; no value is assigned here.
          id > 0xffffffff || // Continue this calculation/condition with the next line.
          !seenIds.add(id) || // Continue this calculation/condition with the next line.
          utf8.encode(name).length > 128 || // Call utf8.encode with the arguments shown here.
          name == '.' || // Continue checking the file/state comparison; no value is assigned here.
          name == '..' || // Continue checking the file/state comparison; no value is assigned here.
          name.contains('/') || // Call name.contains with the arguments shown here.
          name.contains('\\') || // Call name.contains with the arguments shown here.
          name.contains('\r') || // Call name.contains with the arguments shown here.
          !name.endsWith('.wav')) { // Finish the parameters/condition and enter the block below.
        _fail('Invalid FileList identity or basename'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      entries.add(FileEntry(id: id, basename: name)); // Call entries.add with the arguments shown here.
      if (entries.length > 32) _fail('FileList page contains too many entries'); // Test the condition entries.length > 32. If true, mark the frame decoder failed and reject the malformed stream.
    } // End this block, declaration or argument list.
    if (!text.endsWith('\n')) _fail('FileList records must end with newline'); // Test the condition !text.endsWith('\n'). If true, mark the frame decoder failed and reject the malformed stream.
    return FileListMessage(nextCursor: cursor, entries: entries); // Return FileListMessage(nextCursor: cursor, entries: entries) to the caller.
  } // End this block, declaration or argument list.

  DataMessage _decodeData(Uint8List payload) { // Decode finite big-endian waveform samples and nonnegative FFT magnitudes.
    final bytes = ByteData.sublistView(payload); // Initialize bytes to ByteData.sublistView(payload).
    // Dart API: ByteData.getFloat32(offset, Endian.big).
    // Each value occupies four bytes. Decode waveform first, then FFT bins.
    final waveform = <double>[]; // Initialize waveform to <double>[].
    final spectrum = <double>[]; // Initialize spectrum to <double>[].
    for (int i = 0; i < waveformLength; i++) { // Visit the specified items/indexes in order.
      double sample = bytes.getFloat32(i * 4, Endian.big); // Initialize sample to bytes.getFloat32(i * 4, Endian.big).
      if (!sample.isFinite) _fail('Telemetry contains invalid waveform values'); // Test the condition !sample.isFinite. If true, mark the frame decoder failed and reject the malformed stream.
      waveform.add(sample); // Call waveform.add with the arguments shown here.
    } // End this block, declaration or argument list.
    for (int i = 0; i < spectrumLength; i++) { // Visit the specified items/indexes in order.
      int offset = (waveformLength + i) * 4; // Initialize offset to (waveformLength + i) * 4.
      double magnitude = bytes.getFloat32(offset, Endian.big); // Initialize magnitude to bytes.getFloat32(offset, Endian.big).
      if (!magnitude.isFinite || magnitude < 0) { // Test the condition !magnitude.isFinite || magnitude < 0.
        _fail('Telemetry contains invalid spectrum values'); // Mark the frame decoder failed and reject the malformed stream.
      } // End this block, declaration or argument list.
      spectrum.add(magnitude); // Call spectrum.add with the arguments shown here.
    } // End this block, declaration or argument list.
    return DataMessage(waveform: waveform, spectrum: spectrum); // Return DataMessage(waveform: waveform, spectrum: spectrum) to the caller.
  } // End this block, declaration or argument list.
} // End this block, declaration or argument list.

Uint8List encodeCommand(Command command, int value) { // Encode a command ID and unsigned big-endian argument into exactly five bytes.
  if (value < 0 || value > 0xffffffff) { // Test the condition value < 0 || value > 0xffffffff.
    throw RangeError.range(value, 0, 0xffffffff, 'value'); // Reject the invalid input by reporting this exception.
  } // End this block, declaration or argument list.
  // Dart API: ByteData stores explicitly sized binary fields.
  // Put the command in byte 0 and its unsigned argument in bytes 1 to 4.
  final bytes = ByteData(5); // Initialize bytes to ByteData(5).
  bytes.setUint8(0, command.value); // Call bytes.setUint8 with the arguments shown here.
  bytes.setUint32(1, value, Endian.big); // Call bytes.setUint32 with the arguments shown here.
  return bytes.buffer.asUint8List(); // Return bytes.buffer.asUint8List() to the caller.
} // End this block, declaration or argument list.

double spectrumDbfs(double magnitude) { // Convert linear peak amplitude to dBFS with a -120 dBFS display floor.
  if (!magnitude.isFinite || magnitude < 0) { // Test the condition !magnitude.isFinite || magnitude < 0.
    throw ArgumentError.value(magnitude, 'magnitude'); // Reject the invalid input by reporting this exception.
  } // End this block, declaration or argument list.
  if (magnitude <= 1e-6) return -120; // Test the condition magnitude <= 1e-6. If true, return -120 to the caller.
  return 20 * math.log(magnitude) / math.ln10; // Return 20 * math.log(magnitude) / math.ln10 to the caller.
} // End this block, declaration or argument list.
