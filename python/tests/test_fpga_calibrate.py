import argparse
import json

import pytest

from cortrace.fpga import calibrate

SUMMARY = """
=== cortrace decode result ===
  slice events        : 9874
  stream health       : lost-sync=7 overflow=0 resync(TraceOn)=7 addr-nacc=0 startup-sync=1 benign=1 other=0  -> clean
"""


def summary(events=9000, lost=0, resync=0, nacc=0):
    return {
        "slice_events": events,
        "lost_sync": lost,
        "resync": resync,
        "addr_nacc": nacc,
    }


# ---- parsing and the pass criterion ------------------------------------------


def test_parse_summary_reads_the_four_numbers():
    s = calibrate.parse_summary(SUMMARY)
    assert s == {"slice_events": 9874, "lost_sync": 7, "resync": 7, "addr_nacc": 0}


@pytest.mark.parametrize(
    "text", ["", "stream health : lost-sync=7", "slice events : 5"]
)
def test_parse_summary_rejects_an_incomplete_summary(text):
    assert calibrate.parse_summary(text) is None


def test_overflow_is_not_a_sampling_error():
    assert calibrate.sampling_errors(summary(lost=2583, resync=2583)) == 0


def test_lost_sync_without_a_resync_and_addr_nacc_are_sampling_errors():
    assert calibrate.sampling_errors(summary(lost=192, resync=1, nacc=159)) == 350


def test_the_measured_cases_of_the_rt1176_44mhz_scan():
    # tap: (events, lost-sync, resync, addr-nacc), cachecliff, pad 0x2/0x3
    table = {
        0: (938, 131, 3, 49),
        6: (9048, 34, 14, 48),
        12: (9874, 7, 7, 0),
        18: (9730, 10, 10, 0),
        24: (8972, 29, 7, 44),
        31: (7542, 52, 8, 228),
    }
    passing = {
        tap
        for tap, (n, lost, rs, nacc) in table.items()
        if calibrate.sampling_errors(summary(n, lost, rs, nacc)) == 0
    }
    assert passing == {12, 18}


def test_a_decode_that_sees_only_sync_frames_is_caught_by_the_event_count():
    quiet = summary(events=0)
    assert calibrate.sampling_errors(quiet) == 0  # no errors ...
    assert quiet["slice_events"] < 0.5 * 9000  # ... but not a pass


def test_reference_events_is_a_high_percentile_not_the_median():
    # a window that takes 5 of 14 settings: the median is a failing setting
    events = [0, 0, 100, 0, 0, 0, 0, 48000, 48200, 47900, 48100, 48050, 0, 12]
    assert calibrate.reference_events(events) >= 47900
    assert calibrate.reference_events([7]) == 7


# ---- window search, linear -------------------------------------------------


def probe_in(passing):
    calls = []

    def probe(p):
        calls.append(p)
        return p in passing

    probe.calls = calls
    return probe


def test_a_single_window_is_found_to_one_setting():
    probe = probe_in(range(12, 19))
    window, seen = calibrate.find_window(probe, 0, 31, 4)
    assert (window.lo, window.hi, window.width, window.mid) == (12, 18, 7, 15)
    assert not window.edge and not window.full
    # the coarse pass plus a few bisection steps, not 32 probes
    assert len(seen) < 20


def test_a_window_cut_off_by_the_end_of_the_range_is_flagged():
    window, _ = calibrate.find_window(probe_in(range(20, 32)), 0, 31, 4)
    assert (window.lo, window.hi) == (20, 31)
    assert window.edge


def test_every_setting_passing_is_the_full_range():
    window, seen = calibrate.find_window(probe_in(range(32)), 0, 31, 4)
    assert window.full and window.width == 32 and window.mid == 15
    assert len(seen) == 9  # 0,4,...,28 and 31


def test_nothing_passing_gives_no_window():
    window, _ = calibrate.find_window(probe_in(()), 0, 31, 4)
    assert window is None


