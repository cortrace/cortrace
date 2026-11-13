"""Debug-probe access for the host tools: read and write target memory.

Only what the tools need. There are two implementations. One talks to an
OpenOCD that is already running (its telnet port); a second OpenOCD would take
the probe away and clear the DWT configuration that the trace depends on. The
other talks GDB remote serial protocol to the GDB server of a P&E Micro
Multilink (`pegdbserver_console`), which is the on-board debugger of NXP's
S32K3 evaluation boards. `open_probe` picks one from the probe spec.
"""

import re
import socket
import time

PROMPT = b"> "
_WORD = re.compile(r"0x[0-9a-fA-F]+:\s+([0-9a-fA-F]{8})")
_LINE = re.compile(r"\s*0x([0-9a-fA-F]+):\s+((?:[0-9a-fA-F]{2}\s*)+)")


class ProbeError(Exception):
    """The probe could not be reached or did not answer."""


class OpenOcdTelnet:
    """Read target memory through a running OpenOCD's telnet server."""

    def __init__(self, hostport, timeout=5.0):
        host, _, port = hostport.rpartition(":")
        try:
            self._sock = socket.create_connection(
                (host or "127.0.0.1", int(port)), timeout=timeout
            )
        except (OSError, ValueError) as exc:
            raise ProbeError(f"cannot connect to OpenOCD at {hostport}: {exc}") from exc
        self._read_until_prompt()

    def close(self):
        """Close the connection."""
        self._sock.close()

    def _read_until_prompt(self):
        buf = b""
        try:
            while not buf.endswith(PROMPT):
                chunk = self._sock.recv(65536)
                if not chunk:
                    break
                buf += chunk
        except socket.timeout as exc:
            raise ProbeError("OpenOCD did not answer in time") from exc
        return buf.replace(b"\x00", b"").decode(errors="replace")

    def command(self, text):
        """Run one OpenOCD command, return its output."""
        self._sock.sendall(text.encode() + b"\n")
        return self._read_until_prompt()

    def _checked(self, text):
        out = self.command(text)
        if "rror" in out:
            raise ProbeError(f"OpenOCD: `{text}` failed: {out.strip()}")
        return out

    def read32(self, addr):
        """The 32-bit register or word at `addr`."""
        out = self._checked(f"mdw 0x{addr:08x}")
        match = _WORD.search(out)
        if not match:
            raise ProbeError(f"cannot read 0x{addr:08x}: {out.strip()}")
        return int(match.group(1), 16)

    def write32(self, addr, value):
        """Write a 32-bit register or word."""
        self._checked(f"mww 0x{addr:08x} 0x{value:08x}")

    def halt(self):
        """Stop the core."""
        self._checked("halt")

    def resume(self):
        """Let the core run again."""
        self._checked("resume")

    def read_bytes(self, addr, size):
        """`size` bytes at `addr`, or None if the target refused the read."""
        out = {}
        for line in self.command(f"mdb 0x{addr:08x} {size}").splitlines():
            match = _LINE.match(line)
            if not match:
                continue
            base = int(match.group(1), 16)
            for i, tok in enumerate(match.group(2).split()):
                out[base + i] = int(tok, 16)
        if any(addr + i not in out for i in range(size)):
            return None
        return bytes(out[addr + i] for i in range(size))


GDB_PREFIX = "gdb:"


