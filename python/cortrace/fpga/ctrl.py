"""ctrl — write runtime CSRs to trace_ddr_stream_top over UDP :5002.

CSR map (fpga_core_net control port, doc 15):
  0x02  soft re-arm : any write re-arms the one-shot capture (no reflash)
  0x03/04 SWO bit length lo/hi
  0x05  IDELAY tap, all four data lanes (0..31, ~78 ps each; power-on 16)
  0x06  IDELAY tap of one lane: {lane[6:5], tap[4:0]}
  0x14/0x15  MMCM sampling clock fine phase (signed 10 bit, VCO/56 steps; low
        byte first, applied by the 0x15 write), 0x16 reset the MMCM
  0x17  sampling clock source of a dual build: 0 TRACECLK direct, 1 MMCM
  0x18  oversampling capture (feature "oversampling capture"): bit0 on, bit1 vote;
        the pins are sampled at 200 MHz and a nibble taken `dly` cycles after
        each TRACECLK edge, for targets whose data changes with the edge
  0x19  oversampling sample point (cycles after the edge, 0..31, power-on 5)
  0x1A  oversampling dead time after an accepted edge (cycles, power-on 3)
  0x1B  any write clears the oversampling status
  0x08  TPIU port width: 4, 2 or 1 bit -- runtime, so ONE bitstream serves all
        three widths (readback at DEPTH+33)
  0x09  stream selftest ramp   0x0B  fixed 0x42 source

The data lanes go through an IDELAYE2 in front of the IDDR (bitstream feature
"run-time IDELAY tap", `cortrace fpga health`); the tap moves the sampling
point inside the data eye. Bitstreams older than cortrace-fpga 1.1.0 have no
delay element and ignore 0x05/0x06.

Bitstreams built with TRACE_USE_MMCM=1 (feature "MMCM sampling clock") clock the
IDDRs from an MMCM; `set-phase` moves that clock in 1/56 VCO-period steps, which
reaches far beyond the 2.4 ns of IDELAY.

Request payload = {reg_addr(1B), reg_value(1B)} (+pad); the FPGA latches both
and pulses csr_we at end-of-frame.

Usage:
  cortrace fpga ctrl --ip 192.168.10.42 set-width 4
  cortrace fpga ctrl --ip 192.168.10.42 set-tap 16
  cortrace fpga ctrl --ip 192.168.10.42 set-tap 12 --lane 2
  cortrace fpga ctrl --ip 192.168.10.42 set-phase -120
  cortrace fpga ctrl --ip <FPGA_IP> oversample on --dly 5 --blank 3
  cortrace fpga ctrl --ip <FPGA_IP> oversample status
  cortrace fpga ctrl --ip 192.168.10.42 relock
  cortrace fpga ctrl --ip 192.168.10.42 rearm
"""

import argparse
import socket
import sys
import time

from . import calibrate, health
from . import net as fpga_net

CTRL_PORT = 5002
_IFACE = None  # set by main() from discovery; write_csr binds to it
REG_REARM = 0x02
REG_BITLEN_LO = 0x03
REG_BITLEN_HI = 0x04
REG_IDELAY_ALL = 0x05  # IDELAY tap of all four data lanes, 0..31
REG_IDELAY_LANE = 0x06  # IDELAY tap of one lane: {lane[6:5], tap[4:0]}
IDELAY_STEP_PS = 78.125  # one tap at the 200 MHz IDELAYCTRL reference
REG_MM_PHASE_LO = 0x14  # MMCM fine phase target, low 8 bits (latched)
REG_MM_PHASE_HI = 0x15  # bits 9:8; this write applies the target
REG_MM_RELOCK = 0x16  # any write resets the sampling MMCM
MM_MAX_STEPS = 255  # the RTL clamps to this
MM_STEPS_PER_VCO = 56  # one step = 1/56 of the VCO period
REG_CLK_SEL = 0x17  # dual build: 0 = TRACECLK direct (IDELAY), 1 = MMCM
REG_OS_CTRL = 0x18  # bit0 oversampling capture on, bit1 majority vote
REG_OS_DLY = 0x19  # sample point, cycles after the TRACECLK edge
REG_OS_BLANK = 0x1A  # dead time after an accepted edge, cycles
REG_OS_CLR = 0x1B  # any write clears the oversampling status
OS_MAX_CYCLES = 31
REG_WIDTH = 0x08  # TPIU parallel port width: 4, 2 or 1 (runtime, no reflash)
REG_STREAM_SELFTEST = 0x09  # 1=stream FPGA-side byte ramp instead of real trace
REG_IDDR_PRBS = 0x0D  # 1=push a trace_clk-domain xorshift32 PRBS through the
# SAME IDDR-side CDC FIFO (stresses IDDR->CDC->DDR->UDP)


