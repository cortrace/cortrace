"""ctrl — write runtime CSRs to trace_ddr_stream_top over UDP :5002.

CSR map (fpga_core_net control port, doc 15):
  0x02  soft re-arm : any write re-arms the one-shot capture (no reflash)
  0x03/04 SWO bit length lo/hi
  0x05  IDELAY tap, all four data lanes (0..31, ~78 ps each; power-on 16)
  0x06  IDELAY tap of one lane: {lane[6:5], tap[4:0]}
  0x08  TPIU port width: 4, 2 or 1 bit -- runtime, so ONE bitstream serves all
        three widths (readback at DEPTH+33)
  0x09  stream selftest ramp   0x0B  fixed 0x42 source

The data lanes go through an IDELAYE2 in front of the IDDR (bitstream feature
"run-time IDELAY tap", `cortrace fpga health`); the tap moves the sampling
point inside the data eye. Bitstreams older than cortrace-fpga 1.1.0 have no
delay element and ignore 0x05/0x06.

Request payload = {reg_addr(1B), reg_value(1B)} (+pad); the FPGA latches both
and pulses csr_we at end-of-frame.

Usage:
  cortrace fpga ctrl --ip 192.168.10.42 set-width 4
  cortrace fpga ctrl --ip 192.168.10.42 set-tap 16
  cortrace fpga ctrl --ip 192.168.10.42 set-tap 12 --lane 2
  cortrace fpga ctrl --ip 192.168.10.42 rearm
"""

import argparse
import socket

from . import net as fpga_net

CTRL_PORT = 5002
_IFACE = None  # set by main() from discovery; write_csr binds to it
REG_REARM = 0x02
REG_BITLEN_LO = 0x03
REG_BITLEN_HI = 0x04
REG_IDELAY_ALL = 0x05  # IDELAY tap of all four data lanes, 0..31
REG_IDELAY_LANE = 0x06  # IDELAY tap of one lane: {lane[6:5], tap[4:0]}
IDELAY_STEP_PS = 78.125  # one tap at the 200 MHz IDELAYCTRL reference
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


def main(argv=None):
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
    sub.add_parser("rearm")
    a = ap.parse_args(argv)

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
    elif a.cmd == "rearm":
        write_csr(a.ip, REG_REARM, 1)
        print("soft re-arm pulsed")
    return 0
