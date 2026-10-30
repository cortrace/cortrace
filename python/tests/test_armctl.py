"""`cortrace arm`."""

import pytest
from elffixture import FREERTOS_C, NUTTX_C, build
from test_trace_arm import FakeProbe

from cortrace import armctl, osplugins, platdesc


@pytest.fixture(autouse=True)
def isolate(monkeypatch, tmp_path):
    monkeypatch.setattr(osplugins, "USER_DIR", str(tmp_path / "no-plugins"))
    monkeypatch.setattr(platdesc, "USER_DIR", str(tmp_path / "no-platforms"))
    monkeypatch.delenv("CORTRACE_PLUGIN_PATH", raising=False)
    monkeypatch.delenv(platdesc.SEARCH_ENV, raising=False)


@pytest.fixture(name="probe")
def probe_fixture(monkeypatch):
    probe = FakeProbe()
    probe.closed = False
    probe.close = lambda: setattr(probe, "closed", True)
    monkeypatch.setattr(armctl, "OpenOcdTelnet", lambda hostport: probe)
    return probe


def base_args(*extra):
    return ["--platform", "stm32h743", "--probe-telnet", "127.0.0.1:4444", *extra]


def test_list_shows_the_platforms(capsys):
    assert armctl.main(["--list"]) == 0
    names = [line.split("\t")[0] for line in capsys.readouterr().out.splitlines()]
    assert names == ["s32k344", "stm32h743"]


def test_arms_the_variable_the_os_plugin_names(tmp_path, probe, capsys):
    elf = build(tmp_path, "rtos", FREERTOS_C, "-g")
    assert armctl.main(base_args("--elf", elf)) == 0
    err = capsys.readouterr().err
    assert "OS: freertos" in err and "watching pxCurrentTCB" in err
    assert "armed: DWT COMP0=" in err
    assert probe.regs[0xE0001020] != 0 and probe.closed
    assert probe.events == ["halt", "resume"]


def test_explicit_addresses_need_no_elf(probe):
    assert armctl.main(base_args("--watch-addr", "0x24000600", "--width", "2")) == 0
    assert probe.regs[0xE0001020] == 0x24000600
    assert probe.regs[0x5C015004] == 0x2


def test_os_none_arms_without_watching_a_variable(probe, capsys):
    """Bare metal: no ELF needed, no comparator armed, the ETM still on."""
    assert armctl.main(base_args("--os", "none")) == 0
    assert "OS: none, no variable watched" in capsys.readouterr().err
    for comp in range(4):
        assert probe.regs[0xE0001028 + 0x10 * comp] == 0
    assert (0xE0001028, 0xD) not in probe.writes
    assert (0xE0041004, 1) in probe.writes  # ETM on
    assert probe.events == ["halt", "resume"]


def test_os_none_clears_a_comparator_left_by_an_earlier_session(probe):
    probe.regs[0xE0001028] = 0xD  # COMP0 FUNCTION armed before
    assert armctl.main(base_args("--os", "none")) == 0
    assert probe.regs[0xE0001028] == 0


def test_a_watched_address_wins_over_os_none(probe):
    assert armctl.main(base_args("--os", "none", "--watch-addr", "0x24000600")) == 0
    assert probe.regs[0xE0001020] == 0x24000600


def test_no_etm_flag_keeps_the_etm_off(probe):
    assert armctl.main(base_args("--watch-addr", "4", "--no-etm")) == 0
    assert (0xE0041010, 0x810) not in probe.writes


def test_coverage_profile_flag(probe):
    assert armctl.main(base_args("--os", "none", "--profile", "coverage")) == 0
    assert (0xE0041010, 0x1) in probe.writes
    assert (0xE0041010, 0x810) not in probe.writes


def test_dwt_events_flag(probe):
    assert armctl.main(base_args("--os", "none", "--dwt-events", "lsu, sleep")) == 0
    assert (0xE0001000, 0x40000001 | 1 << 20 | 1 << 19 | 1 << 10) in probe.writes


def test_dwt_events_all_and_unknown(probe, capsys):
    assert armctl.main(base_args("--os", "none", "--dwt-events", "all")) == 0
    assert (0xE0001000, 0x40000001 | 0x3E0000 | 1 << 10) in probe.writes
    assert armctl.main(base_args("--os", "none", "--dwt-events", "cache")) == 1
    assert "unknown DWT event" in capsys.readouterr().err


def test_platform_and_probe_are_required():
    with pytest.raises(SystemExit) as e:
        armctl.main(["--platform", "stm32h743"])
    assert e.value.code == 2


@pytest.mark.parametrize(
    "extra,message",
    [
        ([], "pass --elf or --watch-addr"),
        (["--platform", "nochip", "--watch-addr", "4"], "unknown platform"),
        (["--watch-addr", "4", "--width", "9"], "width 9"),
    ],
)
def test_errors_are_reported_not_raised(probe, capsys, extra, message):
    args = base_args(*extra)
    if "--platform" in extra:  # the later --platform wins
        args = ["--probe-telnet", "x:1", *extra]
    assert armctl.main(args) == 1
    assert message in capsys.readouterr().err
    assert not probe.writes


@pytest.mark.usefixtures("probe")
def test_an_elf_without_a_matching_os_is_an_error(tmp_path, capsys):
    bare = build(tmp_path, "bare", "void main(void) {}", "-g")
    assert armctl.main(base_args("--elf", bare)) == 1
    assert "no OS plugin matches" in capsys.readouterr().err
    nuttx = build(tmp_path, "nuttx", NUTTX_C, "-g")
    assert armctl.main(base_args("--elf", nuttx, "--os", "freertos")) == 1
    assert "neither pxCurrentTCB nor pxCurrentTCBs" in capsys.readouterr().err
