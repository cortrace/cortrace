import pytest
from fakefpga import FakeFpga

from cortrace.fpga import health


def put(mem, addr, value, size=1):
    for i, byte in enumerate(value.to_bytes(size, "little")):
        mem[addr + i] = byte


@pytest.fixture
def board(monkeypatch):
    mem = {}
    fake = FakeFpga(mem)
    monkeypatch.setattr(health, "PORT", fake.readout_port)
    monkeypatch.setattr(health, "CTRL_PORT", fake.ctrl_port)
    monkeypatch.setattr(health.time, "sleep", lambda _s: None)
    yield mem, fake
    fake.close()


def run(*args):
    return health.main(["127.0.0.1"] + list(args))


def healthy(mem):
    put(mem, health.A_MAGIC, 0xDB)
    # have_first=0 pkt_active=1 traceclk=1 trace_lock=1 sys_lock=1 fsm=2
    put(mem, health.A_LIVE, 0b0111_1010)


def test_healthy_link(board, capsys):
    mem, _ = board
    healthy(mem)
    assert run() == 0
    out = capsys.readouterr().out
    assert (
        "no sticky errors latched" in out
        and "TRACECLK active + trace MMCM locked" in out
    )


def test_sticky_error_is_only_a_warning_without_reset(board, capsys):
    mem, _ = board
    healthy(mem)
    put(mem, health.A_LIVE, 0b1111_1010)  # have_first
    put(mem, health.A_FIRST_CODE, 0x0401, 2)
    put(mem, health.A_FIRST_TIME, 125_000, 4)
    assert run() == 0
    out = capsys.readouterr().out
    assert "[WARN] FIRST_ERR = 0x0401" in out and "ARP" in out
    assert "may predate the current state" in out and "[FAIL]" not in out


def test_sticky_error_after_reset_is_a_failure(board, capsys):
    mem, _ = board
    healthy(mem)
    stamp(mem, features=0b1_1110)  # bit4: the bitstream can clear the error
    put(mem, health.A_LIVE, 0b1111_1010)
    put(mem, health.A_FIRST_CODE, 0x0401, 2)
    assert run("--reset") == 2
    out = capsys.readouterr().out
    assert "[FAIL] FIRST_ERR = 0x0401" in out and "may predate" not in out


def test_reset_on_a_bitstream_that_cannot_clear_stays_a_warning(board, capsys):
    mem, _ = board
    healthy(mem)
    stamp(mem, features=0b0_1110)  # no clear register
    put(mem, health.A_LIVE, 0b1111_1010)
    put(mem, health.A_FIRST_CODE, 0x0301, 2)
    assert run("--reset") == 0
    out = capsys.readouterr().out
    assert "cannot clear its sticky error" in out and "[WARN] FIRST_ERR" in out


@pytest.mark.parametrize(
    "code,text", [(0x0301, "drain"), (0x0101, "trace configuration")]
)
def test_other_error_codes_get_their_advice(board, capsys, code, text):
    mem, _ = board
    healthy(mem)
    stamp(mem, features=0b1_1110)
    put(mem, health.A_LIVE, 0b1111_1010)
    put(mem, health.A_FIRST_CODE, code, 2)
    assert run("--reset") == 2
    assert text in capsys.readouterr().out


def test_clock_trouble_is_called_out(board, capsys):
    mem, _ = board
    put(mem, health.A_MAGIC, 0xDB)
    put(mem, health.A_LIVE, 0b0000_0010)  # no locks, no TRACECLK
    assert run() == 0
    out = capsys.readouterr().out
    assert "system MMCM not locked" in out and "no TRACECLK activity" in out
    put(mem, health.A_LIVE, 0b0010_1010)  # TRACECLK but trace MMCM unlocked
    assert run() == 0
    assert "trace MMCM not locked" in capsys.readouterr().out


