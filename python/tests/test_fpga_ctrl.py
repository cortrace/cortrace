import pytest
from fakefpga import FakeFpga

from cortrace.fpga import ctrl


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