def write_csr(ip, addr, value, timeout=1.0, iface=None):
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    # CSR writes are ACTIVE sends to the FPGA: with a second NIC on the same
    # subnet (dock direct-attach) the kernel routes them out the wrong port, so
    # rearm/set-width silently never reach the FPGA. Pin to the wired iface.
    iface = iface or _IFACE
    if iface:
        try:
            s.setsockopt(
                socket.SOL_SOCKET, socket.SO_BINDTODEVICE, (iface + "\0").encode()
            )
        except PermissionError:
            pass
    s.settimeout(timeout)
    # payload: addr, value, then a few pad bytes so the frame has a clear tlast
    payload = bytes([addr & 0xFF, value & 0xFF, 0, 0])
    s.sendto(payload, (ip, CTRL_PORT))
    try:
        s.recvfrom(2048)  # reply is echoed; ignore content
    except socket.timeout:
        pass  # write is fire-and-forget; reply optional
    s.close()


def _finish_set_phase(a, timeout_s=3.0):
    """Wait for the MMCM to reach the phase, re-arm, and report."""
    step = ""
    if a.pin_mhz:
        ps = 1e6 / (MM_STEPS_PER_VCO * a.pin_mhz * a.mmcm_mult)
        step = f" (~{a.value * ps:+.0f} ps at {a.pin_mhz:g} MHz, {ps:.1f} ps/step)"
    if a.no_wait:
        print(f"MMCM fine phase target = {a.value:+d} steps{step}")
        return 0
    deadline = time.monotonic() + timeout_s
    status = None
    while time.monotonic() < deadline:
        try:
            status = health.read_mmcm_status(a.ip)
        except socket.timeout:
            status = None
        if status and not status[1] and status[3] == a.value:
            break
        time.sleep(0.05)
    if status is None:
        print("no answer on :5001 (is this an MMCM bitstream?)")
        return 1
    locked, _, timeout, phase, _ = status
    if not locked:
        print(
            "sampling MMCM is not locked: no TRACECLK, or the frequency is out of range"
        )
        return 1
    if timeout:
        print("the MMCM did not answer a phase step")
        return 1
    if phase != a.value:
        print(f"MMCM fine phase is {phase:+d}, wanted {a.value:+d}")
        return 1
    # the sampling clock moved while data was flowing; re-arm for a clean capture
    write_csr(a.ip, REG_REARM, 1)
    print(f"MMCM fine phase = {phase:+d} steps{step}, capture re-armed")
    return 0


def _print_os_status(st):
    clock = (
        f"TRACECLK {st['pin_mhz']:.2f} MHz (high {st['rf']} / low {st['fr']} cycles)"
        if st["pin_mhz"]
        else "TRACECLK not measurable (absent, or slower than 0.8 MHz)"
    )
    print(
        f"oversampling {'on' if st['on'] else 'off'}"
        f"{', vote' if st['vote'] else ''}: dly {st['dly']}, blank {st['blank']}; "
        f"{clock}; {st['rejects']} edges rejected, {st['bytes']} bytes"
    )


def _oversample(a):
    """`oversample on|off|status`."""
    if a.action == "status":
        try:
            st = health.read_os_status(a.ip)
        except OSError:
            print("no answer on :5001")
            return 1
        _print_os_status(st)
        return 0
    if a.action == "off":
        write_csr(a.ip, REG_OS_CTRL, 0)
        write_csr(a.ip, REG_REARM, 1)
        print("oversampling off (IDDR capture), capture re-armed")
        return 0
    if a.blank is not None:
        write_csr(a.ip, REG_OS_BLANK, a.blank)
    if a.dly is not None:
        write_csr(a.ip, REG_OS_DLY, a.dly)
    write_csr(a.ip, REG_OS_CLR, 1)
    write_csr(a.ip, REG_OS_CTRL, 1 | (2 if a.vote else 0))
    write_csr(a.ip, REG_REARM, 1)
    print(f"oversampling on{', vote' if a.vote else ''}, capture re-armed")
    return 0