def test_idle_target_is_not_reported_as_a_fault(board, capsys):
    """Fresh bitstream, target not tracing: no clock, saturated gap counter."""
    mem, _ = board
    put(mem, health.A_MAGIC, 0xDB)
    put(mem, health.A_LIVE, 0b0000_1010)  # sys lock, no TRACECLK
    put(mem, health.A_GAP, 1, 2)
    put(mem, health.A_GAP + 2, 0xFFFF, 2)
    assert run() == 0
    out = capsys.readouterr().out
    assert "[FAIL]" not in out and "DISCONTINUOUS" not in out
    assert "pins are quiet" in out and "STM32" not in out and "H7" not in out


def stamp(mem, features=0b1110, flags=0b101, build_id=1_791_000_000):
    put(mem, health.A_ID_MARK, health.ID_MARK)
    for i, byte in enumerate((3, 2, 1, flags)):  # patch, minor, major, flags
        mem[health.A_VERSION + i] = byte
    put(mem, health.A_GIT, 0x1A2B3C4D, 4)
    put(mem, health.A_FEATURES, features)
    put(mem, health.A_BUILD_ID, build_id, 4)


def test_fpga_version_and_build_time_are_printed(board, capsys):
    mem, _ = board
    healthy(mem)
    stamp(mem)
    assert run() == 0
    out = capsys.readouterr().out
    assert "FPGA design : v1.2.3 (git 1a2b3c4d, built from a modified tree" in out
    assert "not a release tag" in out and "pre-release" not in out
    assert "built 2026-" in out and "BUILD_ID=1791000000" in out
    assert "features: DDR3 ring, stream self-test, run-time port width" in out
    assert "not JTAG" in out


def test_idelay_feature_bit_is_named(board, capsys):
    mem, _ = board
    healthy(mem)
    stamp(mem, features=0b11_1110)  # bit5: run-time IDELAY tap (cortrace-fpga 1.1.0)
    assert run() == 0
    assert "run-time IDELAY tap" in capsys.readouterr().out


def test_bitstream_without_pin_monitors_is_not_judged_on_them(board, capsys):
    """The DDR-ring bitstream ties TRACECLK/GPIO/gap monitors to constants."""
    mem, _ = board
    put(mem, health.A_MAGIC, 0xDB)
    put(mem, health.A_LIVE, 0b0001_1010)  # sys lock, DDR3 calibrated, no TRACECLK
    put(mem, health.A_GAP, 1, 2)
    put(mem, health.A_GAP + 2, 0xFFFF, 2)
    stamp(mem)
    assert run() == 0
    out = capsys.readouterr().out
    assert "does not expose TRACECLK / pin monitors" in out
    assert "DDR3 calibrated=1" in out
    for word in ("[FAIL]", "gaps", "STATIC", "raw GPIO", "no TRACECLK"):
        assert word not in out


def test_legacy_ring_bitstream_is_recognised_without_a_version_block(board, capsys):
    mem, _ = board
    put(mem, health.A_MAGIC, 0xDB)
    put(mem, health.A_LIVE, 0b0000_1010)  # DDR3 not calibrated
    put(mem, health.A_RING_MAGIC, 0xD1)
    put(mem, health.A_BUILD_ID, 1_791_000_000, 4)
    assert run() == 0
    out = capsys.readouterr().out
    assert "no version register" in out and "built 2026-" in out
    assert "does not expose TRACECLK / pin monitors" in out
    assert "DDR3 not calibrated" in out


def test_unstamped_build_id_is_reported_as_unknown(board, capsys):
    mem, _ = board
    healthy(mem)
    put(mem, health.A_BUILD_ID, 0xDEADBEEF, 4)  # the HDL default
    assert run() == 0
    assert "built unknown" in capsys.readouterr().out


def test_gaps_while_the_clock_runs_are_a_warning(board, capsys):
    mem, _ = board
    healthy(mem)
    put(mem, health.A_FREQ, 2_000_000, 3)
    put(mem, health.A_GPIO_EDGES, 500, 2)  # TRACECLK edges seen at the pin
    put(mem, health.A_GAP, 3, 2)
    put(mem, health.A_GAP + 2, 100, 2)
    assert run() == 0
    out = capsys.readouterr().out
    assert "[WARN] TRACECLK has gaps (3 gaps" in out and "[FAIL]" not in out


