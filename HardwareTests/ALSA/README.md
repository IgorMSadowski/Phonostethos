# ALSA test

Files: `AlsaTest.c` and `Makefile`. This has the same list/rec/play style as the original AlsaTest, with checked WAV reads/writes. It tests audio independently of the full application.

Build on the laptop: `make` from this folder. Output: `build-pi/AlsaTest`. For a laptop build use `make native` and `build-host/AlsaTest`.

After extracting the Pi package:

```sh
cd /tmp/phonostethos-tests/HardwareTests/ALSA
./AlsaTest list
./AlsaTest rec /tmp/mic-test.wav 5 'plughw:CARD=HiFiAudio,DEV=0'
./AlsaTest play /tmp/mic-test.wav 'plughw:CARD=Headphones,DEV=0'
echo $?
```

Stop the real server first. During those five seconds, speak or tap gently near the microphone. Use your actual PCM names if they differ. A repeated recording needs a new filename; existing files are refused.

Expected: 40000/40000 captured samples, successful WAV finalization, then your recorded sound on headphones. Peak/RMS show signal level in PCM16 units; a nonzero value can also be noise. A peak near 32768 suggests clipping. The command's PASS checks I/O, not sound quality.

To isolate output from the microphone, first run USB/UsbTest to create its known 500 Hz tone, then play that file. One second of a steady tone should be audible. Use your normal low listening volume for the first tone test.

Main APIs: snd_device_name_hint lists PCM names; snd_pcm_open/set_params configures a stream; snd_pcm_readi/writei transfer frames and can return partial counts; snd_pcm_recover handles recoverable errors; snd_pcm_drain finishes output before close. libsndfile reads/writes the WAV header correctly.

Troubleshooting: Device busy -> stop the other server/audio process. No such file/device -> check /proc/asound/cards and PCM names. PASS with silence -> check input selection, wiring, mixer level and listen to the WAV. On the laptop, PCM `null` verifies calls only and does not capture/play physical sound.

API reference: https://www.alsa-project.org/alsa-doc/alsa-lib/group___p_c_m.html
