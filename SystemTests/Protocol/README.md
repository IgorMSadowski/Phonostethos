# Binary protocol test

Files: `ProtocolTest.cpp` and `Makefile`. This includes the project's actual protocol.h. Build with `make`; output: `build-pi/ProtocolTest`.

On the packaged Pi:

```sh
cd /tmp/phonostethos-tests/SystemTests/Protocol
./ProtocolTest
```

Expected: four PASS messages for commands, State/gain codes, combined frames and archive IDs. No socket or GUI is needed.

The test checks:

- SET_GAIN 0.5x is exactly five bytes: `02 00 00 00 65` (101 decimal = 0x65).
- A four-byte incomplete command cannot be decoded as a complete one.
- Split State headers/payloads stay buffered until complete; integer counters use big-endian byte order.
- Integer and fractional gain codes survive State encoding/decoding; unknown codes are rejected.
- Status has its fixed five-byte layout and can be followed immediately by a length-prefixed CommandResult.
- AcceptedPending remains distinct from Completed; archive entries retain the server's file ID.

If it exits 1, the last PASS line identifies which earlier stages succeeded; inspect the next block in ProtocolTest.cpp. This checks the C++ implementation. SystemTests/GUI separately tests Dart decoding of the same layouts.