def test_wrong_magic_warns(board, capsys):
    mem, _ = board
    put(mem, health.A_MAGIC, 0x00)
    assert run() == 0
    assert "magic = 0x00" in capsys.readouterr().out


def test_reset_flag_pulses_soft_reset_first(board, capsys):
    mem, fake = board
    healthy(mem)
    assert run("--reset") == 0
    assert (health.REG_SOFTRST, 1) in fake.csr_writes
    assert "soft reset pulsed" in capsys.readouterr().out


def test_ddr3_page(board, capsys):
    mem, _ = board
    put(mem, health.A_DDR3_MAGIC, 0xD3)
    put(mem, health.A_DDR3_FLAGS, 0b101)  # calib + mig calib, no error
    put(mem, health.A_DDR3_PASSB, 7, 4)
    put(mem, health.A_BUILD_ID, 1_700_000_000, 4)
    assert run("--check", "ddr3") == 0
    assert "DDR3 PASS" in capsys.readouterr().out
    put(mem, health.A_DDR3_PASSB, 0, 4)
    assert run("--check", "ddr3") == 0
    assert "no verified bursts" in capsys.readouterr().out
    put(mem, health.A_DDR3_FLAGS, 0)
    assert run("--check", "ddr3") == 2
    assert "NOT calibrated" in capsys.readouterr().out
    put(mem, health.A_DDR3_FLAGS, 0b111)  # calib + sticky error
    put(mem, health.A_DDR3_ERRC, 3, 4)
    assert run("--check", "ddr3") == 2
    out = capsys.readouterr().out
    assert "MISMATCH" in out and "all-zero" in out
    put(mem, health.A_DDR3_GOTLO, 5)
    assert run("--check", "ddr3") == 2
    assert "first word wrong" in capsys.readouterr().out
    put(mem, health.A_DDR3_MAGIC, 0)
    assert run("--check", "ddr3") == 1


def test_blackbox_page(board, capsys):
    mem, _ = board
    put(mem, 0xFF50, 0xB0)
    put(mem, 0xFF51, 0b101)  # calib + TRACECLK active
    put(mem, 0xFF52, 100, 4)  # words written
    put(mem, 0xFF56, 2, 4)  # lost
    assert run("--check", "blackbox") == 0
    out = capsys.readouterr().out
    assert "BLACK BOX RECORDING" in out and "capture-side bytes dropped" in out
    put(mem, 0xFF52, 0, 4)
    assert run("--check", "blackbox") == 0
    assert "0 words written" in capsys.readouterr().out
    put(mem, 0xFF51, 0b001)
    assert run("--check", "blackbox") == 0
    assert "no TRACECLK" in capsys.readouterr().out
    put(mem, 0xFF51, 0)
    assert run("--check", "blackbox") == 2
    put(mem, 0xFF50, 0)
    assert run("--check", "blackbox") == 1


def test_silent_fpga_is_a_failure(monkeypatch, capsys):
    monkeypatch.setattr(health, "PORT", 9)  # nobody listens
    real_socket = health.socket.socket

    class QuickSocket(real_socket):
        def settimeout(self, _value):
            super().settimeout(0.05)

    monkeypatch.setattr(health.socket, "socket", QuickSocket)
    assert health.main(["127.0.0.1"]) == 1
    assert health.main(["127.0.0.1", "--check", "ddr3"]) == 1
    assert health.main(["127.0.0.1", "--check", "blackbox"]) == 1
    assert "no reply on :5001" in capsys.readouterr().out


def test_arguments_are_parsed_by_argparse():
    a = health.parse_args([])
    assert (a.ip, a.check, a.reset) == (health.IP, "health", False)
    a = health.parse_args(["10.1.2.3", "--check", "ddr3", "--reset"])
    assert (a.ip, a.check, a.reset) == ("10.1.2.3", "ddr3", True)
    with pytest.raises(SystemExit) as e:
        health.parse_args(["--check", "bogus"])
    assert e.value.code == 2
