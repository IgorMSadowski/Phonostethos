import 'dart:typed_data';

import 'package:flutter_test/flutter_test.dart';
import 'package:phonostethos/protocol.dart';

void main() {
  test('commands use five bytes and big-endian values', () {
    expect(encodeCommand(Command.setGain, 2), <int>[2, 0, 0, 0, 2]);
  });

  test('decoder retains a fragmented frame and parses status exception', () {
    final decoder = FrameDecoder();
    expect(decoder.add(<int>[statusType, 1, 1]), isEmpty);
    final messages = decoder.add(<int>[0, 0]);
    expect(messages, hasLength(1));
    expect((messages.first as StatusMessage).storageMounted, isTrue);
  });

  test('state validates fixed layout and network-order counters', () {
    final payload = ByteData(20)
      ..setUint8(0, 1)
      ..setUint8(1, Transport.recording.index)
      ..setUint8(2, 1)
      ..setUint8(3, 3)
      ..setUint8(4, 0x1b)
      ..setUint8(5, 0)
      ..setUint16(6, 0, Endian.big)
      ..setUint32(8, 0x12345678, Endian.big)
      ..setUint32(12, 4, Endian.big)
      ..setUint32(16, 2, Endian.big);
    final decoder = FrameDecoder();
    final message = decoder
        .add(_frame(stateType, payload.buffer.asUint8List()))
        .single as StateMessage;
    expect(message.transport, Transport.recording);
    expect(message.activeSession, 0x12345678);
    expect(message.droppedTelemetry, 4);
    expect(message.recordingOverruns, 2);
  });

  test('file lists retain opaque server file IDs', () {
    const text = 'next_cursor=91\n44\tHEART_20261101_143022_000001.wav\n';
    final decoder = FrameDecoder();
    final message = decoder
        .add(_frame(fileListType, Uint8List.fromList(text.codeUnits)))
        .single as FileListMessage;
    expect(message.nextCursor, 91);
    expect(message.entries.single.id, 44);
    expect(message.entries.single.basename, startsWith('HEART_'));
  });

  test('data floats are decoded as big-endian normalized values', () {
    final payload = ByteData((waveformLength + spectrumLength) * 4)
      ..setFloat32(0, 1, Endian.big)
      ..setFloat32(4, -1, Endian.big)
      ..setFloat32(waveformLength * 4, 0.5, Endian.big);
    final decoder = FrameDecoder();
    final message = decoder
        .add(_frame(dataType, payload.buffer.asUint8List()))
        .single as DataMessage;
    expect(message.waveform[0], 1);
    expect(message.waveform[1], -1);
    expect(message.spectrum[0], 0.5);
    expect(spectrumDbfs(1), closeTo(0, 1e-9));
    expect(spectrumDbfs(0), -120);
  });

  test('unknown or wrongly sized frames are rejected', () {
    expect(
      () => FrameDecoder().add(<int>[0xff, 0, 0, 0, 0]),
      throwsFormatException,
    );
    expect(
      () => FrameDecoder().add(<int>[stateType, 0, 0, 0, 19]),
      throwsFormatException,
    );
  });
}

Uint8List _frame(int type, Uint8List payload) {
  final header = ByteData(5)
    ..setUint8(0, type)
    ..setUint32(1, payload.length, Endian.big);
  return Uint8List.fromList(<int>[
    ...header.buffer.asUint8List(),
    ...payload,
  ]);
}
