#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0
"""mixed_slices.py IN OUT - an Annex B stream two of whose pictures have a later slice that disagrees with the first.

From clip720_slices.h265 (four slices a picture): picture 4's second slice gets another NAL unit type (TRAIL_R for
TRAIL_N), picture 5's another TemporalId (1 for 0).  7.4.2.2 holds every slice of a picture to one NAL unit type and
every VCL unit of an access unit to one TemporalId, so the stream layer drops both pictures.  Both are TRAIL_N, which
no picture references: every other picture is shown (developer-guide.md section 4.3)."""
import sys

import broken

NAL_TRAIL_N = 0
NAL_TRAIL_R = 1
TYPE_PICTURE = 3  # picture 4, by its access unit: its second slice gets TRAIL_R
TID_PICTURE = 4   # picture 5: its second slice gets TemporalId 1
TYPE_MASK = 0x7E  # the first header byte's nal_unit_type bits
TID_MASK = 0x07   # the second header byte's nuh_temporal_id_plus1 bits


def second_slice(au: list[broken.Nal]) -> broken.Nal:
    """An access unit's second slice segment, which must be a TRAIL_N one at TemporalId 0."""
    nal = [n for n in au if n.vcl][1]
    assert nal.type == NAL_TRAIL_N and nal.data[1] & TID_MASK == 1, "not the clip720_slices this was made for"
    return nal


def main() -> int:
    if len(sys.argv) != 3:
        print(__doc__, file=sys.stderr)
        return 2
    with open(sys.argv[1], "rb") as f:
        nals, tail = broken.nal_units(f.read())
    aus = broken.access_units(nals)
    typed = second_slice(aus[TYPE_PICTURE])
    typed.data = bytes([(typed.data[0] & ~TYPE_MASK) | NAL_TRAIL_R << 1]) + typed.data[1:]
    timed = second_slice(aus[TID_PICTURE])
    timed.data = timed.data[:1] + bytes([(timed.data[1] & ~TID_MASK) | 2]) + timed.data[2:]
    with open(sys.argv[2], "wb") as f:
        f.write(broken.joined(aus, tail))
    return 0


if __name__ == "__main__":
    sys.exit(main())
