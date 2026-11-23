# Project DSP test

Files: `DspTest.cpp` and `Makefile`. C++ is used here to call the real DspProcessor class; the test contains straightforward functions, arrays and loops. Its Makefile links the actual project source.

Build with `make`; output: `build-pi/DspTest`. On the packaged Pi:

```sh
cd /tmp/phonostethos-tests/SystemTests/DSP
./DspTest
```

It prints the measured response at 5, 50, 200, 500, 1500 and 3000 Hz for General, Heart and Lung. Approximately 0 dB means a tone passes; negative dB means attenuation. It ignores the initial settling samples before measuring RMS.

Expected automatic checks:

- All profiles retain their 200 Hz passband tone.
- General reduces 5 and 3000 Hz, Heart reduces 1500 Hz, Lung reduces 50 Hz by at least 20 dB.
- Gain codes 101/102/103 turn sample 1000 into 500/750/800; codes 1/2/20 give 1000/2000/20000.
- Excessive positive/negative gain clamps at 32767/-32768.
- The project's rolling FFT waits for 1024 samples, then the 500 Hz tone appears at bin 64 with magnitude about 0.25 after Hann-window normalization.

PASS here checks known numerical inputs against the project's actual methods. It does not test the microphone, acoustic noise, headphone sound, the gain-change ramp in AudioEngine or GUI visibility. Use the real-system procedure for those.

Filter coefficient reference: https://www.w3.org/TR/audio-eq-cookbook/
