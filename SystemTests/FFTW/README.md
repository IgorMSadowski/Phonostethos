# FFTW library test

Files: `FftwTest.c` and `Makefile`. Build with `make`; Pi executable: `build-pi/FftwTest`.

On the packaged Pi:

```sh
cd /tmp/phonostethos-tests/SystemTests/FFTW
./FftwTest
```

Expected: `Peak: bin 128, 1000.00 Hz, amplitude 1.0000`, then PASS. No microphone, headphones or drive is needed.

The input is a generated 1000 Hz sine, amplitude 1, sampled at 8000 Hz. In a 1024-point FFT the spacing is 8000/1024 = 7.8125 Hz; 1000 Hz is exactly bin 128. A real-input transform produces 513 complex outputs. This direct library test uses no Hann window because the tone fits an exact FFT bin.

APIs: fftwf_plan_dft_r2c_1d creates the single-precision plan; fftwf_execute runs it; fftwf_destroy_plan releases it. The program scales the raw positive-frequency magnitude by 2/1024 and checks the expected amplitude. SystemTests/DSP separately checks the project's actual windowed FFT.

Reference: https://www.fftw.org/fftw3_doc/One_002dDimensional-DFTs-of-Real-Data.html
