# SPDX-License-Identifier: GPL-2.0
"""
trace_items.py - what this suite must cover (run.sh gates), for emu68test's
traceability, which finds the tests that cover it and writes TRACEABILITY.md.

The items, read from the sources so a new one cannot be missed:
  - the backend's entry points (the VCB_* functions of rpivid_backend.c);
  - the events a session raises (VCEV_*);
  - the errors the backend answers or fails a session with (VCERR_*).

The conformance streams need no item: test_conformance.py takes its cases
from tests/conformance/LIST itself.

KNOWN_GAPS names what no test covers yet, each with its reason; a gap that a
test covers after all fails the gate until it is taken off the list.
"""
from __future__ import annotations

import re
from pathlib import Path

from emu68test.trace import Item

TESTS = Path(__file__).resolve().parents[1]
COMPONENT_DIR = TESTS.parent
SOURCES = COMPONENT_DIR / "src"
OUTPUT = TESTS / "TRACEABILITY.md"
TITLE = "rpivid.codec"
KEPT = "hevc_d"                 # src/cmdgen/hevc_d*: Linux's files, not read for items nor gated

KNOWN_GAPS = {
    "VCERR_TRANSPORT": "needs a block that does not answer: only fault injection in the driver makes one (the user's call: none)",
}

def own_sources() -> list[Path]:
    """The backend's own C files: src/ without Linux's kept ones."""
    return sorted(path for path in SOURCES.rglob("*.[ch]") if not path.name.startswith(KEPT))


def named(pattern: str) -> list[str]:
    """Every name matching @pattern in the backend's own sources, sorted."""
    return sorted({name for path in own_sources() for name in re.findall(pattern, path.read_text(encoding="latin-1"))})


def entry_points() -> list[str]:
    """The VCB_* functions rpivid_backend.c defines."""
    text = (SOURCES / "rpivid_backend.c").read_text(encoding="latin-1")
    return re.findall(r"^[A-Za-z][^\n(]*\b(VCB_\w+)\(", text, re.M)


def items() -> list[Item]:
    """Every item the suite must cover."""
    found = [Item("The backend's entry points", name) for name in entry_points()]
    found += [Item("Events", name) for name in named(r"\bVCEV_\w+")]
    found += [Item("Errors", name) for name in named(r"\bVCERR_\w+")]
    for item in found:
        item.gap = KNOWN_GAPS.get(item.name, "")
    names = {item.name for item in found}
    unknown = [name for name in KNOWN_GAPS if name not in names]
    if unknown:
        raise ValueError("KNOWN_GAPS names no item: " + ", ".join(unknown))
    return found
