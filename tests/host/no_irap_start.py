#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""no_irap_start.py IN OUT - an Annex B stream whose first picture is gone, its parameter sets kept.

What a player sends that starts in the middle of a stream it has the codec configuration of: the stream
layer's first picture is then no IRAP picture (developer-guide.md section 4.3)."""
import sys

import broken


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        nals, tail = broken.nal_units(f.read())
    aus = broken.access_units(nals)
    aus[0] = [nal for nal in aus[0] if not nal.vcl]
    with open(sys.argv[2], "wb") as f:
        f.write(broken.joined(aus, tail))
    return 0


if __name__ == "__main__":
    sys.exit(main())
