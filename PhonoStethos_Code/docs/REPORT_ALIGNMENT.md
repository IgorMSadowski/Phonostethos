# Report and implementation audit

Reference: the locally saved report extract dated November 1, plus the recording synchronization and GUI command flowcharts supplied in chat. This comparison covers the simplified source copy. Later Google Docs edits require a fresh comparison.

## What the implementation follows

| Design | Code |
|---|---|
| Four execution contexts and pthread scheduling | Main is the initial thread. AudioDSP, FileIO and Network use pthreads. Main and Network request SCHED_OTHER with nice 0. Audio and FileIO request FIFO priorities 80 and 50. Check actual settings on the Pi. |
| Separate WAV and audio ownership | FileIoManager owns SNDFILE handles and AudioEngine owns normal ALSA processing. Main sends requests and receives results. AudioEngine's maintenance stop can interrupt capture to unblock joining. |
| Recording synchronization | FileIO reports RecordingOpened before Main enables recording. AudioDSP pushes processed samples. FileIO pops and writes outside the buffer lock. Stop closes the gate and drains the buffer before completing. |
| Playback synchronization | FileIO fills a separate buffer and AudioDSP consumes it. Main starts playback after FileIO readiness and capture suspension. EOF lets AudioDSP consume the remaining samples. |
| TCP and queues | Five-byte commands, control replies before waiting telemetry, one waiting telemetry snapshot, a single TCP writer, partial byte handling, and server-issued file IDs. |
| DSP and telemetry | 8 kHz mono PCM16, 512-sample blocks, 1024-sample rolling FFT with a Hann window, 513 FFT outputs and 512 transmitted bins. |
| LED interface | One byte per mask via the character device. GPIO 17/27/24/22 and a separate FF pulse command. |
| USB ejection | FileIO closes files, checks syncfs and umount2, then reports the result. Safe USB ejection applies to that filesystem. |
| GUI command state | The Flutter app tracks one pending command and consumes CommandResult and State. This is present in code. Verify the visible behavior on the Pi. |

## Differences and open verification

| Area | Difference or verification still needed |
|---|---|
| Controller signatures | The saved report describes startPlayback() with a retained argument. This copy uses startPlayback(file_id) and listFiles(cursor) so the inputs are explicit. The dispatch boxes should pass cmd.value. |
| TCP decoding | The report explicitly names ntohl(). The implementation decodes the same big-endian bytes with read_u32_be(). Wire behavior is equivalent, but the API label differs. |
| File listing | getFileList returns a sorted vector of WAV names as described. Main assigns file IDs and paginates the network response. |
| Filter description | The code uses a second-order high-pass followed by a second-order low-pass, fourth order overall. Any report or slide describing the whole cascade as second order needs that distinction. |
| File-name uniqueness | FileIO uses O_EXCL and never overwrites an existing recording. A collision fails the open. If the design promises automatic selection of another suffix on collision, that retry is missing. |
| GUI send failure | Socket.add queues bytes and flush returns a Future. The current _request does not await it. The receive/disconnect path clears pending state, but the flowchart's send-success decision is not implemented as a separate awaited check. |
| Complete FileIO flowchart | The recording flowchart does not describe the implemented playback producer. The missing path is open WAV, read available space, push to playback buffer, signal EOF, and close the file. |
| Root filesystem and power removal | The saved report leaves read-only root/volatile runtime writes to implementation. Confirm the actual Buildroot image configuration before claiming that unplugging is safe for the Pi filesystem. |
| Target behavior | Build success does not measure hardware timing, ALSA device availability, LED wiring, USB mount behavior, or GUI reconnect behavior. Use the deployment checklist on the Pi. |

The comparison is not a 100% match. The deployment guide is for testing this build. It does not certify the report or hardware.
