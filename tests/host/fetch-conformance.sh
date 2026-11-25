#!/bin/sh
# SPDX-License-Identifier: GPL-2.0
# fetch-conformance.sh - the JCT-VC HEVC_v1 conformance streams of test_conformance.py
# (developer-guide.md section 4.2) into tests/fixtures/conformance/ (ignored by git): each
# stream tests/conformance/LIST names, from the zip of ITU's HEVC_v1 directory, its
# bitstream unpacked as <name>.h265 and held to LIST's SHA-256: the references beside
# LIST are made from exactly these files.  Zips stay in zips/; nothing is fetched twice.
#
# ITU's server answers a burst of requests with an HTML page ("Request Rejected"):
# there is a pause between zips, and a file that is not a zip is dropped.
#
#   tests/host/fetch-conformance.sh [NAME...]     (every stream of LIST when none named)
#
# Needs: curl, unzip, sha256sum.
set -u
HERE=$(cd "$(dirname "$0")" && pwd)
TOP=$(cd "$HERE/../.." && pwd)
LIST="$TOP/tests/conformance/LIST"
DIR="$TOP/tests/fixtures/conformance"
SITE="https://www.itu.int/wftp3/av-arch/jctvc-site/bitstream_exchange/draft_conformance/HEVC_v1/"
AGENT="Mozilla/5.0 (X11; Linux x86_64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/120.0 Safari/537.36"
mkdir -p "$DIR/zips"
cd "$DIR" || exit 1

fetch() {
	curl -s -f -L -A "$AGENT" -e "https://www.itu.int/" --retry 2 --retry-delay 5 "$@"
}

# unpack NAME: NAME.h265 from its zip
unpack() {
	local name=$1 zip="zips/$1.zip" bit
	if [ ! -s "$zip" ]; then
		sleep 2
		fetch "$SITE$name.zip" -o "$zip" && unzip -tq "$zip" >/dev/null 2>&1 || { rm -f "$zip"; return 1; }
	fi
	bit=$(unzip -Z1 "$zip" | grep -i -E '\.(bit|bin)$' | grep -v -i '__MACOSX' | head -1)
	[ -n "$bit" ] && unzip -p "$zip" "$bit" > "$name.h265"
}

grep -v '^#' "$LIST" | while read -r name w h fmt pictures expect source sha; do
	if [ $# -gt 0 ]; then
		case " $* " in *" $name "*) ;; *) continue ;; esac
	fi
	if [ ! -s "$name.h265" ] || [ "$(sha256sum "$name.h265" | cut -c1-16)" != "$sha" ]; then
		echo "fetching $name"
		rm -f "$name.h265"
		unpack "$name" || { echo "$name: the fetch failed" >&2; rm -f "$name.h265"; continue; }
	fi
	got=$(sha256sum "$name.h265" | cut -c1-16)
	[ "$got" = "$sha" ] || { echo "$name: SHA-256 $got, LIST says $sha: not the stream the references were made from" >&2; rm -f "$name.h265"; }
done
missing=$(grep -v '^#' "$LIST" | while read -r name rest; do [ -s "$name.h265" ] || echo "$name"; done)
if [ -n "$missing" ]; then
	echo "not fetched:" $missing
	exit 1
fi
echo "$(grep -vc '^#' "$LIST") streams in $DIR"
