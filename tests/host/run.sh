#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# run.sh - the host tests (developer-guide.md section 4.3): the unit tests and the
# clips through make check, then every conformance stream tests/conformance/LIST
# names through stream_check: one the backend takes with the pictures FFmpeg
# showed in a right output order, one it refuses by design refused as planned
# (test_refusals).  Needs the fixtures (make-fixtures.sh, fetch-conformance.sh);
# without the conformance streams only the clips run.  Exit 0 when everything passed.
#
#   tests/host/run.sh [--conformance-only] [NAME...]
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
LIST="$HERE/../conformance/LIST"
CONF="$HERE/../fixtures/conformance"
OUT="$HERE/out/conformance"
only=0
if [ "${1:-}" = "--conformance-only" ]; then
	only=1
	shift
fi
if [ $only -eq 0 ]; then
	make -C "$HERE" check || exit 1
else
	make -C "$HERE" all >/dev/null || exit 1
fi
ls "$CONF"/*.h265 >/dev/null 2>&1 || { echo "no conformance streams (tests/host/fetch-conformance.sh)"; exit 0; }
mkdir -p "$OUT"
passed=0; failed=0; failed_names=""
while read -r name w h fmt frames expect source sha; do
	case "$name" in \#*|"") continue ;; esac
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*) ;; *) continue ;; esac
	fi
	log="$OUT/$name.log"
	if [ "$expect" = refuse ]; then
		"$HERE/out/stream_check" --expect-refusal "$CONF/$name.h265" > "$log" 2>&1
	elif [ "$frames" = - ]; then
		"$HERE/out/stream_check" "$CONF/$name.h265" > "$log" 2>&1
	else
		"$HERE/out/stream_check" --frames "$frames" "$CONF/$name.h265" > "$log" 2>&1
	fi
	if [ $? -eq 0 ]; then
		passed=$((passed + 1))
		echo "ok   $name: $(tail -1 "$log")"
	else
		failed=$((failed + 1)); failed_names="$failed_names $name"
		echo "FAIL $name: $(grep -v '^stream_check:' "$log" | head -1) [$(tail -1 "$log")]"
	fi
done < "$LIST"
echo "conformance: $passed passed, $failed failed$([ $failed -gt 0 ] && echo ":$failed_names")"
[ $failed -eq 0 ]