def test_the_longest_of_two_windows_is_used_and_the_count_is_reported():
    window, _ = calibrate.find_window(
        probe_in(set(range(4, 8)) | set(range(16, 28))), 0, 31, 2
    )
    assert (window.lo, window.hi) == (16, 27)
    assert window.runs == 2


def test_the_window_is_narrow_enough_to_hide_between_coarse_points_is_missed():
    # a known limit: a window narrower than the coarse step can fall between points
    window, _ = calibrate.find_window(probe_in({13, 14}), 0, 31, 4)
    assert window is None


def test_eye_text_marks_passed_failed_and_untested():
    seen = {0: False, 1: True, 3: True}
    assert calibrate.eye_text(seen, 0, 4) == "x#.#."


# ---- window search, circular ------------------------------------------------

PERIOD = 392
LO = -196
HI = LO + PERIOD - 1


def arc(start, end):
    """Settings of an arc (unwrapped start..end) as a set inside LO..HI."""
    return {LO + (p - LO) % PERIOD for p in range(start, end + 1)}


def test_a_circular_window_inside_the_range():
    window, _ = calibrate.find_window(
        probe_in(arc(-150, -30)), LO, HI, 28, circular=True
    )
    assert (window.lo, window.hi, window.width) == (-150, -30, 121)
    assert window.mid == -90


def test_a_circular_window_that_wraps_round_the_end_of_the_range():
    # clean from +150 across the wrap to -170 (= +222)
    window, _ = calibrate.find_window(
        probe_in(arc(150, 222)), LO, HI, 28, circular=True
    )
    assert (window.lo, window.hi) == (150, 222)
    assert window.mid == 186
    # the setting to write is the same angle inside the hardware's range
    knob = calibrate.PhaseKnob("x", None, None, 14)
    assert knob.hw_value(window.mid) == 186
    assert knob.hw_value(222) == 222 - PERIOD


def test_a_window_that_starts_before_the_first_coarse_point():
    window, _ = calibrate.find_window(
        probe_in(arc(-190, -100)), LO, HI, 28, circular=True
    )
    assert (window.lo, window.hi) == (-190, -100)


def test_a_circular_range_that_passes_everywhere():
    window, _ = calibrate.find_window(
        probe_in(set(range(LO, HI + 1))), LO, HI, 28, circular=True
    )
    assert window.full and window.width == PERIOD


def test_the_phase_knob_period_follows_the_multiplier():
    assert calibrate.PhaseKnob("x", None, None, 14).period == 392
    assert calibrate.PhaseKnob("x", None, None, 16).period == 448


# ---- oversampling capture ----------------------------------------------------


class FakeCtrl:
    """Records CSR writes and keeps the settings the fake target reacts to."""

    REG_REARM = 0x02
    REG_IDELAY_ALL = 0x05
    REG_OS_CTRL = 0x18
    REG_OS_DLY = 0x19
    REG_OS_BLANK = 0x1A
    REG_OS_CLR = 0x1B

    def __init__(self):
        self.writes = []
        self.state = {"tap": 16, "dly": 5, "os_on": 0, "blank": 3}

    def write_csr(self, _ip, addr, value):
        self.writes.append((addr, value))
        if addr == self.REG_IDELAY_ALL:
            self.state["tap"] = value
        elif addr == self.REG_OS_DLY:
            self.state["dly"] = value
        elif addr == self.REG_OS_BLANK:
            self.state["blank"] = value
        elif addr == self.REG_OS_CTRL:
            self.state["os_on"] = value & 1


class FakeHealth:
    def __init__(self, oversample=True, half=8.5):
        self.oversample, self.half = oversample, half

    def has_oversample(self, _ip):
        return self.oversample

    def read_os_status(self, _ip):
        if self.half is None:
            return {"half_cycles": None, "pin_mhz": None}
        return {"half_cycles": self.half, "pin_mhz": 200 / (2 * self.half)}


