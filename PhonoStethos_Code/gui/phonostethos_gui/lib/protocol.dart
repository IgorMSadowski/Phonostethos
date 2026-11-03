import 'dart:convert';
import 'dart:math' as math;
import 'dart:typed_data';

const int dataType = 0x01;
const int statusType = 0x02;
const int fileListType = 0x03;
const int stateType = 0x04;
const int commandResultType = 0x05;
const int maxPayloadBytes = 16384;
const int maxFileListPayloadBytes = 8192;
const int waveformLength = 1024;
const int spectrumLength = 512;

int _readUint32(List<int> bytes, int offset) =>
    ((bytes[offset] & 0xff) << 24) |
    ((bytes[offset + 1] & 0xff) << 16) |
    ((bytes[offset + 2] & 0xff) << 8) |
    (bytes[offset + 3] & 0xff);

enum Command {
  setMode(1),
  setGain(2),
  startRecording(3),
  stopRecording(4),
  playAudio(5),
  stopPlayback(6),
  ejectStorage(7),
  getArchive(8),
  deleteRecording(9);

  const Command(this.value);
  final int value;
}

enum Transport { live, recording, playback, stopping, ejecting, error }

enum CommandOutcome { completed, acceptedPending, rejected, failed }

sealed class ServerMessage {
  const ServerMessage();
}

class StatusMessage extends ServerMessage {
  const StatusMessage({
    required this.storageMounted,
    required this.allowRecord,
    required this.allowPlayback,
    required this.errorCode,
  });

  final bool storageMounted;
  final bool allowRecord;
  final bool allowPlayback;
  final int errorCode;
}

class StateMessage extends ServerMessage {
  const StateMessage({
    required this.transport,
    required this.filter,
    required this.gain,
    required this.storageFlags,
    required this.errorCode,
    required this.activeSession,
    required this.droppedTelemetry,
    required this.recordingOverruns,
  });

  final Transport transport;
  final int filter;
  final int gain;
  final int storageFlags;
  final int errorCode;
  final int activeSession;
  final int droppedTelemetry;
  final int recordingOverruns;
}

class CommandResultMessage extends ServerMessage {
  const CommandResultMessage({
    required this.command,
    required this.outcome,
    required this.detail,
  });

  final Command command;
  final CommandOutcome outcome;
  final int detail;
}

class FileEntry {
  const FileEntry({required this.id, required this.basename});

  final int id;
  final String basename;
}

class FileListMessage extends ServerMessage {
  const FileListMessage({required this.nextCursor, required this.entries});

  final int nextCursor;
  final List<FileEntry> entries;
}

class DataMessage extends ServerMessage {
  const DataMessage({required this.waveform, required this.spectrum});

  final List<double> waveform;
  final List<double> spectrum;
}

class FrameDecoder {
  final List<int> _buffer = <int>[];
  bool _failed = false;

  void clear() {
    _buffer.clear();
    _failed = false;
  }

  List<ServerMessage> add(List<int> bytes) {
    if (_failed) {
      throw const FormatException(
          'Protocol decoder is already in a failed state');
    }
    if (_buffer.length + bytes.length > 1024 * 1024) {
      _failed = true;
      throw const FormatException('Receive buffer exceeds its safety limit');
    }
    _buffer.addAll(bytes);
    final messages = <ServerMessage>[];
    while (_buffer.isNotEmpty) {
      final type = _buffer.first;
      if (type == statusType) {
        if (_buffer.length < 5) break;
        final payload = _buffer.sublist(1, 5);
        _buffer.removeRange(0, 5);
        messages.add(_decodeStatus(payload));
        continue;
      }
      if (type < dataType || type > commandResultType) {
        _fail('Unknown server frame type: $type');
      }
      if (_buffer.length < 5) break;
      final length = _readUint32(_buffer, 1);
      if (length > maxPayloadBytes) {
        _fail('Frame payload exceeds the protocol maximum');
      }
      final expected = switch (type) {
        dataType => (waveformLength + spectrumLength) * 4,
        fileListType => maxFileListPayloadBytes,
        stateType => 20,
        commandResultType => 8,
        _ => -1,
      };
      if (expected < 0 ||
          (type == fileListType ? length > expected : length != expected)) {
        _fail('Invalid payload length for frame type $type');
      }
      final frameLength = 5 + length;
      if (_buffer.length < frameLength) break;
      final payload = Uint8List.fromList(_buffer.sublist(5, frameLength));
      _buffer.removeRange(0, frameLength);
      messages.add(switch (type) {
        dataType => _decodeData(payload),
        fileListType => _decodeFileList(payload),
        stateType => _decodeState(payload),
        commandResultType => _decodeCommandResult(payload),
        _ => throw const FormatException('Unsupported server frame'),
      });
    }
    return messages;
  }

  Never _fail(String message) {
    _failed = true;
    throw FormatException(message);
  }

