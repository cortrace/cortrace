"""The architecture's arming sequence, against a register file in memory."""

import pytest

from cortrace import platdesc, trace_arm
from cortrace.osplugins import Watch
from cortrace.trace_arm import ArmError


class FakeProbe:
    """A register file that logs writes in order."""

    def __init__(self, initial=None):
        self.regs = dict(initial or {})
        self.writes = []
        self.events = []

    def halt(self):
        self.events.append("halt")

    def resume(self):
        self.events.append("resume")

    def read32(self, addr):
        return self.regs.get(addr, 0)

    def write32(self, addr, value):
        self.writes.append((addr, value))
        self.regs[addr] = value


@pytest.fixture(name="plat")
def plat_fixture():
    return platdesc.load("stm32h743")


def order(probe, addr):
    """Values written to `addr`, in order."""
    return [v for a, v in probe.writes if a == addr]


def test_halts_first_resumes_last_and_reports_the_readback(plat):
    probe = FakeProbe()
    state = trace_arm.arm(probe, plat, [Watch("g_x", 0x24000600)], log=lambda _m: None)
    assert probe.events == ["halt", "resume"]
    assert state == {"dwt_comp0": 0x24000600, "dwt_func0": 0xD, "itm_tcr": 0x1000F}


def test_chip_steps_run_before_the_generic_blocks(plat):
    probe = FakeProbe({0x58021000: 0xFFFFFFFF, 0x58000420: 0x100})
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    addrs = [a for a, _ in probe.writes]
    assert addrs.index(0x58021000) < addrs.index(0xE000EDFC) < addrs.index(0x5C015004)
    # GPIOE_MODER: clear 0x3FF0, set 0x2AA0 -- the other bits survive
    assert probe.regs[0x58021000] == (0xFFFFFFFF & ~0x3FF0) | 0x2AA0
    assert probe.regs[0x5C001004] == 0x00700000  # DBGMCU_CR


@pytest.mark.parametrize("width,size", [(4, 0x8), (2, 0x2), (1, 0x1)])
def test_tpiu_port_size_follows_the_width(plat, width, size):
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], width=width, log=lambda _m: None)
    assert order(probe, 0x5C015004) == [size]
    assert order(probe, 0x5C0150F0) == [0]  # parallel
    assert order(probe, 0x5C015304) == [0x102]


def test_blocks_are_unlocked_before_they_are_programmed(plat):
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    for base in (0x5C015000, 0x5C013000, 0xE0041000, 0xE0000000):
        in_block = [w for w in probe.writes if base <= w[0] < base + 0x1000]
        assert in_block[0] == (base + 0xFB0, trace_arm.CS_UNLOCK_KEY)
    # the ETF is switched off first, then unlocked, then programmed
    etf = [w for w in probe.writes if 0x5C014000 <= w[0] < 0x5C015000]
    assert etf[0] == (0x5C014020, 0)
    assert etf[1] == (0x5C014000 + 0xFB0, trace_arm.CS_UNLOCK_KEY)


def test_etf_is_enabled_last_among_its_registers(plat):
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    assert order(probe, 0x5C014020) == [0, 1]  # off to program, on at the end
    assert order(probe, 0x5C014028) == [2]


def test_etm_is_programmed_and_enabled_with_the_platform_id(plat):
    probe = FakeProbe({0xE004100C: 1})
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    assert order(probe, 0xE0041004) == [0, 1]  # off, program, on
    assert order(probe, 0xE0041040) == [2]  # TRCTRACEIDR
    assert order(probe, 0xE0041010) == [0x810]


def test_coverage_profile_drops_cycle_counts_and_timestamps(plat):
    probe = FakeProbe({0xE004100C: 1})
    trace_arm.arm(probe, plat, [], profile="coverage", log=lambda _m: None)
    assert order(probe, 0xE0041010) == [0x1]
    probe = FakeProbe()
    with pytest.raises(ArmError, match="unknown ETM profile"):
        trace_arm.arm(probe, plat, [], profile="fast", log=lambda _m: None)
    assert not probe.writes and not probe.events  # refused before the halt
    with pytest.raises(ArmError, match="unknown ETM profile"):
        trace_arm.arm_etm(trace_arm.Regs(probe), 0xE0041000, 2, "fast")


def test_comparators_that_are_not_used_are_switched_off(plat):
    """A comparator armed by an earlier session must not keep emitting."""
    probe = FakeProbe({0xE0001038: 0xD, 0xE0001048: 0xD})
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    assert probe.regs[0xE0001028] == 0xD  # COMP0 is the watched variable
    assert probe.regs[0xE0001038] == 0
    assert probe.regs[0xE0001048] == 0
    assert probe.regs[0xE0001058] == 0


