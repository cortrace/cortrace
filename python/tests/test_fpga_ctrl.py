import pytest
from fakefpga import FakeFpga

from cortrace.fpga import ctrl, health


@pytest.fixture
def fpga(monkeypatch):
    fake = FakeFpga()
    monkeypatch.setattr(ctrl, "CTRL_PORT", fake.ctrl_port)
    yield fake
    fake.close()


def run(args):
    return ctrl.main(["--ip", "127.0.0.1", "--no-discover"] + args)


def test_write_csr_sends_addr_value_and_padding(fpga):
    ctrl.write_csr("127.0.0.1", 0x08, 4)
    assert fpga.csr_writes == [(0x08, 4)]


def test_write_csr_tolerates_a_silent_fpga(monkeypatch):
    monkeypatch.setattr(ctrl, "CTRL_PORT", 9)  # nobody answers (discard port)
    ctrl.write_csr("127.0.0.1", 0x02, 1, timeout=0.05)


def test_set_width_writes_width_then_rearms(fpga, capsys):
    assert run(["set-width", "2"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_WIDTH, 2), (ctrl.REG_REARM, 1)]
    assert "width = 2 bit" in capsys.readouterr().out


def test_set_tap_all_lanes_writes_tap_then_rearms(fpga, capsys):
    assert run(["set-tap", "16"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_IDELAY_ALL, 16), (ctrl.REG_REARM, 1)]
    out = capsys.readouterr().out
    assert "tap = 16" in out and "all data lanes" in out


def test_set_tap_one_lane_packs_the_lane_above_the_tap(fpga, capsys):
    assert run(["set-tap", "12", "--lane", "2"]) == 0
    assert fpga.csr_writes == [
        (ctrl.REG_IDELAY_LANE, (2 << 5) | 12),
        (ctrl.REG_REARM, 1),
    ]
    assert "data lane 2" in capsys.readouterr().out


@pytest.mark.parametrize(
    "args", [["set-tap", "32"], ["set-tap", "-1"], ["set-tap", "5", "--lane", "4"]]
)
def test_set_tap_rejects_out_of_range(fpga, args):
    with pytest.raises(SystemExit):
        run(args)
    assert fpga.csr_writes == []


def _mmcm_at(fpga, phase, flags=0b001):
    fpga.mem[health.A_MM_STATUS] = flags
    raw = phase & 0x3FF
    fpga.mem[health.A_MM_PHASE] = raw & 0xFF
    fpga.mem[health.A_MM_PHASE + 1] = raw >> 8


