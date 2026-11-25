# SPDX-License-Identifier: GPL-2.0
"""
conftest.py - the hardware suite of rpivid.codec (developer-guide.md section
4.4): emu68-common's harness (emu68test) and videocodec-library's video part
of it (videocodectest), both from Kinora's build, with this backend's
module added to the files the install replaces, the material taken from the
checked-in references (tests/conformance/) and tests/fixtures/ (made by
tests/host/make-fixtures.sh and fetch-conformance.sh), and this component's
own baselines (tests/baselines.json).
"""
from __future__ import annotations

from pathlib import Path

import pytest

pytest.register_assert_rewrite("emu68test", "videocodectest")    # before the first import: their asserts explain themselves

from emu68test import plugin  # noqa: E402
from videocodectest import suite  # noqa: E402
from videocodectest.material import Material, MaterialError  # noqa: E402

TESTS = Path(__file__).resolve().parent
FIXTURES = TESTS / "fixtures"
REFERENCES = TESTS  # tests/conformance/: the conformance set's LIST and references, checked in
MODULE = ("LIBS/videocodec/rpivid.codec", "LIBS:videocodec/rpivid.codec")

plugin.configure(suite.suite("rpivid.codec", "rpivid-codec", TESTS))
if MODULE not in suite.RESIDENT:
    suite.RESIDENT.append(MODULE)   # this backend's module goes with the install, beside videocodec-library's files
pytest_plugins = ["emu68test.plugin", "videocodectest.plugin"]


def pytest_addoption(parser):
    g = parser.getgroup("rpivid", "rpivid.codec's suite")
    g.addoption("--kinora", default=None,
                help="the Kinora program on the Amiga for test_players.py (default: SYS:Utilities/Kinora/Kinora, where its "
                     "Installer puts it); those tests skip without it")
    g.addoption("--archive", default=None,
                help="Kinora's .lha for test_install (default: the newest in Kinora's build/package/)")


class Fixtures(Material):
    """The material of this suite: a checked-in reference (conformance/<name>.colmd5) first, else the file of
    tests/fixtures/, made ahead by make-fixtures.sh and fetch-conformance.sh."""

    def path(self, name: str) -> Path:
        for root in (REFERENCES, self.cache):
            if (root / name).is_file():
                return root / name
        raise MaterialError(f"{name}: not in {self.cache} (tests/host/make-fixtures.sh, fetch-conformance.sh)")


@pytest.fixture(scope="session")
def material(device) -> Material:
    """The clips, streams and references, uploaded to RAM:hw when first used."""
    return Fixtures(FIXTURES, device)