def test_no_etm_holds_it_disabled(plat):
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], etm=False, log=lambda _m: None)
    assert order(probe, 0xE0041004) == [0]
    assert order(probe, 0xE0041010) == []


def test_itm_gets_the_platform_trace_id(plat):
    plat["trace"]["itm_trace_id"] = 5
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    assert order(probe, 0xE0000E80) == [(5 << 16) | 0xF]
    assert order(probe, 0xE0000E00) == [0xFFFFFFFF]


def test_one_dwt_comparator_per_watched_variable(plat):
    probe = FakeProbe()
    watches = [Watch("a", 0x20000010), Watch("b", 0x20000014)]
    trace_arm.arm(probe, plat, watches, log=lambda _m: None)
    assert order(probe, 0xE0001020) == [0x20000010]
    assert order(probe, 0xE0001030) == [0x20000014]
    assert order(probe, 0xE0001028) == [0, 0xD]  # off while programming, then on
    assert order(probe, 0xE0001038) == [0, 0xD]


def test_too_many_variables_for_the_comparators(plat):
    watches = [Watch(str(i), 4 * i) for i in range(5)]
    with pytest.raises(ArmError, match="5 variables.*4 comparators"):
        trace_arm.arm(FakeProbe(), plat, watches, log=lambda _m: None)


@pytest.mark.parametrize("width", [0, 5])
def test_width_outside_the_platform_range(plat, width):
    with pytest.raises(ArmError, match="width"):
        trace_arm.arm(FakeProbe(), plat, [Watch("g", 4)], width=width)


def steps(*items):
    return {
        "trace": {"max_width": 4, "etm_trace_id": 2, "itm_trace_id": 1},
        "coresight": {k: 0x1000 * i for i, k in enumerate(platdesc.BLOCKS, start=1)},
        "on_trace_start": list(items),
    }


def test_write32_and_poll32_steps():
    probe = FakeProbe({0x500: 0x4})
    done = steps(
        {"op": "write32", "addr": 0x100, "value": 7},
        {"op": "poll32", "addr": 0x500, "mask": 0x4, "comment": "ready"},
    )
    messages = []
    trace_arm.run_chip_steps(
        trace_arm.Regs(probe), done["on_trace_start"], messages.append
    )
    assert probe.regs[0x100] == 7 and messages == ["  write32", "  ready"]


def test_a_poll_that_never_succeeds_goes_on_unless_required():
    regs = trace_arm.Regs(FakeProbe())
    messages = []
    soft = {"op": "poll32", "addr": 0x500, "mask": 1, "tries": 3, "comment": "ready"}
    trace_arm.run_chip_steps(regs, [soft], messages.append)
    assert "not set after 3 reads, going on" in messages[0]
    with pytest.raises(ArmError, match="timed out: ready"):
        trace_arm.run_chip_steps(regs, [dict(soft, required=True)], messages.append)
    with pytest.raises(ArmError, match="timed out: 0x00000500"):
        hard = {"op": "poll32", "addr": 0x500, "mask": 1, "tries": 1, "required": True}
        trace_arm.run_chip_steps(regs, [hard], messages.append)


def test_poll_with_a_value_waits_for_the_masked_bits_to_equal_it():
    regs = trace_arm.Regs(FakeProbe({0x500: 0x20000}))
    assert not regs.poll(0x500, 0x20000, 3, value=0)  # still set
    regs = trace_arm.Regs(FakeProbe({0x500: 0x1}))
    assert regs.poll(0x500, 0x20000, 3, value=0)  # the bit is clear
    assert not regs.poll(0x500, 0x20000, 3)  # without value: any bit set


def test_a_poll_step_with_a_value_times_out_when_it_never_matches():
    regs = trace_arm.Regs(FakeProbe({0x500: 0x20000}))
    step = {"op": "poll32", "addr": 0x500, "mask": 0x20000, "value": 0, "tries": 2}
    messages = []
    trace_arm.run_chip_steps(regs, [step], messages.append)
    assert "not set after 2 reads, going on" in messages[0]
    with pytest.raises(ArmError, match="timed out"):
        trace_arm.run_chip_steps(regs, [dict(step, required=True)], messages.append)


def test_the_default_path_programs_the_funnel_before_the_etf(plat):
    probe = FakeProbe()
    trace_arm.arm(probe, plat, [Watch("g_x", 4)], log=lambda _m: None)
    assert order(probe, 0x5C013000) == [0x303]  # ports 0 and 1, hold time 3
    first = {
        base: next(
            i for i, w in enumerate(probe.writes) if base <= w[0] < base + 0x1000
        )
        for base in (0x5C013000, 0x5C014000)
    }
    assert first[0x5C013000] < first[0x5C014000]