class FakeProber:
    """Decodes cleanly where `sim["good"](state)` says so."""

    sim = {"good": lambda _state: False}

    def __init__(self, a, ctrl, health, iface):
        del a, health, iface
        self.ctrl = ctrl
        self.events = {}
        self.rate_mhz = 12.0

    def measure(self, _secs):
        ok = FakeProber.sim["good"](self.ctrl.state)
        return summary(events=9000 if ok else 0)

    def close(self):
        pass


def cal_args(tmp_path, *extra):
    p = argparse.ArgumentParser()
    calibrate.add_arguments(p)
    return p.parse_args(
        ["--elf", "x.elf", "--save", str(tmp_path / "rec.json")] + list(extra)
    )


@pytest.fixture(name="rig")
def rig_fixture(monkeypatch, tmp_path):
    monkeypatch.setattr(calibrate, "Prober", FakeProber)
    monkeypatch.setattr(calibrate, "_clock_source", lambda _ip, _h: "direct")
    return FakeCtrl(), tmp_path


def test_the_oversample_range_ends_before_the_next_edge():
    # half a period of 8.5 cycles (11.8 MHz): sample points 0..6
    knob = calibrate.OversampleKnob("ip", FakeCtrl(), 8.5, False)
    assert (knob.lo, knob.hi, knob.blank) == (0, 6, 2)
    assert knob.unit_ps == 5000.0
    assert not knob.circular


def test_the_oversample_range_is_capped_by_the_hardware():
    knob = calibrate.OversampleKnob("ip", FakeCtrl(), 83.0, True)
    assert (knob.hi, knob.blank) == (31, 27)
    assert knob.vote


def test_the_oversample_blank_is_at_least_one_cycle():
    assert calibrate.OversampleKnob("ip", FakeCtrl(), 4.0, False).blank == 1


def test_the_oversample_knob_writes_prepare_apply_and_release():
    ctrl = FakeCtrl()
    knob = calibrate.OversampleKnob("ip", ctrl, 8.5, True)
    knob.prepare()
    knob.apply(3)
    knob.release()
    assert ctrl.writes == [
        (ctrl.REG_OS_BLANK, 2),
        (ctrl.REG_OS_CLR, 1),
        (ctrl.REG_OS_CTRL, 3),  # on + vote
        (ctrl.REG_OS_DLY, 3),
        (ctrl.REG_REARM, 1),
        (ctrl.REG_OS_CTRL, 0),
        (ctrl.REG_REARM, 1),
    ]


def test_auto_falls_back_to_oversampling_when_no_tap_decodes(rig, capsys):
    ctrl, tmp_path = rig
    # edge-aligned target: no tap works, sample points 2..4 do
    FakeProber.sim["good"] = lambda st: st["os_on"] and 2 <= st["dly"] <= 4
    a = cal_args(tmp_path)
    assert calibrate.run(a, "ip", "eth0", ctrl, FakeHealth()) == calibrate.EXIT_OK
    out = capsys.readouterr().out
    assert "trying oversample" in out and "dly" in out
    assert ctrl.state["os_on"] == 1 and ctrl.state["dly"] == 3
    rec = json.loads((tmp_path / "rec.json").read_text())
    assert rec["mode"] == "oversample" and rec["knob"] == "dly" and rec["value"] == 3
    assert rec["blank"] == 2 and rec["vote"] is False


def test_auto_keeps_the_idelay_window_when_there_is_one(rig):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: not st["os_on"] and 12 <= st["tap"] <= 20
    a = cal_args(tmp_path)
    assert calibrate.run(a, "ip", "eth0", ctrl, FakeHealth()) == calibrate.EXIT_OK
    assert ctrl.state["os_on"] == 0 and ctrl.state["tap"] == 16
    assert (ctrl.REG_OS_CTRL, 1) not in ctrl.writes
    rec = json.loads((tmp_path / "rec.json").read_text())
    assert rec["mode"] == "direct"


def test_auto_starts_on_the_iddr_path_even_if_oversampling_was_left_on(rig):
    ctrl, tmp_path = rig
    ctrl.state["os_on"] = 1
    FakeProber.sim["good"] = lambda st: not st["os_on"] and 12 <= st["tap"] <= 20
    assert calibrate.run(cal_args(tmp_path), "ip", "eth0", ctrl, FakeHealth()) == 0
    assert ctrl.writes[0] == (ctrl.REG_OS_CTRL, 0)


