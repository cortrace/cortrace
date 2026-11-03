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
