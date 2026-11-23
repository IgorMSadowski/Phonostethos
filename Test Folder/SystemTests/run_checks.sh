#!/bin/sh
# Automatic software checks; explicit host argument selects laptop executables.
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
MODE=${1:-pi}
case "$MODE" in
    host|pi) ;;
    *) echo "Usage: $0 [pi|host]" >&2; exit 2 ;;
esac
for item in FFTW/FftwTest DSP/DspTest Pthreads/PthreadTest Protocol/ProtocolTest FileIO/FileIOTest; do
    folder=${item%/*}
    program=${item##*/}
    if [ "$MODE" = host ]; then
        executable="$TEST_DIR/$folder/build-host/$program"
    elif [ -x "$TEST_DIR/$folder/$program" ]; then
        executable="$TEST_DIR/$folder/$program"
    else
        executable="$TEST_DIR/$folder/build-pi/$program"
    fi
    if [ ! -x "$executable" ]; then
        echo "Missing executable: $executable. Build or extract the correct test package first." >&2
        exit 2
    fi
    echo "Running $folder / $program"
    "$executable"
done
echo "PASS: all five automatic software component tests"