class PemicroRsp:
    """Read and write target memory through the P&E GDB server (RSP over TCP).
    The server answers memory accesses only while the core is halted (a read of a
    running core returns zeros), has no `vCont` and no `monitor` commands. So:
    `halt` is `c` followed by Ctrl-C, which stops the core whatever state it was
    in, and a memory access outside `halt`..`resume` halts the core for its own
    duration and lets it run again.
    """

    # The server ignores a Ctrl-C that follows `c` at once.
    HALT_SETTLE = 0.4

    def __init__(self, hostport, timeout=10.0):
        host, _, port = hostport.rpartition(":")
        try:
            self._sock = socket.create_connection(
                (host or "127.0.0.1", int(port)), timeout=timeout
            )
        except (OSError, ValueError) as exc:
            raise ProbeError(
                f"cannot connect to GDB server at {hostport}: {exc}"
            ) from exc
        self._buf = b""
        self._halted = False
        features = self._request("qSupported:swbreak+;hwbreak+")
        match = re.search(r"PacketSize=([0-9a-fA-F]+)", features)
        packet = int(match.group(1), 16) if match else 0x100
        self._chunk = max(4, (packet - 16) // 2) & ~3  # data bytes per m / M packet

    def close(self):
        """Close the connection. The core keeps its state."""
        self._sock.close()

    def _send(self, payload):
        checksum = sum(payload.encode()) & 0xFF
        self._sock.sendall(f"${payload}#{checksum:02x}".encode())

    def _reply(self):
        """The payload of the next packet, acknowledged."""
        try:
            while True:
                start = self._buf.find(b"$")
                end = self._buf.find(b"#", start + 1) if start >= 0 else -1
                if end >= 0 and len(self._buf) >= end + 3:
                    payload = self._buf[start + 1 : end]
                    self._buf = self._buf[end + 3 :]
                    self._sock.sendall(b"+")
                    return payload.decode(errors="replace")
                chunk = self._sock.recv(65536)
                if not chunk:
                    raise ProbeError("GDB server closed the connection")
                self._buf += chunk
        except socket.timeout as exc:
            raise ProbeError("GDB server did not answer in time") from exc
        except OSError as exc:
            raise ProbeError(f"GDB server connection failed: {exc}") from exc

    def _request(self, payload):
        self._send(payload)
        return self._reply()

    def _read(self, addr, size):
        data = b""
        while len(data) < size:
            count = min(self._chunk, size - len(data))
            out = self._request(f"m{addr + len(data):x},{count:x}")
            if len(out) != 2 * count or not re.fullmatch(r"[0-9a-fA-F]+", out):
                return None
            data += bytes.fromhex(out)
        return data

    def _write(self, addr, data):
        for off in range(0, len(data), self._chunk):
            part = data[off : off + self._chunk]
            out = self._request(f"M{addr + off:x},{len(part):x}:{part.hex()}")
            if out != "OK":
                raise ProbeError(f"cannot write 0x{addr + off:08x}: {out}")

    def _while_halted(self, func):
        if self._halted:
            return func()
        self.halt()
        try:
            return func()
        finally:
            self.resume()

    def halt(self):
        """Stop the core (also when it is stopped already)."""
        for _ in range(3):
            self._send("c")
            time.sleep(self.HALT_SETTLE)
            self._sock.sendall(b"\x03")
            if self._reply()[:1] in ("T", "S"):
                self._halted = True
                return
        raise ProbeError("GDB server did not stop the core")

    def resume(self):
        """Let the core run again."""
        self._send("c")
        self._halted = False

    def read32(self, addr):
        """The 32-bit register or word at `addr`."""
        data = self._while_halted(lambda: self._read(addr, 4))
        if data is None:
            raise ProbeError(f"cannot read 0x{addr:08x}")
        return int.from_bytes(data, "little")

    def write32(self, addr, value):
        """Write a 32-bit register or word."""
        self._while_halted(lambda: self._write(addr, value.to_bytes(4, "little")))

    def read_bytes(self, addr, size):
        """`size` bytes at `addr`, or None if the target refused the read."""
        return self._while_halted(lambda: self._read(addr, size))

    def write_bytes(self, addr, data):
        """Write `data` (the length a multiple of 4) at `addr`."""
        self._while_halted(lambda: self._write(addr, bytes(data)))

    def run_from(self, sp, pc):
        """Start the core at `pc` (the address of the code; the Thumb bit is
        the target's business) with the stack pointer `sp`."""
        self.halt()
        for reg, value in (("d", sp), ("f", pc)):  # r13, r15
            out = self._request(f"P{reg}={value.to_bytes(4, 'little').hex()}")
            if out != "OK":
                raise ProbeError(f"cannot set register {reg}: {out}")
        self.resume()


def open_probe(spec):
    """The probe a `--probe-telnet` value names: `HOST:PORT` is a running
    OpenOCD's telnet port, `gdb:HOST:PORT` a P&E GDB server."""
    if spec.startswith(GDB_PREFIX):
        return PemicroRsp(spec[len(GDB_PREFIX) :])
    return OpenOcdTelnet(spec)
