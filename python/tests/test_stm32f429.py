"""The STM32F429 platform file: a Cortex-M4 with an ETMv3.5 and no funnel, ETF or
timestamp generator, read off a STM32F429I-DISC1."""

import pytest
from test_trace_arm import FakeProbe, order
from cortrace import platdesc, trace_arm
from cortrace.osplugins import Watch
from cortrace.trace_arm import ArmError

ETM = 0xE0041000
TPIU = 0xE0040000
PROGRAMMING = 1 << 10
PORT_SELECT = 1 << 11


@pytest.fixture(name="f429")
def f429_fixture():
    return platdesc.load("stm32f429")


def armed(f429, **kwargs):
    # ETMSR programming bit and SYSCFG_CMPCR.READY, which the arming waits for
    probe = FakeProbe({ETM + 0x010: 0x2, 0x40013820: 0x100})
    trace_arm.arm(
        probe, f429, [Watch("g_x", 0x20000600)], log=lambda _m: None, **kwargs
    )
    return probe


def test_the_blocks_are_the_ones_in_the_rom_table(f429):
    blocks = f429["coresight"]
    assert (blocks["etm"], blocks["tpiu"]) == (ETM, TPIU)
    assert blocks["path"] == [] and "tsgen" not in blocks
    assert f429["trace"]["etm_version"] == 3
    assert (f429["trace"]["default_width"], f429["trace"]["max_width"]) == (2, 4)


def test_the_etmv3_is_programmed_then_started(f429):
    probe = armed(f429, profile="coverage")
    # programming bit set twice (the TRM allows it), then cleared to start
    assert order(probe, ETM) == [
        PROGRAMMING,
        PROGRAMMING,
        PORT_SELECT | PROGRAMMING,
        PORT_SELECT,
    ]
    assert order(probe, ETM + 0x200) == [2]  # TRACEIDR
    assert order(probe, ETM + 0x020) == [0x6F]  # TraceEnable: always
    assert order(probe, ETM + 0x024) == [0]
    assert order(probe, ETM + 0x028) == [8]
    assert order(probe, ETM + 0x008) == [0x406F]  # TRIGGER: never


def test_the_timing_profile_asks_for_timestamps(f429):
    probe = armed(f429, profile="timing")
    assert order(probe, ETM)[-1] == PORT_SELECT | (1 << 28)


def test_no_funnel_buffer_or_timestamp_generator_is_touched(f429):
    probe = armed(f429)
    for addr, _ in probe.writes:
        assert (
            not 0xE0043000 <= addr < 0xE0044000
        )  # where those blocks sit on other M4s
        assert (
            not 0xE0042000 <= addr < 0xE0043000 or addr == 0xE0042004
        )  # DBGMCU_CR only


@pytest.mark.parametrize("width,size", [(2, 0x2), (4, 0x8), (1, 0x1)])
def test_the_tpiu_port_size_follows_the_width(f429, width, size):
    probe = armed(f429, width=width)
    assert order(probe, TPIU + 0x004) == [size]


def test_the_trace_pins_are_alternate_function_0_on_pe2_to_pe6(f429):
    probe = armed(f429)
    gpioe = 0x40021000
    assert probe.regs[gpioe + 0x00] & 0x3FF0 == 0x2AA0  # MODER: alternate function
    assert probe.regs[gpioe + 0x08] & 0x3FF0 == 0x3FF0  # OSPEEDR: very high speed
    assert probe.regs[gpioe + 0x20] & 0x0FFFFF00 == 0  # AFRL: AF0
    assert probe.regs[0xE0042004] & 0xE7 == 0xE7  # DBGMCU_CR: trace on, 4-bit mode


def test_an_unknown_profile_is_refused_before_the_core_is_halted(f429):
    probe = FakeProbe()
    with pytest.raises(ArmError, match="unknown ETM profile"):
        trace_arm.arm(probe, f429, [Watch("g_x", 4)], profile="nonsense", log=print)
    assert not probe.events
