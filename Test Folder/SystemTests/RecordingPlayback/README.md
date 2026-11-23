# Complete recording / playback test on the Pi

This is a manual system test using the real server, GUI, microphone, headphones, LEDs and FAT32 pen drive together. It supplements the standalone tests. Record the observed result beside each step.

Preparation: HardwareTests/ALSA and USB pass; the drive is mounted at /media/usb_storage; the correct LED module is loaded; the real server and laptop GUI are connected. Use a short disposable recording and normal listening volume. The usual server command is in PhonoStethos_Code/docs/PI_TUTORIAL.md.

| Step | What to do | Expected result |
|---|---|---|
| 1 | Select General, gain 1x; listen and speak near the microphone. | Live sound, moving waveform and matching mode LED. |
| 2 | Press Record; speak/tap for about five seconds. | GUI reports recording only after the WAV is opened; recording LED turns on. |
| 3 | Press Stop; wait for completion. | Accepted samples are drained and the WAV closed; recording LED goes off; live listening continues. |
| 4 | Open/refresh the archive. | A new WAV with the selected initial mode/date/time/suffix appears. |
| 5 | Play that recording to the end. | Recorded content is heard, then the system returns to live capture and responds to controls. |
| 6 | During another playback choose 0.5x, 0.75x, 0.8x, 1x, 2x. | Playback loudness decreases/increases accordingly; the stored file itself is unchanged. |
| 7 | Play again and press Stop Playback midway. | Playback stops and microphone capture resumes after worker cleanup. |
| 8 | Make another short recording and stop at a different moment. | No stale sound from the previous session; file begins/ends with the new recording. |
| 9 | Cancel deletion once, then confirm deletion of the disposable file. | Cancel leaves it present; confirmed successful deletion removes it. |
| 10 | Press Eject and wait for completion. Check `mount | grep '/media/usb_storage'`. | The volume is no longer mounted; storage controls are disabled. Remove the pen drive afterwards. |

Also check the real server's startup log for affinity/FIFO errors. HardwareTests/Scheduling only establishes that a separate test process can request the settings.

If a step fails, isolate it: known tone -> ALSA output, microphone WAV -> ALSA input, USB round-trip -> filesystem/WAV, FileIOTest -> worker/buffers, Controller test -> coordination, GUI automatic tests -> framing/decoding. Keep the server log and exact action that triggered the failure.

The files created by the full server have already had live filtering/gain applied. Playback bypasses the live band-pass filter but applies the current gain again before headphones/FFT. Test playback gain using a moderately recorded signal so clipping does not hide the change.
