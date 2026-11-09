"""The S32K358 platform file: the blocks read off a chip, the pads off the board's
schematic and the IOMUX workbook."""

import pytest
from test_trace_arm import FakeProbe, order

from cortrace import platdesc, trace_arm
from cortrace.osplugins import Watch


@pytest.fixture(name="k358")
def k358_fixture():
    return platdesc.load("s32k358")


class GatingProbe(FakeProbe):
    """MC_CGM mux 11: CS follows the CG bit, DIV_STAT clears only when it runs."""

    def __init__(self):
        super().__init__({0xE004100C: 1})

    def write32(self, addr, value):
        super().write32(addr, value)
        if addr == 0x402D85C0:  # MUX_11_CSC
            self.regs[0x402D85C4] = 0 if value & 0x4 else 0x20000
        if addr == 0x402D85C8:  # MUX_11_DC_0: sampled only on a running clock
            self.regs[0x402D85FC] = 1 if self.regs[0x402D85C4] == 0 else 0


def armed(k358):
    probe = GatingProbe()
    trace_arm.arm(probe, k358, [Watch("g_x", 4)], log=lambda _m: None)
    return probe


def test_the_path_is_the_one_read_off_the_chip(k358):
    probe = armed(k358)
    chain = (0x4024A000, 0x40247000, 0x40243000, 0x40244000, 0x40241000)
    touched = []
    for addr, _ in probe.writes:
        base = addr & ~0xFFF
        if base in chain and base not in touched:
            touched.append(base)
    assert touched == list(chain)
    assert order(probe, 0x40243000) == [0x301]
    assert order(probe, 0x40241000) == [0x303]
    assert order(probe, 0x4024A004) == [0x8]


def test_trace_clock_runs_from_firc_and_the_divider_is_written_on_a_running_clock(
    k358,
):
    probe = armed(k358)
    # gate, select FIRC (SELCTL 0), ungate
    assert order(probe, 0x402D85C0) == [0x4, 0x4, 0x0]
    assert order(probe, 0x402D85C8) == [0x80010000]  # DE, DIV = 1
    clock = [a for a, _ in probe.writes if 0x402D85C0 <= a <= 0x402D85FC]
    assert clock.index(0x402D85C8) == len(clock) - 1  # after the last CSC write


def test_pads_are_trace_alt7_on_the_j50_balls(k358):
    probe = armed(k358)
    for gpio in (198, 199, 207, 208, 191):  # PTG6, PTG7, PTG15, PTG16, PTF31
        (value,) = order(probe, 0x40290000 + 0x240 + 4 * gpio)  # SIUL2 MSCRn
        assert value == 0x0020_0107  # ALT7, OBE, DSE, SRC 0


def test_blocks_and_widths(k358):
    blocks = k358["coresight"]
    assert blocks["tpiu"] == 0x4024A000 and blocks["tsgen"] == 0x4024C000
    assert k358["trace"]["max_width"] == 4