def test_oversampling_is_released_when_it_does_not_work_either(rig, capsys):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: False
    code = calibrate.run(cal_args(tmp_path), "ip", "eth0", ctrl, FakeHealth())
    assert code != calibrate.EXIT_OK
    assert ctrl.state["os_on"] == 0  # back on the IDDR path
    assert "trying oversample" in capsys.readouterr().out


def test_a_bitstream_without_the_feature_is_not_asked_to_oversample(rig, capsys):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: False
    code = calibrate.run(
        cal_args(tmp_path), "ip", "eth0", ctrl, FakeHealth(oversample=False)
    )
    assert code != calibrate.EXIT_OK
    assert ctrl.writes.count((ctrl.REG_OS_CTRL, 1)) == 0
    assert "trying" not in capsys.readouterr().out


def test_mode_oversample_needs_the_feature(rig, capsys):
    ctrl, tmp_path = rig
    a = cal_args(tmp_path, "--mode", "oversample")
    code = calibrate.run(a, "ip", "eth0", ctrl, FakeHealth(oversample=False))
    assert code == calibrate.EXIT_ERROR
    assert "no oversampling capture" in capsys.readouterr().out
    assert ctrl.writes == []


def test_mode_oversample_skips_the_idelay_scan(rig):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: st["os_on"] and 1 <= st["dly"] <= 5
    a = cal_args(tmp_path, "--mode", "oversample", "--vote")
    assert calibrate.run(a, "ip", "eth0", ctrl, FakeHealth()) == calibrate.EXIT_OK
    assert not any(addr == ctrl.REG_IDELAY_ALL for addr, _ in ctrl.writes)
    assert (ctrl.REG_OS_CTRL, 3) in ctrl.writes  # on + vote
    assert json.loads((tmp_path / "rec.json").read_text())["vote"] is True


def test_oversampling_without_a_measurable_clock_is_an_error(rig, capsys):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: False
    a = cal_args(tmp_path, "--mode", "oversample")
    code = calibrate.run(a, "ip", "eth0", ctrl, FakeHealth(half=None))
    assert code == calibrate.EXIT_ERROR
    assert "cannot measure TRACECLK" in capsys.readouterr().out


def test_oversampling_refuses_a_clock_that_is_too_fast(rig, capsys):
    ctrl, tmp_path = rig
    FakeProber.sim["good"] = lambda st: False
    a = cal_args(tmp_path, "--mode", "oversample")
    code = calibrate.run(a, "ip", "eth0", ctrl, FakeHealth(half=2.0))
    assert code == calibrate.EXIT_ERROR
    assert "too fast" in capsys.readouterr().out


def test_decode_args_reach_cortrace_decode(monkeypatch, tmp_path):
    seen = []

    def fake_run(cmd, **_kw):
        seen.append(cmd)
        return type("Done", (), {"stdout": "", "stderr": ""})()

    monkeypatch.setattr(calibrate.subprocess, "run", fake_run)
    prober = object.__new__(calibrate.Prober)
    prober.a = cal_args(
        tmp_path, "--decode-args", "--etm-idr0 0x08 --etm-configr '0x1'"
    )
    prober.raw, prober.syms = "cal.bin", "cal.nm"
    assert prober.decode() is None  # an empty summary parses to nothing
    cmd = seen[0]
    assert cmd[-4:] == ["--etm-idr0", "0x08", "--etm-configr", "0x1"]
    assert "--raw" in cmd and "x.elf" in cmd


def test_oversampling_accepts_the_fastest_clock_that_leaves_a_sample_point():
    # 30 MHz is 3 cycles of the 200 MHz clock per half period: dly 0 and 1 remain
    knob = calibrate.OversampleKnob("ip", None, 3, False)
    assert (knob.lo, knob.hi, knob.blank) == (0, 1, 1)
