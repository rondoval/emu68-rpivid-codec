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
# The suite runs in Kinora's tree (external/rpivid-codec) on the test harness Kinora's build makes:
# emu68-common's (<kinora>/build/emu68-prefix/share/emu68-testing, or $EMU68_TESTING) and
# videocodec-library's video part (<kinora>/build/video-prefix/share/videocodec-testing, or
# $VIDEOCODEC_TESTING). Build Kinora first (scripts/docker-build.sh, then --target install). The build
# options are this repository's build directory's (<kinora>/build/rpivid-codec), the files Kinora's
# install tree's (<kinora>/install): a later --build-dir= or --install-dir= replaces them.
# The first run creates tests/.venv from their requirements.txt files.
set -e
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/../../.." && pwd)
harness=${EMU68_TESTING:-$root/build/emu68-prefix/share/emu68-testing}
videocodec=${VIDEOCODEC_TESTING:-$root/build/video-prefix/share/videocodec-testing}
if [ ! -f "$harness/emu68test/plugin.py" ] || [ ! -f "$videocodec/videocodectest/plugin.py" ]; then
    echo "run.sh: no test harness in $harness and $videocodec: build Kinora first (scripts/docker-build.sh)" >&2
    exit 10
fi
venv="$here/.venv"
if [ ! -x "$venv/bin/python" ]; then
    python3 -m venv "$venv"
    "$venv/bin/pip" install -q -r "$harness/requirements.txt" -r "$videocodec/requirements.txt"
fi
export PYTHONPATH="$harness:$videocodec${PYTHONPATH:+:$PYTHONPATH}"
cd "$here"
places="--build-dir=$root/build/rpivid-codec --install-dir=$root/install"
if [ "${1:-}" = gates ]; then
    shift
    exec "$venv/bin/python" -m pytest $places gates "$@"
fi
exec "$venv/bin/python" -m pytest $places --ignore=gates "$@"
