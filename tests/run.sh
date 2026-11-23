#!/bin/sh
set -eu
cd "$(dirname "$0")/.."
build_dir=$(mktemp -d "${TMPDIR:-/tmp}/feberis-tests.XXXXXX")
trap 'rm -rf "$build_dir"' EXIT HUP INT TERM
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g protocol.c tests/test_protocol.c -o "$build_dir/protocol"
mkdir -p "$build_dir/sd/apps_data"
cc -DFBR_TEST_ROOT=\"$build_dir/sd\" -std=c11 -Wall -Wextra -Werror -Wno-unused-function -fsanitize=address,undefined -g protocol.c tests/test_session.c -o "$build_dir/session"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g ui_model.c tests/test_ui.c -o "$build_dir/ui"
"$build_dir/protocol"
"$build_dir/session"
"$build_dir/ui"
python3 tests/check_exports.py "$build_dir/sd"
