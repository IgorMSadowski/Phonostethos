/*
 * protocol_test.dart - checks GUI protocol behavior with repeatable tests.
 *
 * CONNECTIONS: This test imports the handwritten Dart protocol code. It checks frame boundaries and validation without connecting to Raspberry Pi hardware.
 *
 * FUNCTIONS:
 * - main(): start the program and hand execution to the application controller or GUI framework.
 * - _frame(): frame.
 */

import 'dart:typed_data'; // Import byte arrays and explicit big-endian integer/float access.

import 'package:flutter_test/flutter_test.dart'; // Import Flutter test assertions and test registration.
import 'package:phonostethos/protocol.dart'; // Import the matching command encoder, frame decoder and message records.

void main() { // Start the program and hand execution to the application controller or GUI framework.
  test('commands use five bytes and big-endian values', () { // Call test with the arguments shown here.
    expect(encodeCommand(Command.setGain, 2), <int>[2, 0, 0, 0, 2]); // Verify this expected result; fail the test if it is not satisfied.
  }); // End this block, declaration or argument list.

  test('decoder retains a fragmented frame and parses status exception', () { // Call test with the arguments shown here.
    final decoder = FrameDecoder(); // Initialize decoder to FrameDecoder().
    expect(decoder.add(<int>[statusType, 1, 1]), isEmpty); // Verify this expected result; fail the test if it is not satisfied.
    final messages = decoder.add(<int>[0, 0]); // Initialize messages to decoder.add(<int>[0, 0]).
    expect(messages, hasLength(1)); // Verify this expected result; fail the test if it is not satisfied.
    expect((messages.first as StatusMessage).storageMounted, isTrue); // Verify this expected result; fail the test if it is not satisfied.
  }); // End this block, declaration or argument list.

  test('state validates fixed layout and network-order counters', () { // Call test with the arguments shown here.
    final payload = ByteData(20) // Initialize payload to ByteData(20).
      ..setUint8(0, 1) // Continue the expression or parameter list from the preceding line.
      ..setUint8(1, Transport.recording.index) // Continue the expression or parameter list from the preceding line.
      ..setUint8(2, 1) // Continue the expression or parameter list from the preceding line.
      ..setUint8(3, 3) // Continue the expression or parameter list from the preceding line.
      ..setUint8(4, 0x1b) // Continue the expression or parameter list from the preceding line.
      ..setUint8(5, 0) // Continue the expression or parameter list from the preceding line.
      ..setUint16(6, 0, Endian.big) // Continue the expression or parameter list from the preceding line.
      ..setUint32(8, 0x12345678, Endian.big) // Continue the expression or parameter list from the preceding line.
      ..setUint32(12, 4, Endian.big) // Continue the expression or parameter list from the preceding line.
      ..setUint32(16, 2, Endian.big); // Complete the call above with these remaining arguments.
    final decoder = FrameDecoder(); // Initialize decoder to FrameDecoder().
    final message = decoder // Initialize message to decoder.
        .add(_frame(stateType, payload.buffer.asUint8List())) // Continue the expression or parameter list from the preceding line.
        .single as StateMessage; // Complete this expression or declaration.
    expect(message.transport, Transport.recording); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.activeSession, 0x12345678); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.droppedTelemetry, 4); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.recordingOverruns, 2); // Verify this expected result; fail the test if it is not satisfied.
  }); // End this block, declaration or argument list.

  test('state accepts all supported gain codes and rejects unknown codes', () { // Call test with the arguments shown here.
    final validCodes = [1, 10, 11, 20, 101, 102, 103]; // Initialize valid Codes to [1, 10, 11, 20, 101, 102, 103].
    for (final code in validCodes) { // Visit the specified items/indexes in order.
      final payload = Uint8List(20); // Initialize payload to Uint8List(20).
      payload[0] = 1; // Protocol version.
      payload[3] = code; // Set payload[3] to code.
      final messages = FrameDecoder().add(_frame(stateType, payload)); // Initialize messages to FrameDecoder().add(_frame(stateType, payload)).
      expect((messages.single as StateMessage).gain, code); // Verify this expected result; fail the test if it is not satisfied.
    } // End this block, declaration or argument list.
    final invalidPayload = Uint8List(20); // Initialize invalid Payload to Uint8List(20).
    invalidPayload[0] = 1; // Set invalid Payload[0] to 1.
    invalidPayload[3] = 21; // Set invalid Payload[3] to 21.
    expect( // Verify this expected result; fail the test if it is not satisfied.
      () => FrameDecoder().add(_frame(stateType, invalidPayload)), // Pass this next argument/value to the call or initializer above.
      throwsFormatException, // Name this field, choice or argument: throws Format Exception.
    ); // End this block, declaration or argument list.
  }); // End this block, declaration or argument list.

  test('file lists retain opaque server file IDs', () { // Call test with the arguments shown here.
    const text = 'next_cursor=91\n44\tHEART_20261101_143022_000001.wav\n'; // Initialize text to 'next_cursor=91\n44\tHEART_20261101_143022_000001.wav\n'.
    final decoder = FrameDecoder(); // Initialize decoder to FrameDecoder().
    final message = decoder // Initialize message to decoder.
        .add(_frame(fileListType, Uint8List.fromList(text.codeUnits))) // Continue the expression or parameter list from the preceding line.
        .single as FileListMessage; // Complete this expression or declaration.
    expect(message.nextCursor, 91); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.entries.single.id, 44); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.entries.single.basename, startsWith('HEART_')); // Verify this expected result; fail the test if it is not satisfied.
  }); // End this block, declaration or argument list.

  test('data floats are decoded as big-endian normalized values', () { // Call test with the arguments shown here.
    final payload = ByteData((waveformLength + spectrumLength) * 4) // Initialize payload to ByteData((waveformLength + spectrumLength) * 4).
      ..setFloat32(0, 1, Endian.big) // Continue the expression or parameter list from the preceding line.
      ..setFloat32(4, -1, Endian.big) // Continue the expression or parameter list from the preceding line.
      ..setFloat32(waveformLength * 4, 0.5, Endian.big); // Complete the call above with these remaining arguments.
    final decoder = FrameDecoder(); // Initialize decoder to FrameDecoder().
    final message = decoder // Initialize message to decoder.
        .add(_frame(dataType, payload.buffer.asUint8List())) // Continue the expression or parameter list from the preceding line.
        .single as DataMessage; // Complete this expression or declaration.
    expect(message.waveform[0], 1); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.waveform[1], -1); // Verify this expected result; fail the test if it is not satisfied.
    expect(message.spectrum[0], 0.5); // Verify this expected result; fail the test if it is not satisfied.
    expect(spectrumDbfs(1), closeTo(0, 1e-9)); // Verify this expected result; fail the test if it is not satisfied.
    expect(spectrumDbfs(0), -120); // Verify this expected result; fail the test if it is not satisfied.
  }); // End this block, declaration or argument list.

  test('unknown or wrongly sized frames are rejected', () { // Call test with the arguments shown here.
    expect( // Verify this expected result; fail the test if it is not satisfied.
      () => FrameDecoder().add(<int>[0xff, 0, 0, 0, 0]), // Pass this next argument/value to the call or initializer above.
      throwsFormatException, // Name this field, choice or argument: throws Format Exception.
    ); // End this block, declaration or argument list.
    expect( // Verify this expected result; fail the test if it is not satisfied.
      () => FrameDecoder().add(<int>[stateType, 0, 0, 0, 19]), // Pass this next argument/value to the call or initializer above.
      throwsFormatException, // Name this field, choice or argument: throws Format Exception.
    ); // End this block, declaration or argument list.
  }); // End this block, declaration or argument list.
} // End this block, declaration or argument list.

Uint8List _frame(int type, Uint8List payload) { // Finish the parameters/condition and enter the block below.
  final header = ByteData(5) // Initialize header to ByteData(5).
    ..setUint8(0, type) // Continue the expression or parameter list from the preceding line.
    ..setUint32(1, payload.length, Endian.big); // Complete the call above with these remaining arguments.
  return Uint8List.fromList(<int>[ // Return Uint8List.fromList(<int>[ to the caller.
    ...header.buffer.asUint8List(), // Pass this next argument/value to the call or initializer above.
    ...payload, // Pass this next argument/value to the call or initializer above.
  ]); // End this block, declaration or argument list.
} // End this block, declaration or argument list.
