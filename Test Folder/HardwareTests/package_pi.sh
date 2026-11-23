#!/bin/sh
# Build and package diagnostics only. Does not install anything or connect to a Pi.
set -eu
HARDWARE_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
ROOT_DIR=$(CDPATH= cd -- "$HARDWARE_DIR/.." && pwd)
make -C "$HARDWARE_DIR"
make -C "$ROOT_DIR/SystemTests"
BUNDLE="$HARDWARE_DIR/pi-bundle"
mkdir -p "$BUNDLE"
for item in HardwareTests/ALSA/AlsaTest HardwareTests/USB/UsbTest HardwareTests/LEDs/LedTest HardwareTests/Udev/UdevTest HardwareTests/TCP/TcpTest HardwareTests/Scheduling/SchedulerTest SystemTests/FFTW/FftwTest SystemTests/DSP/DspTest SystemTests/Pthreads/PthreadTest SystemTests/Protocol/ProtocolTest SystemTests/FileIO/FileIOTest; do
    folder=${item%/*}
    program=${item##*/}
    mkdir -p "$BUNDLE/$folder"
    cp "$ROOT_DIR/$folder/build-pi/$program" "$BUNDLE/$item"
    cp "$ROOT_DIR/$folder/README.md" "$BUNDLE/$folder/README.md"
    echo "Packaged $item"
done
for folder in SystemTests/RecordingPlayback SystemTests/ErrorHandling; do
    mkdir -p "$BUNDLE/$folder"
    cp "$ROOT_DIR/$folder/README.md" "$BUNDLE/$folder/README.md"
done
cp "$ROOT_DIR/SystemTests/run_checks.sh" "$BUNDLE/SystemTests/run_checks.sh"
cp "$HARDWARE_DIR/PI_RUN.md" "$BUNDLE/README.md"
tar -czf "$HARDWARE_DIR/phonostethos-tests-pi.tar.gz" -C "$BUNDLE" README.md HardwareTests SystemTests
echo "Ready: $HARDWARE_DIR/phonostethos-tests-pi.tar.gz"
