"""The ITM PRBS stream: the sequence, its score, and loading the program."""

import struct

import pytest
from test_probe import FakeGdbServer, wait_for

from cortrace import prbs
from cortrace.probe import PemicroRsp, ProbeError, open_probe


def words(n, x=0x12345678):
    out = []
    for _ in range(n):
        x = prbs.xorshift32(x)
        out.append(x)
    return out


def stream(ws):
    return b"".join(b"\x03" + struct.pack("<I", w) for w in ws)


def test_xorshift32_matches_the_firmware_sequence():
    # the first three words of the sequence tools/prbs_itm sends from 0x12345678,
    # as seen on the target
    assert words(3) == [0x87985AA5, 0x155B24A3, 0x4820F4C4]


def test_a_clean_stream_scores_every_word():
    est, good = prbs.chain_stats(stream(words(1000)))
    assert est == 1000
    assert good == 999  # the last word has no successor in the stream


def test_summary_of_a_clean_stream_has_no_errors():
    s = prbs.summary(stream(words(1000)))
    assert s["slice_events"] == 999
    assert s["lost_sync"] + s["resync"] + s["addr_nacc"] == 0


def test_a_flipped_bit_costs_the_word_and_its_neighbour():
    data = bytearray(stream(words(1000)))
    data[5 * 500 + 2] ^= 0x10
    est, good = prbs.chain_stats(bytes(data))
    assert est == 1000
    assert good == 999 - 2


def test_whole_packets_lost_show_as_fewer_events_not_as_errors():
    # calibrate's event count (at least half of the best setting) catches this case
    ws = words(1000)
    s = prbs.summary(stream(ws[:400]) + stream(ws[600:]))
    assert s["lost_sync"] == 0
    assert s["slice_events"] == 798  # 800 words, 799 pairs, one broken at the seam


def test_a_stream_of_noise_scores_nothing():
    noise = bytes((i * 37 + 11) & 0xFF for i in range(5000))
    s = prbs.summary(noise)
    assert s["slice_events"] == 0
    assert s["lost_sync"] > 900


def test_calibrate_judges_a_setting_by_the_prbs_score():
    from cortrace.fpga import calibrate

    good = prbs.summary(stream(words(2000)))
    bad = prbs.summary(stream(words(2000))[:5000] + bytes(5000))
    assert calibrate.sampling_errors(good) == 0
    assert calibrate.sampling_errors(bad) > 0


@pytest.fixture(name="rig")
def fixture_rig(monkeypatch):
    monkeypatch.setattr(PemicroRsp, "HALT_SETTLE", 0)
    monkeypatch.setattr(prbs, "ITM_TCR", 0x3F0)  # inside the fake's memory
    server = FakeGdbServer(bytes(0x400))
    probe = open_probe(f"gdb:127.0.0.1:{server.port}")
    yield server, probe
    probe.close()


def test_start_loads_the_image_sets_tcr_and_runs_it(rig):
    server, probe = rig
    image = struct.pack("<II", 0x2000_1000, 0x2040_0009) + bytes(range(8))
    sp, reset = prbs.start(probe, image, 0x100)
    assert (sp, reset) == (0x2000_1000, 0x2040_0009)
    wait_for(lambda: not server.halted)
    assert bytes(server.memory[0x100:0x110]) == image
    assert struct.unpack_from("<I", server.memory, 0x3F0)[0] == prbs.ITM_TCR_PRBS
    assert server.regs == {13: 0x2000_1000, 15: 0x2040_0008}  # Thumb bit dropped


def test_start_needs_the_pemicro_server():
    class NotGdb:  # pylint: disable=too-few-public-methods
        pass

    with pytest.raises(ProbeError, match="P&E"):
        prbs.start(NotGdb(), bytes(16), 0)


def test_start_rejects_an_image_without_a_vector_table(rig):
    _, probe = rig
    with pytest.raises(ProbeError, match="too short"):
        prbs.start(probe, b"\x00" * 4, 0x100)
