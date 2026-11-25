#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# rpivid.codec's hardware suite (developer-guide.md section 4.4); the host tests are tests/host/run.sh.
#
#   run.sh [pytest options]             e.g. run.sh -m smoke --server=amiga-cm4
#   run.sh gates [pytest options]       the code gates and traceability; no machine
#
# Give an option that names a directory as --build-dir=PATH, with the equals sign: pytest takes a
# separate existing path for a place to collect from, and then finds neither conftest.py nor pytest.ini.
# The hardware tests run only after the machine's owner agreed to the round.
# The suite builds on the stack's test harness, taken from its install tree: emu68-common's
# (<stack>/install/share/emu68-testing, or $EMU68_TESTING) and the vchiq library's VideoCore part
# (<stack>/install/share/vchiq-testing, or $VCHIQ_TESTING). Build the stack first.
# The first run creates tests/.venv from their requirements.txt files.
set -e
here=$(cd "$(dirname "$0")" && pwd)
share=$here/../../../install/share
harness=${EMU68_TESTING:-$share/emu68-testing}
videocore=${VCHIQ_TESTING:-$share/vchiq-testing}
if [ ! -f "$harness/emu68test/plugin.py" ] || [ ! -f "$videocore/vchiqtest/plugin.py" ]; then
    echo "run.sh: no test harness in $harness and $videocore: build the stack (scripts/docker-build.sh)" >&2
    exit 10
fi
venv="$here/.venv"
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q -r "$harness/requirements.txt" -r "$videocore/requirements.txt"
fi
export PYTHONPATH="$harness:$videocore${PYTHONPATH:+:$PYTHONPATH}"
cd "$here"
if [ "${1:-}" = gates ]; then
    shift
    exec "$venv/bin/python" -m pytest gates "$@"
fi
exec "$venv/bin/python" -m pytest --ignore=gates "$@"
