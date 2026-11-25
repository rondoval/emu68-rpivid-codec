#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0
# tests/host/make-fixtures.sh - the clips and their references
# (developer-guide.md section 4.2), into tests/fixtures/ (ignored by git):
#
#   clip1080.h265     Main, 1920x1080, 300 pictures, B pictures, a scaling list
#   clip720.h265      Main, 1280x720, 300 pictures
#   clip4k.h265       Main, 3840x2160, 48 pictures
#   clip1080_10.h265  Main10, 1920x1080, 300 pictures
#   clip4k_10.h265    Main10, 3840x2160, 24 pictures (the refusal past 1080p)
#   clip_shape.h265   clip720 then clip1080 end to end: a shape change mid-stream (test_robust.py)
#   clip1080.hvcc     FFmpeg's hvcC record of clip1080, and clip1080_bare.h265 the clip without its
#                     parameter sets (test_extradata)
#   clip720_slices.h265  Main, 1280x720, 300 pictures, four slices a picture, the parameter sets before
#                     every IRAP: the source of broken/, the damaged streams of break-fixtures.py
#   player1080.mp4, player4k.mp4  Main, 1920x1080 and 3840x2160, 300 pictures at 30 a second, encoded
#                     straight into MP4 for the players (test_players.py)
#   bars709.h265, bars709full.h265  the vchiq suite's colour bars, 1280x720, five pictures, as BT.709 in
#                     limited and in full range, each saying so in its VUI (test_colours_through_the_chain)
#
# Each with its .framemd5 and its .colmd5, the column reference of VCTest DECODE
# OUTFORMAT NV12_COL128 (colmd5.py), both from the host FFmpeg's decode; for the
# 10-bit clip FFmpeg's output narrowed to 8 bits first (narrow_ref.py), as the
# backend's pass narrows it.  MANIFEST lists what was made and from what.  The
# source is the vchiq suite's sample video ($SOURCE, or its cache).
set -euo pipefail
HERE=$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)
TOP=$(cd -- "$HERE/../.." && pwd)
FIX="$TOP/tests/fixtures"
SOURCE=${SOURCE:-"$TOP/../videocodec-library/tests/.cache/source/sample-mp4-1080p-h264-10s.mp4"}
[ -f "$SOURCE" ] || { echo "no source video at $SOURCE (the vchiq suite's material.py fetches it)" >&2; exit 1; }
mkdir -p "$FIX"
cd "$FIX"

# encode NAME SIZE FRAMES PIXFMT EXTRA...: an Annex B stream with libx265
encode() {
	local name=$1 size=$2 frames=$3 pixfmt=$4; shift 4
	[ -s "$name.h265" ] && return 0
	echo "encoding $name"
	ffmpeg -v error -y -i "$SOURCE" -frames:v "$frames" -vf "scale=$size" -pix_fmt "$pixfmt" \
		-c:v libx265 -preset medium -x265-params "bframes=4:b-adapt=2:keyint=60:min-keyint=30:log-level=error:$1" \
		-f hevc "$name.h265"
}

# references NAME SIZE: the framemd5 and the column reference, from the host FFmpeg's decode
references() {
	local name=$1 size=$2
	local raw="$name.yuv"
	[ -s "$name.framemd5" ] && { [ -s "$name.colmd5" ] || [ "$name" = clip4k_10 ]; } && return 0 # clip4k_10 is refused: no colmd5
	echo "decoding $name"
	ffmpeg -nostdin -v error -threads 1 -i "$name.h265" -f rawvideo -y "$raw"
	ffmpeg -nostdin -v error -i "$name.h265" -f framemd5 -y "$name.framemd5"
	if [ "$name" = "clip1080_10" ]; then
		python3 -I "$HERE/narrow_ref.py" "$raw" "$size" "$name.narrow8"
		python3 -I "$HERE/colmd5.py" "$name.narrow8" "$size" "$name.colmd5" # the column reference of the narrowed frames
	fi
	case "$name" in
	*_10) ;;
	*) python3 -I "$HERE/colmd5.py" "$raw" "$size" "$name.colmd5" ;; # the column reference for VCTest (8-bit clips)
	esac
	rm -f "$raw"
}

