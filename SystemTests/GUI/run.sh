#!/bin/sh
# Run the GUI's real Dart protocol tests from its Flutter project directory.
set -eu
TEST_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
GUI_DIR=$(CDPATH= cd -- "$TEST_DIR/../../PhonoStethos_Code/gui/phonostethos_gui" && pwd)
cd "$GUI_DIR"
exec flutter test --no-pub test/protocol_test.dart
