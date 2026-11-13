"""A random but reproducible stream for judging the trace port: the target writes a
xorshift32 sequence to ITM stimulus port 0 (tools/prbs_itm), the TPIU sends it out
framed like any trace, and the host checks that every word is the successor of the
one before. A trace of ETM data is a poor test of the sampling point: it is short of
transitions, so it can decode without a single error where random data cannot
(an S32K358 on fly wires decoded ETM cleanly at 60 MHz and lost 99% of this stream).

  cortrace prbs start --probe-telnet gdb:127.0.0.1:7224 --bin prbs_itm.bin --base 0x20400000
      load the program into RAM and run it; arm the trace first (`cortrace arm
      --no-etm`), because arming rewrites ITM_TCR
  cortrace prbs check stream1.bin
      score a deframed ITM stream (cortrace-decode --raw --want-stream 1 --dump-etm)

`cortrace fpga ctrl calibrate --prbs` sweeps a sampling knob with the same score.
"""

import argparse
import struct
import sys

from .probe import PemicroRsp, ProbeError, open_probe

ITM_TCR = 0xE0000E80
ITM_TCR_PRBS = 0x00010001  # ITMENA, ATB id 1; no timestamps, sync or DWT packets
PACKET = b"\x03"  # ITM source packet: port 0, four bytes of payload
# A capture can be cut inside a packet at both ends (two words), and the odd word
# is lost where the TPIU resynchronises: measured, 2 of 41,000 over 0.3 s on a good
# link and up to 6 of 13,500 over 0.1 s. Up to 0.1% is not counted as an error.
TOLERANCE_WORDS = 2
TOLERANCE_FRACTION = 0.001


def xorshift32(x):
    """The successor of `x` in the sequence the firmware sends."""
    x ^= (x << 13) & 0xFFFFFFFF
    x ^= x >> 17
    x ^= (x << 5) & 0xFFFFFFFF
    return x


def chain_stats(data):
    """(estimated words sent, words whose successor follows them) in the bytes of
    a deframed ITM stream. A word counts when `03 w 03 xorshift32(w)` appears with
    the packets back to back; the estimate is the stream length over five bytes,
    so a stream that lost bytes scores low even if what is left is clean."""
    good = 0
    i = data.find(PACKET)
    while 0 <= i < len(data) - 9:
        (word,) = struct.unpack_from("<I", data, i + 1)
        if data[i + 5] == 3:
            (nxt,) = struct.unpack_from("<I", data, i + 6)
            if nxt == xorshift32(word):
                good += 1
        i = data.find(PACKET, i + 1)
    return len(data) // 5, good


def summary(data):
    """The numbers `calibrate` reads from a decode summary, from a PRBS stream:
    the events are the intact words; the words that went missing are what a
    sampling error costs."""
    est, good = chain_stats(data)
    return {
        "slice_events": good,
        "lost_sync": max(
            0, est - good - TOLERANCE_WORDS - int(est * TOLERANCE_FRACTION)
        ),
        "resync": 0,
        "addr_nacc": 0,
    }


def start(probe, image, base, tcr=ITM_TCR_PRBS):
    """Load `image` (a Cortex-M image: the stack pointer and the reset address in
    its first two words) at `base`, set ITM_TCR and run it."""
    if not isinstance(probe, PemicroRsp):
        raise ProbeError("`prbs start` needs the P&E GDB server (gdb:HOST:PORT)")
    if len(image) < 8:
        raise ProbeError("the image is too short for a vector table")
    sp, reset = struct.unpack_from("<II", image)
    pad = image + b"\0" * (-len(image) % 4)
    probe.halt()
    probe.write_bytes(base, pad)
    if probe.read_bytes(base, len(pad)) != pad:
        raise ProbeError(f"the program did not read back at 0x{base:08x}")
    probe.write32(ITM_TCR, tcr)
    probe.run_from(sp, reset & ~1)
    return sp, reset


def main(argv=None):
    """`cortrace prbs`."""
    ap = argparse.ArgumentParser(
        prog="cortrace prbs", description=__doc__.split("\n\n", maxsplit=1)[0]
    )
    sub = ap.add_subparsers(dest="cmd", required=True)
    ps = sub.add_parser("start", help="load the PRBS program and run it")
    ps.add_argument(
        "--probe-telnet", required=True, help="gdb:HOST:PORT of the P&E server"
    )
    ps.add_argument("--bin", required=True, help="prbs_itm.bin (tools/prbs_itm)")
    ps.add_argument(
        "--base", type=lambda v: int(v, 0), required=True, help="RAM address"
    )
    pc = sub.add_parser("check", help="score a deframed ITM stream")
    pc.add_argument("stream")
    a = ap.parse_args(argv)
    try:
        if a.cmd == "check":
            with open(a.stream, "rb") as f:
                data = f.read()
            est, good = chain_stats(data)
            print(
                f"{est} words sent (estimate), {good} followed by their successor "
                f"({good / max(est, 1):.1%})"
            )
            return 0 if est and summary(data)["lost_sync"] == 0 else 1
        probe = open_probe(a.probe_telnet)
        with open(a.bin, "rb") as f:
            sp, reset = start(probe, f.read(), a.base)
        print(f"running from 0x{reset & ~1:08x}, sp 0x{sp:08x}")
        probe.close()
    except (OSError, ProbeError) as exc:
        print(f"cortrace prbs: {exc}", file=sys.stderr)
        return 1
    return 0
