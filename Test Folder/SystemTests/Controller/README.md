# Main + worker coordination test

This folder provides a simple `run.sh` entry point for the project's existing complete-server test. Its single implementation is `PhonoStethos_Code/app/tests/controller_tcp_test.py`; the launcher reuses that file so the two copies cannot drift apart.

## Run on the laptop

Install the native development packages listed in HardwareTests/README.md, then build the native server from the checkout:

```sh
cd /home/igor/Documents/Phonostethos
cmake -S PhonoStethos_Code/app -B PhonoStethos_Code/app/build-host -DBUILD_TESTING=ON
cmake --build PhonoStethos_Code/app/build-host --parallel
sh "Test Folder/SystemTests/Controller/run.sh"
```

To use another native executable: `sh "Test Folder/SystemTests/Controller/run.sh" /absolute/path/to/phonostethos-server`.

Expected: a final PASS for gain codes, TCP fragments, playback/EOF/live transition and archive commands. Python 3 and the native ALSA/libsndfile/FFTW/libudev libraries are required. It chooses an unused loopback port and uses ALSA `null`, a temporary directory and a regular file in place of the LED device. It stops its own server afterwards.

The test follows real commands through Network -> CommandQueue -> Main -> worker mailboxes -> worker results -> ControlReplyQueue -> Network. It tests supported gain codes, partial commands, invalid gain rejection, archive IDs, playback start/EOF/return to live, a later command after EOF, stale IDs and deletion.

The EOF fixture is an empty valid WAV. That isolates the drain/prepare/control transition because ALSA null does not pace playback in real time. SystemTests/FileIO separately transfers a nonempty WAV exactly. Neither test verifies physical sound or real Pi scheduling.

The canonical controller test is also part of `ctest --test-dir PhonoStethos_Code/app/build-host --output-on-failure`. Tests compiled for ARM cannot run as native laptop programs. This Python launcher is intended for the laptop/full source checkout and is not included in the Pi-only binary package.
