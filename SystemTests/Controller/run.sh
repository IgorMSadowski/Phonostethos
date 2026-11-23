#!/bin/sh
# Run the canonical end-to-end test instead of keeping a second copy of its logic.
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
PROJECT_DIR=$(CDPATH= cd -- "$TEST_DIR/../../PhonoStethos_Code" && pwd)
SERVER=${1:-"$PROJECT_DIR/app/build-host/phonostethos-server"}
if [ ! -x "$SERVER" ]; then
    echo "Build the native server first, or pass its executable path:" >&2
    echo "sh $0 /absolute/path/to/phonostethos-server" >&2
    exit 2
fi
exec python3 "$PROJECT_DIR/app/tests/controller_tcp_test.py" "$SERVER"