  StatusMessage _decodeStatus(List<int> bytes) {
    if (bytes.take(3).any((value) => value > 1) || bytes[3] > 19) {
      _fail('Invalid Status payload');
    }
    return StatusMessage(
      storageMounted: bytes[0] == 1,
      allowRecord: bytes[1] == 1,
      allowPlayback: bytes[2] == 1,
      errorCode: bytes[3],
    );
  }

  StateMessage _decodeState(Uint8List bytes) {
    final data = ByteData.sublistView(bytes);
    if (bytes[0] != 1 ||
        bytes[1] >= Transport.values.length ||
        bytes[2] > 2 ||
        bytes[3] < 1 ||
        bytes[3] > 10 ||
        bytes[4] > 0x1f ||
        bytes[5] > 19 ||
        data.getUint16(6, Endian.big) != 0) {
      _fail('Invalid State payload');
    }
    return StateMessage(
      transport: Transport.values[bytes[1]],
      filter: bytes[2],
      gain: bytes[3],
      storageFlags: bytes[4],
      errorCode: bytes[5],
      activeSession: data.getUint32(8, Endian.big),
      droppedTelemetry: data.getUint32(12, Endian.big),
      recordingOverruns: data.getUint32(16, Endian.big),
    );
  }

  CommandResultMessage _decodeCommandResult(Uint8List bytes) {
    final data = ByteData.sublistView(bytes);
    if (bytes[0] < 1 ||
        bytes[0] > Command.values.length ||
        bytes[1] >= CommandOutcome.values.length ||
        data.getUint16(2, Endian.big) != 0) {
      _fail('Invalid CommandResult payload');
    }
    return CommandResultMessage(
      command: Command.values[bytes[0] - 1],
      outcome: CommandOutcome.values[bytes[1]],
      detail: data.getUint32(4, Endian.big),
    );
  }

  FileListMessage _decodeFileList(Uint8List payload) {
    final text = utf8.decode(payload, allowMalformed: false);
    final lines = text.split('\n');
    if (lines.isEmpty || !lines.first.startsWith('next_cursor=')) {
      _fail('Invalid FileList header');
    }
    final cursorText = lines.first.substring('next_cursor='.length);
    final cursor = RegExp(r'^[0-9]+$').hasMatch(cursorText)
        ? int.tryParse(cursorText)
        : null;
    if (cursor == null || cursor < 0 || cursor > 0xffffffff) {
      _fail('Invalid FileList cursor');
    }
    final entries = <FileEntry>[];
    final seenIds = <int>{};
    final records = lines.skip(1).toList();
    if (records.isNotEmpty && records.last.isEmpty) records.removeLast();
    for (final line in records) {
      final separator = line.indexOf('\t');
      if (separator <= 0 ||
          separator == line.length - 1 ||
          line.indexOf('\t', separator + 1) >= 0) {
        _fail('Invalid FileList entry');
      }
      final idText = line.substring(0, separator);
      final id =
          RegExp(r'^[0-9]+$').hasMatch(idText) ? int.tryParse(idText) : null;
      final name = line.substring(separator + 1);
      if (id == null ||
          id == 0 ||
          id > 0xffffffff ||
          !seenIds.add(id) ||
          utf8.encode(name).length > 128 ||
          name == '.' ||
          name == '..' ||
          name.contains('/') ||
          name.contains('\\') ||
          name.contains('\r') ||
          !name.endsWith('.wav')) {
        _fail('Invalid FileList identity or basename');
      }
      entries.add(FileEntry(id: id, basename: name));
      if (entries.length > 32) _fail('FileList page contains too many entries');
    }
    if (!text.endsWith('\n')) _fail('FileList records must end with newline');
    return FileListMessage(nextCursor: cursor, entries: entries);
  }

  DataMessage _decodeData(Uint8List payload) {
    final bytes = ByteData.sublistView(payload);
    final waveform = List<double>.generate(
      waveformLength,
      (index) => bytes.getFloat32(index * 4, Endian.big),
      growable: false,
    );
    final spectrum = List<double>.generate(
      spectrumLength,
      (index) => bytes.getFloat32((waveformLength + index) * 4, Endian.big),
      growable: false,
    );
    if (waveform.any((value) => !value.isFinite) ||
        spectrum.any((value) => !value.isFinite || value < 0)) {
      _fail('Telemetry contains invalid floating-point values');
    }
    return DataMessage(waveform: waveform, spectrum: spectrum);
  }
}

Uint8List encodeCommand(Command command, int value) {
  if (value < 0 || value > 0xffffffff) {
    throw RangeError.range(value, 0, 0xffffffff, 'value');
  }
  final bytes = ByteData(5)
    ..setUint8(0, command.value)
    ..setUint32(1, value, Endian.big);
  return bytes.buffer.asUint8List();
}

double spectrumDbfs(double magnitude) {
  if (!magnitude.isFinite || magnitude < 0) {
    throw ArgumentError.value(magnitude, 'magnitude');
  }
  if (magnitude <= 1e-6) return -120;
  return 20 * math.log(magnitude) / math.ln10;
}
