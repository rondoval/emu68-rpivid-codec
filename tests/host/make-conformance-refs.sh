#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# make-conformance-refs.sh - a conformance stream's column reference (colmd5) for
# test_conformance.py, made again or for a stream new to tests/conformance/LIST:
# FFmpeg's output of the stream in the block's 8-bit column layout, hashed over the
# conformance window (colmd5.py); a Main10 stream's output first truncated to 8 bits
# as the backend's pass does (narrow_ref.py).  The window's place comes from the
# stream's SPS (stream_check --verbose).  Reads the stream from
# tests/fixtures/conformance/ (fetch-conformance.sh), writes <name>.colmd5 into
# tests/conformance/, beside LIST.
#
# The references there are FFmpeg 5.1's, and the hardware decodes every stream bit
# exact against them (the SAODBLK pair aside: test_conformance.py).  A later FFmpeg
# need not agree: 8.0 loses reference pictures of NUT_A_ericsson_5 and
# RPS_D_ericsson_6 ("Error constructing the frame RPS").  Hold a new reference to the
# hardware's decode before keeping it.
#
#   make-conformance-refs.sh name ...        the named streams (FFMPEG= picks the FFmpeg)
set -e
HERE=$(cd "$(dirname "$0")" && pwd)
CONF="$HERE/../fixtures/conformance"
REFS="$HERE/../conformance"
FFMPEG=${FFMPEG:-ffmpeg}
TMP=${TMPDIR:-/tmp}/conformance-refs.$$
mkdir -p "$TMP"
trap 'rm -rf "$TMP"' EXIT
make -C "$HERE" -s out/stream_check

one()
{
	name=$1 w=$2 h=$3 fmt=$4 pictures=$5
	window=$("$HERE/out/stream_check" --verbose "$CONF/$name.h265" 2>&1 | sed -n 's/^sps [0-9]*: .* window \([0-9]*\),\([0-9]*\),.*/\1,\2/p' | sort -u)
	[ "$(echo "$window" | wc -l)" = 1 ] || { echo "$name: more than one window ($window), the first taken"; window=$(echo "$window" | head -1); }
	raw="$TMP/$name.raw"
	"$FFMPEG" -nostdin -v error -threads 1 -flags unaligned -i "$CONF/$name.h265" -fps_mode passthrough -f rawvideo -pix_fmt "$fmt" -y "$raw" 2>&1 |
		sed "s/^/$name: /"
	if [ "$fmt" = yuv420p10le ]; then
		python3 -I "$HERE/narrow_ref.py" "$raw" "${w}x$h" "$raw.8"
		raw="$raw.8"
	fi
	python3 -I "$HERE/colmd5.py" "$raw" "${w}x$h" "$REFS/$name.colmd5" --at "$window" | sed "s/^/$name: /"
	got=$(grep -vc '^#' "$REFS/$name.colmd5")
	[ "$got" = "$pictures" ] || echo "$name: $got pictures hashed, LIST says $pictures"
	rm -f "$TMP/$name.raw" "$TMP/$name.raw.8"
}

[ $# -gt 0 ] || { echo "usage: make-conformance-refs.sh name ..." >&2; exit 2; }
for name in "$@"; do
	one $(awk -v n="$name" '!/^#/ && $1==n {print $1, $2, $3, $4, $5}' "$REFS/LIST")
done
