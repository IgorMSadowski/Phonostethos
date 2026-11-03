# PhonoStethos Flutter app

Flutter desktop client for the Raspberry Pi service. It displays the processed
waveform and FFT, selects GENERAL/HEART/LUNG profiles and gain, and controls
recording, playback, archive deletion, and safe storage eject.

The client connects to `10.42.0.3:8080`. Configure the laptop's USB Ethernet
interface as `10.42.0.10/24`. The GUI permits only one command awaiting its
final `CommandResult`, and uses the server-issued archive IDs rather than
filenames or list positions in control requests.

```sh
cd PhonoStethos_Code/gui/phonostethos_gui
flutter pub get
flutter analyze
flutter test
flutter run -d linux
```

The parser rejects unknown frame types, malformed frame lengths, invalid UTF-8,
non-finite telemetry, and invalid state/results before updating the display.
The FFT chart converts linear peak amplitudes to dBFS with a -120 dBFS floor.
These plots are not calibrated acoustic measurements and do not diagnose
medical conditions.