def test_set_phase_writes_low_then_high_then_rearms(fpga, monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    _mmcm_at(fpga, -120)
    assert run(["set-phase", "-120"]) == 0
    assert fpga.csr_writes == [
        (ctrl.REG_MM_PHASE_LO, 0x88),
        (ctrl.REG_MM_PHASE_HI, 0x03),
        (ctrl.REG_REARM, 1),
    ]
    assert "fine phase = -120 steps" in capsys.readouterr().out


def test_set_phase_prints_the_step_size_when_the_clock_is_given(
    fpga, monkeypatch, capsys
):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    _mmcm_at(fpga, 100)
    assert run(["set-phase", "100", "--pin-mhz", "44"]) == 0
    # 56 steps per VCO period, VCO = 14 x 44 MHz
    assert "~+2899 ps at 44 MHz, 29.0 ps/step" in capsys.readouterr().out


def test_set_phase_fails_when_the_mmcm_never_gets_there(fpga, monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    monkeypatch.setattr(ctrl.time, "sleep", lambda _s: None)
    ticks = iter(range(0, 1000, 1))
    monkeypatch.setattr(ctrl.time, "monotonic", lambda: next(ticks) * 0.5)
    _mmcm_at(fpga, 3)
    assert run(["set-phase", "10"]) == 1
    assert "is +3, wanted +10" in capsys.readouterr().out


def test_set_phase_reports_an_unlocked_mmcm(fpga, monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    monkeypatch.setattr(ctrl.time, "sleep", lambda _s: None)
    ticks = iter(range(0, 1000, 1))
    monkeypatch.setattr(ctrl.time, "monotonic", lambda: next(ticks) * 0.5)
    _mmcm_at(fpga, 0, flags=0)
    assert run(["set-phase", "5"]) == 1
    assert "not locked" in capsys.readouterr().out


def test_set_phase_no_wait_only_writes(fpga, capsys):
    assert run(["set-phase", "7", "--no-wait"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_MM_PHASE_LO, 7), (ctrl.REG_MM_PHASE_HI, 0)]
    assert "target = +7 steps" in capsys.readouterr().out


@pytest.mark.parametrize("value", ["256", "-256"])
def test_set_phase_rejects_out_of_range(fpga, value):
    with pytest.raises(SystemExit):
        run(["set-phase", value])
    assert fpga.csr_writes == []


@pytest.mark.parametrize("mode,value", [("direct", 0), ("mmcm", 1)])
def test_set_mode_writes_the_source_then_rearms(fpga, mode, value, capsys):
    assert run(["set-mode", mode]) == 0
    assert fpga.csr_writes == [(ctrl.REG_CLK_SEL, value), (ctrl.REG_REARM, 1)]
    assert f"sampling clock = {mode}" in capsys.readouterr().out


def test_relock_writes_the_relock_register(fpga):
    assert run(["relock"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_MM_RELOCK, 1)]


def test_set_bitlen_splits_into_lo_and_hi(fpga):
    assert run(["set-bitlen", "4660"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_BITLEN_LO, 0x34), (ctrl.REG_BITLEN_HI, 0x12)]


@pytest.mark.parametrize(
    "cmd,reg",
    [("stream-selftest", ctrl.REG_STREAM_SELFTEST), ("iddr-prbs", ctrl.REG_IDDR_PRBS)],
)
def test_switch_commands(fpga, cmd, reg):
    assert run([cmd, "1"]) == 0
    assert fpga.csr_writes == [(reg, 1)]


def test_rearm(fpga):
    assert run(["rearm"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_REARM, 1)]


def test_discovery_fills_in_ip_and_iface(monkeypatch):
    seen = {}
    monkeypatch.setattr(
        ctrl.fpga_net,
        "discover_fpga",
        lambda ip: {"ip": "127.0.0.1", "iface": "eth9", "mac": b""},
    )
    monkeypatch.setattr(
        ctrl, "write_csr", lambda ip, a, v, **_k: seen.update(ip=ip, iface=ctrl._IFACE)
    )
    assert ctrl.main(["rearm"]) == 0
    assert seen == {"ip": "127.0.0.1", "iface": "eth9"}


def test_discovery_permission_error_falls_back_to_default_ip(monkeypatch):
    def deny(ip):
        raise PermissionError

    monkeypatch.setattr(ctrl.fpga_net, "discover_fpga", deny)
    seen = {}
    monkeypatch.setattr(ctrl, "write_csr", lambda ip, a, v, **_k: seen.update(ip=ip))
    ctrl.main(["rearm"])
    assert seen["ip"] == ctrl.fpga_net.DEFAULT_FPGA_IP


# ---- oversampling capture ----------------------------------------------------
def test_oversample_on_writes_blank_dly_clear_enable_then_rearms(fpga, capsys):
    assert run(["oversample", "on", "--dly", "6", "--blank", "2"]) == 0
    assert fpga.csr_writes == [
        (ctrl.REG_OS_BLANK, 2),
        (ctrl.REG_OS_DLY, 6),
        (ctrl.REG_OS_CLR, 1),
        (ctrl.REG_OS_CTRL, 1),
        (ctrl.REG_REARM, 1),
    ]
    assert "oversampling on" in capsys.readouterr().out


def test_oversample_on_keeps_the_settings_that_are_not_given(fpga):
    assert run(["oversample", "on"]) == 0
    addrs = [a for a, _ in fpga.csr_writes]
    assert ctrl.REG_OS_DLY not in addrs and ctrl.REG_OS_BLANK not in addrs


def test_oversample_vote_sets_bit_one(fpga, capsys):
    assert run(["oversample", "on", "--vote"]) == 0
    assert (ctrl.REG_OS_CTRL, 3) in fpga.csr_writes
    assert "vote" in capsys.readouterr().out


def test_oversample_off_clears_the_enable_and_rearms(fpga, capsys):
    assert run(["oversample", "off"]) == 0
    assert fpga.csr_writes == [(ctrl.REG_OS_CTRL, 0), (ctrl.REG_REARM, 1)]
    assert "IDDR" in capsys.readouterr().out


@pytest.mark.parametrize("args", [["--dly", "32"], ["--blank", "-1"], ["--dly", "x"]])
def test_oversample_rejects_out_of_range_settings(fpga, args):
    with pytest.raises(SystemExit):
        run(["oversample", "on"] + args)
    assert fpga.csr_writes == []


def test_oversample_status_prints_the_pin_clock(fpga, monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    for addr, value in (
        (health.A_OS_RF, 8),
        (health.A_OS_FR, 8),
        (health.A_OS_CFG, 1),
        (health.A_OS_DLY, 5),
        (health.A_OS_BLANK, 3),
    ):
        fpga.mem[addr] = value
    assert run(["oversample", "status"]) == 0
    out = capsys.readouterr().out
    assert "oversampling on" in out and "12.50 MHz" in out and "dly 5" in out
    assert fpga.csr_writes == []


def test_oversample_status_without_a_clock_says_so(fpga, monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", fpga.readout_port)
    assert run(["oversample", "status"]) == 0
    assert "not measurable" in capsys.readouterr().out
