# GUI tests

## Automatic Dart protocol tests on the laptop

Run once from the Flutter project to obtain its dependencies, then use this folder's launcher:

```sh
cd /home/igor/Documents/Phonostethos/PhonoStethos_Code/gui/phonostethos_gui
flutter pub get
cd /home/igor/Documents/Phonostethos
sh "Test Folder/SystemTests/GUI/run.sh"
```

The launcher runs the actual `gui/phonostethos_gui/test/protocol_test.dart`, using the actual Dart protocol implementation. Expected: seven tests pass. They cover five-byte command encoding, fragmented frames, State layout, integer/fractional gains, archive file IDs, binary floats and invalid frame rejection. The Pi and server are not needed.

## Visible dashboard checks with the real Pi

Start the real server and GUI using the project deployment instructions. Check these manually:

| Action | Expected |
|---|---|
| Open GUI with reachable server | Connected state, current mode/gain and storage capability arrive. |
| Open without USB | Recording/playback storage actions are disabled as specified by the Pi. |
| Insert mounted FAT32 drive | Storage controls become available after status refresh. |
| Move gain slider to 0.5x, 0.75x, 0.8x, 1x and 2x | Labels show these multipliers; no invalid State payload/disconnection. |
| Send an operation that takes time | Conflicting controls remain disabled until its final result. |
| Delete a disposable recording, choose Cancel | No delete command is applied; the file remains. |
| Delete the same file, choose Delete | File disappears after the successful server result. |
| Play a steady known tone to the microphone from another device | Waveform updates; FFT has a visible peak near the tone if the input captures it. A 500 Hz tone corresponds to bin 64. |
| Close/reopen the GUI | Current server state is restored rather than relying on old local state. |
| Temporarily disconnect the laptop/Pi network link | Disconnection is displayed; reconnect restores current server state. |

Use the numerical FFT/DSP checks to verify calculations. The microphone/room/speaker path adds its own response and noise, so an acoustic check is not an exact amplitude calibration. Recording/playback sound checks are described in the neighbouring RecordingPlayback folder.