def main(argv=None):  # pylint: disable=too-many-statements
    ap = argparse.ArgumentParser()
    ap.add_argument(
        "--ip",
        default=None,
        help="FPGA IP (default: auto-discover, fallback 192.168.10.42)",
    )
    ap.add_argument(
        "--iface",
        default=None,
        help="bind CSR writes to this NIC (default: auto-discover)",
    )
    ap.add_argument("--no-discover", action="store_true")
    sub = ap.add_subparsers(dest="cmd", required=True)
    pb = sub.add_parser(
        "set-bitlen", help="SWO NRZ bit length in ref_200m cycles (=200e6/baud)"
    )
    pb.add_argument("value", type=int)
    pw = sub.add_parser(
        "set-width", help="TPIU port width 4/2/1 bits (runtime, no reflash)"
    )
    pw.add_argument("value", type=int, choices=(4, 2, 1))
    pt = sub.add_parser(
        "set-tap",
        help="IDELAY tap of the trace data lanes, 0..31 (~78 ps each; runtime, "
        "no reflash). Moves the sampling point inside the data eye.",
    )
    pt.add_argument("value", type=int, choices=range(32), metavar="0..31")
    pt.add_argument(
        "--lane",
        type=int,
        choices=range(4),
        default=None,
        help="set only this data lane (default: all four)",
    )
    ps = sub.add_parser(
        "stream-selftest",
        help="stream an FPGA-side byte ramp instead of real "
        "trace (isolates the UDP path from ETM)",
    )
    ps.add_argument("value", type=int, choices=(0, 1))
    pp = sub.add_parser(
        "iddr-prbs",
        help="push a trace_clk-domain xorshift32 PRBS through "
        "the SAME CDC FIFO as the IDDR sample (stresses "
        "IDDR->CDC->DDR->UDP with a host-reproducible, "
        "violently-changing pattern; bypasses only the "
        "IDDR primitive)",
    )
    pp.add_argument("value", type=int, choices=(0, 1))
    pm = sub.add_parser(
        "set-phase",
        help="fine phase of the MMCM sampling clock (bitstreams with the MMCM "
        "feature), in VCO/56 steps, -255..255; waits until the MMCM got there",
    )
    pm.add_argument("value", type=int, metavar="-255..255")
    pm.add_argument(
        "--pin-mhz",
        type=float,
        default=None,
        help="TRACECLK in MHz, only to print the step in ps",
    )
    pm.add_argument(
        "--mmcm-mult",
        type=int,
        default=14,
        help="VCO multiplier the bitstream was built with (default: %(default)s)",
    )
    pm.add_argument("--no-wait", action="store_true")
    sub.add_parser("relock", help="reset the sampling MMCM (lock again)")
    pc = sub.add_parser(
        "set-mode",
        help="sampling clock of a dual bitstream (feature 'selectable sampling "
        "clock'): direct = TRACECLK through a global buffer, tuned with set-tap; "
        "mmcm = the MMCM, tuned with set-phase. Re-arms the capture.",
    )
    pc.add_argument("mode", choices=("direct", "mmcm"))
    pcal = sub.add_parser(
        "calibrate",
        help="scan the sampling knob (IDELAY tap or MMCM phase), decode the running "
        "target at each setting and write the middle of the clean window",
    )
    calibrate.add_arguments(pcal)
    po = sub.add_parser(
        "oversample",
        help="oversampling capture (bitstreams with the feature): sample the pins at "
        "200 MHz and take a nibble a set time after each TRACECLK edge. For targets "
        "whose data changes with the clock edge, where no IDELAY tap can sample.",
    )
    po.add_argument("action", choices=("on", "off", "status"))
    po.add_argument(
        "--dly",
        type=int,
        default=None,
        choices=range(OS_MAX_CYCLES + 1),
        metavar="0..31",
        help="sample point, 200 MHz cycles after the edge (5 ns each; default: "
        "leave as is). It has to lie before the next edge.",
    )
    po.add_argument(
        "--blank",
        type=int,
        default=None,
        choices=range(OS_MAX_CYCLES + 1),
        metavar="0..31",
        help="dead time after an edge, cycles: edges closer than this (ringing) are "
        "ignored. Keep it below half a TRACECLK period.",
    )
    po.add_argument(
        "--vote",
        action="store_true",
        help="take the majority of three samples (removes one-sample glitches)",
    )
    sub.add_parser("rearm")
    a = ap.parse_args(argv)
    if a.cmd == "set-phase" and abs(a.value) > MM_MAX_STEPS:
        ap.error(f"set-phase: {a.value} is outside -{MM_MAX_STEPS}..{MM_MAX_STEPS}")

    global _IFACE  # pylint: disable=global-statement
    ip = a.ip
    _IFACE = a.iface
    if not a.no_discover and (ip is None or _IFACE is None):
        try:
            info = fpga_net.discover_fpga(ip=a.ip or fpga_net.DEFAULT_FPGA_IP)
            if info:
                ip = ip or info["ip"]
                _IFACE = _IFACE or info["iface"]
        except PermissionError:
            pass
    a.ip = ip or fpga_net.DEFAULT_FPGA_IP

    if a.cmd == "set-bitlen":
        write_csr(a.ip, REG_BITLEN_LO, a.value & 0xFF)
        write_csr(a.ip, REG_BITLEN_HI, (a.value >> 8) & 0xFF)
        print(f"set SWO bitlen = {a.value} ref cycles (~{200e6/a.value/1e6:.3f} Mbaud)")
    elif a.cmd == "set-width":
        # The RTL accepts the literal width (4/2/1) and maps it to the traceIF
        # encoding itself, so one bitstream covers all widths. Changing width
        # resets traceIF's frame assembly, so re-arm afterwards for a clean
        # capture.
        write_csr(a.ip, REG_WIDTH, a.value)
        write_csr(a.ip, REG_REARM, 1)
        print(
            f"set TPIU port width = {a.value} bit (traceIF re-synced, capture re-armed)"
        )
    elif a.cmd == "set-tap":
        if a.lane is None:
            write_csr(a.ip, REG_IDELAY_ALL, a.value)
            where = "all data lanes"
        else:
            write_csr(a.ip, REG_IDELAY_LANE, (a.lane << 5) | a.value)
            where = f"data lane {a.lane}"
        # The delay is reloaded while data is moving; re-arm for a clean capture.
        write_csr(a.ip, REG_REARM, 1)
        print(
            f"IDELAY tap = {a.value} (~{a.value * IDELAY_STEP_PS:.0f} ps) on {where}, "
            "capture re-armed"
        )
    elif a.cmd == "set-phase":
        raw = a.value & 0x3FF
        write_csr(a.ip, REG_MM_PHASE_LO, raw & 0xFF)
        write_csr(a.ip, REG_MM_PHASE_HI, raw >> 8)
        return _finish_set_phase(a)
    elif a.cmd == "set-mode":
        write_csr(a.ip, REG_CLK_SEL, 1 if a.mode == "mmcm" else 0)
        write_csr(a.ip, REG_REARM, 1)
        print(f"sampling clock = {a.mode}, capture re-armed")
    elif a.cmd == "calibrate":
        return calibrate.run(a, a.ip, _IFACE, sys.modules[__name__], health)
    elif a.cmd == "relock":
        write_csr(a.ip, REG_MM_RELOCK, 1)
        print("sampling MMCM reset requested")
    elif a.cmd == "stream-selftest":
        write_csr(a.ip, REG_STREAM_SELFTEST, a.value)
        print(
            f"STREAM data source = "
            f"{'FPGA byte ramp (selftest)' if a.value else 'real trace'}"
        )
    elif a.cmd == "iddr-prbs":
        write_csr(a.ip, REG_IDDR_PRBS, a.value)
        print(
            f"IDDR-side CDC source = "
            f"{'trace_clk xorshift32 PRBS' if a.value else 'real IDDR sample'}"
        )
    elif a.cmd == "oversample":
        return _oversample(a)
    elif a.cmd == "rearm":
        write_csr(a.ip, REG_REARM, 1)
        print("soft re-arm pulsed")
    return 0
