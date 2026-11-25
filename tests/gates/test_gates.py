# SPDX-License-Identifier: GPL-2.0
"""The code gates on the backend's own C (emu68test/gates.py) and traceability (trace_items.py): run.sh
gates, no machine. Linux's kept files (src/cmdgen/hevc_d*) are left out: they stay as they came. The host
tests' C (tests/host/) is the component's own and is in."""
import pytest
from emu68test import trace
from emu68test.gates import Gates

import trace_items as items

pytestmark = pytest.mark.gates

INHERITED = ""                     # no ceiling above COMPLEXITY_ALLOWED is inherited: the 0.1 code's are gone
COMPLEXITY_LIMIT = 15              # without an entry in complexity-allow.txt (the harness says 10)
COMPLEXITY_ALLOWED = 20            # the highest ceiling an entry gives (the harness says 15)
SOURCES = items.own_sources() + sorted((items.TESTS / "host").glob("*.c"))
GATES = Gates(items.COMPONENT_DIR, SOURCES, items.TESTS / "complexity-allow.txt", INHERITED,
              COMPLEXITY_LIMIT, COMPLEXITY_ALLOWED)


def test_every_function_has_its_kernel_doc():
    assert GATES.kerneldoc_findings() == []


def test_every_function_stays_within_its_complexity_limit_and_the_allow_list_only_goes_down():
    assert GATES.complexity_findings() == []


def test_every_item_has_a_test_or_is_a_known_gap_and_traceability_md_says_which():
    traced = trace.trace(items.items(), items.TESTS)
    items.OUTPUT.write_text(trace.markdown(traced, items.TITLE), encoding="utf-8")
    assert trace.uncovered(traced) == [], "no test covers:\n" + "\n".join(trace.uncovered(traced))
    assert trace.stale_gaps(traced) == [], "covered after all, take off KNOWN_GAPS:\n" + "\n".join(trace.stale_gaps(traced))