encode clip1080    1920x1080 300 yuv420p    "scaling-list=default"
encode clip720     1280x720  300 yuv420p    "scaling-list=off"
encode clip4k      3840x2160 48  yuv420p    "scaling-list=off"
encode clip1080_10 1920x1080 300 yuv420p10le "profile=main10"
encode clip4k_10   3840x2160 24  yuv420p10le "profile=main10"

references clip1080    1920x1080
references clip720     1280x720
references clip4k      3840x2160
references clip1080_10 1920x1080
references clip4k_10   3840x2160

# the shape change: two clips end to end, and their references the same way (VCTest reads the hashes in order)
cat clip720.h265 clip1080.h265 > clip_shape.h265
cat clip720.colmd5 clip1080.colmd5 > clip_shape.colmd5

# the extradata as an hvcC record, and the stream that needs it
[ -s clip1080.hvcc ] || python3 -I "$HERE/hvcc_fixture.py" clip1080.h265 clip1080.hvcc clip1080_bare.h265

# the damaged streams: their source, its column reference, the damages and their expectations, checked against
# FFmpeg's decodes
encode clip720_slices 1280x720 300 yuv420p "scaling-list=off:slices=4:repeat-headers=1"
if [ ! -s clip720_slices.colmd5 ]; then
	ffmpeg -nostdin -v error -i clip720_slices.h265 -f rawvideo -pix_fmt yuv420p -y clip720_slices.yuv
	python3 -I "$HERE/colmd5.py" clip720_slices.yuv 1280x720 clip720_slices.colmd5
	rm -f clip720_slices.yuv
fi
python3 -I "$HERE/break-fixtures.py" --check

# bars NAME RANGE: the vchiq suite's colour-bar picture as BT.709 of that range (tv or pc), the stream saying so
bars() {
	local name=$1 range=$2
	[ -s "$name.h265" ] && return 0
	echo "encoding $name"
	python3 - "$TOP/../videocodec-library/tests" <<'PATTERN' |
import sys
sys.path.insert(0, sys.argv[1])
from videocodectest.pictures import pattern
sys.stdout.buffer.write(pattern(1280, 720)[0].tobytes() * 5)
PATTERN
	ffmpeg -nostdin -v error -y -f rawvideo -pix_fmt rgb24 -s 1280x720 -r 25 -i - \
		-vf "scale=out_color_matrix=bt709:out_range=$range,format=yuv420p" -c:v libx265 -crf 1 \
		-x265-params "log-level=error" -colorspace bt709 -color_primaries bt709 -color_trc bt709 -color_range "$range" \
		-f hevc "$name.h265"
}
bars bars709     tv
bars bars709full pc

# player NAME SIZE: H.265 encoded straight into MP4 at exactly 30 pictures a second, as an encoder writes it;
# a raw stream remuxed carries decode-order timestamps, which a player shows as stutter
player() {
	local name=$1 size=$2
	[ -s "$name.mp4" ] && return 0
	echo "encoding $name"
	ffmpeg -nostdin -v error -y -i "$SOURCE" -an -vf "fps=30,scale=$size" -frames:v 300 -pix_fmt yuv420p \
		-c:v libx265 -preset fast -tag:v hvc1 -x265-params "keyint=60:log-level=error" "$name.mp4"
}
player player1080 1920x1080
player player4k   3840x2160

{
	echo "# made $(date -u +%Y-%m-%dT%H:%MZ) by tests/host/make-fixtures.sh from $(basename "$SOURCE") $(sha256sum "$SOURCE" | cut -c1-16)"
	echo "# ffmpeg: $(ffmpeg -version | head -1 | cut -d' ' -f3)"
	for f in *.h265 *.hvcc *.mp4 *.framemd5 *.colmd5 *.narrow8 broken/*.h265 broken/EXPECT; do # clip_shape has no framemd5 of its own
		[ -f "$f" ] && printf '%s %12d %s\n' "$(sha256sum "$f" | cut -c1-16)" "$(stat -c %s "$f")" "$f"
	done
} > MANIFEST
cat MANIFEST
