"""OpenOcdTelnet and PemicroRsp against tiny fake servers."""

import socket
import threading
import time

import pytest

from cortrace.probe import OpenOcdTelnet, PemicroRsp, ProbeError, open_probe


class FakeOpenOcd:
    """Answers `mdb ADDR COUNT` like OpenOCD does (16 bytes per line)."""

    def __init__(self, memory, fail=None):
        self.memory = memory  # {addr: byte}
        self.words = {}  # {addr: 32-bit value} for mdw / mww
        self.log = []
        self.fail = fail  # command word that answers with an error
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        self.thread = threading.Thread(target=self.serve, daemon=True)
        self.thread.start()

    def serve(self):
        conn, _ = self.server.accept()
        conn.sendall(b"Open On-Chip Debugger\r\n> ")
        buf = b""
        while True:
            data = conn.recv(4096)
            if not data:
                break
            buf += data
            while b"\n" in buf:
                line, buf = buf.split(b"\n", 1)
                conn.sendall(self.answer(line.decode().strip()) + b"> ")
        conn.close()

    def answer(self, command):
        word, *rest = command.split()
        self.log.append(command)
        if word in ("halt", "resume"):
            return b"" if self.fail != word else b"Error: no target\r\n"
        if word == "mww":
            self.words[int(rest[0], 0)] = int(rest[1], 0)
            return b"" if self.fail != word else b"Error: write failed\r\n"
        if word == "mdw":
            addr = int(rest[0], 0)
            if addr not in self.words:
                return b"\r\n"
            return f"0x{addr:08x}: {self.words[addr]:08x} \r\n".encode()
        assert word == "mdb"
        addr, count = int(rest[0], 0), int(rest[1])
        lines = []
        for base in range(addr, addr + count, 16):
            row = [
                self.memory[a]
                for a in range(base, min(base + 16, addr + count))
                if a in self.memory
            ]
            if row:
                lines.append(f"0x{base:08x}: " + " ".join(f"{b:02x}" for b in row))
        return ("\r\n".join(lines) + "\r\n").encode()


def test_reads_bytes_across_lines():
    memory = {0x2000_0000 + i: i for i in range(40)}
    server = FakeOpenOcd(memory)
    probe = OpenOcdTelnet(f"127.0.0.1:{server.port}")
    assert probe.read_bytes(0x2000_0000, 40) == bytes(range(40))
    assert probe.read_bytes(0x2000_0004, 4) == bytes([4, 5, 6, 7])
    probe.close()


def test_a_refused_read_gives_none():
    server = FakeOpenOcd({0x2000_0000: 1})
    probe = OpenOcdTelnet(f"127.0.0.1:{server.port}")
    assert probe.read_bytes(0x2000_0000, 4) is None
    assert probe.read_bytes(0x3000_0000, 4) is None
    probe.close()


def test_registers_are_written_and_read_back():
    server = FakeOpenOcd({})
    probe = OpenOcdTelnet(f"127.0.0.1:{server.port}")
    probe.halt()
    probe.write32(0xE0001020, 0x20000410)
    assert probe.read32(0xE0001020) == 0x20000410
    probe.resume()
    assert server.log == [
        "halt",
        "mww 0xe0001020 0x20000410",
        "mdw 0xe0001020",
        "resume",
    ]
    probe.close()


def test_unreadable_register_is_a_probe_error():
    server = FakeOpenOcd({})
    probe = OpenOcdTelnet(f"127.0.0.1:{server.port}")
    with pytest.raises(ProbeError, match="cannot read 0xe0001020"):
        probe.read32(0xE0001020)
    probe.close()


@pytest.mark.parametrize("which", ["halt", "resume", "mww"])
def test_openocd_errors_are_probe_errors(which):
    server = FakeOpenOcd({}, fail=which)
    probe = OpenOcdTelnet(f"127.0.0.1:{server.port}")
    with pytest.raises(ProbeError, match="failed"):
        {
            "halt": probe.halt,
            "resume": probe.resume,
            "mww": lambda: probe.write32(4, 5),
        }[which]()
    probe.close()


def test_connection_failure_is_a_probe_error():
    with pytest.raises(ProbeError, match="cannot connect"):
        OpenOcdTelnet("127.0.0.1:1")
    with pytest.raises(ProbeError, match="cannot connect"):
        OpenOcdTelnet("127.0.0.1:notaport")


def test_a_silent_server_times_out():
    server = socket.socket()
    server.bind(("127.0.0.1", 0))
    server.listen(1)
    with pytest.raises(ProbeError, match="did not answer"):
        OpenOcdTelnet(f"127.0.0.1:{server.getsockname()[1]}", timeout=0.2)
    server.close()


class FakeGdbServer:  # pylint: disable=too-many-instance-attributes
    """The P&E GDB server's manners: memory answers only while halted (zeros
    otherwise), `c` has no reply, Ctrl-C halts and answers T02."""

    def __init__(self, memory, packet_size=0x40, ctrl_c_after_c=True):
        self.memory = bytearray(memory)  # the target memory from address 0
        self.regs = {}  # register number -> value, from `P` packets
        self.halted = False
        self.packet_size = packet_size
        self.log = []
        self.ctrl_c_after_c = ctrl_c_after_c
        self.server = socket.socket()
        self.server.bind(("127.0.0.1", 0))
        self.server.listen(1)
        self.port = self.server.getsockname()[1]
        threading.Thread(target=self.serve, daemon=True).start()

    @staticmethod
    def packet(payload):
        return f"${payload}#{sum(payload.encode()) & 0xFF:02x}".encode()

    def serve(self):
        conn, _ = self.server.accept()
        buf = b""
        while True:
            try:
                data = conn.recv(4096)
            except ConnectionResetError:  # the client closed with an unread ack
                break
            if not data:
                break
            buf += data
            while buf:
                if buf[:1] in (b"+", b"-"):
                    buf = buf[1:]
                elif buf[:1] == b"\x03":
                    buf = buf[1:]
                    self.log.append("^C")
                    if self.ctrl_c_after_c:
                        self.halted = True
                        conn.sendall(self.packet("T02"))
                elif b"#" in buf and len(buf) >= buf.index(b"#") + 3:
                    end = buf.index(b"#") + 3
                    payload = buf[1 : end - 3].decode()
                    buf = buf[end:]
                    conn.sendall(b"+")
                    reply = self.answer(payload)
                    if reply is not None:
                        conn.sendall(self.packet(reply))
                else:
                    break
        conn.close()

    def answer(self, payload):  # pylint: disable=too-many-return-statements
        self.log.append(payload)
        if payload.startswith("qSupported"):
            return f"PacketSize={self.packet_size:x};qXfer:memory-map:read+"
        if payload == "c":
            self.halted = False
            return None
        if payload[0] == "m":
            addr, count = (int(x, 16) for x in payload[1:].split(","))
            if addr + count > len(self.memory):
                return "E01"
            if not self.halted:
                return "00" * count
            return bytes(self.memory[addr : addr + count]).hex()
        if payload[0] == "P":
            number, value = payload[1:].split("=")
            self.regs[int(number, 16)] = int.from_bytes(bytes.fromhex(value), "little")
            return "OK"
        assert payload[0] == "M"
        head, data = payload[1:].split(":")
        addr, count = (int(x, 16) for x in head.split(","))
        assert self.halted
        self.memory[addr : addr + count] = bytes.fromhex(data)
        return "OK"


def wait_for(condition):
    """The fake server runs in a thread: give it a moment to see a command."""
    for _ in range(200):
        if condition():
            return
        time.sleep(0.01)
    raise AssertionError("the fake server did not get there")


@pytest.fixture(name="gdb_probe")
def fixture_gdb_probe(monkeypatch):
    monkeypatch.setattr(PemicroRsp, "HALT_SETTLE", 0)
    server = FakeGdbServer(bytes(range(256)) * 4)
    probe = open_probe(f"gdb:127.0.0.1:{server.port}")
    yield server, probe
    probe.close()


def test_open_probe_picks_the_implementation_by_prefix(gdb_probe):
    _, probe = gdb_probe
    assert isinstance(probe, PemicroRsp)
    server = FakeOpenOcd({})
    other = open_probe(f"127.0.0.1:{server.port}")
    assert isinstance(other, OpenOcdTelnet)
    other.close()


def test_gdb_read_halts_the_core_for_the_read_only(gdb_probe):
    server, probe = gdb_probe
    assert probe.read32(0x10) == 0x13121110
    wait_for(lambda: not server.halted)  # `c` has no reply: wait for the server
    assert server.log[-2:] == ["m10,4", "c"]  # halted for the read, then running


def test_gdb_reads_between_halt_and_resume_keep_the_core_stopped(gdb_probe):
    server, probe = gdb_probe
    probe.halt()
    probe.read32(0x20)
    probe.read32(0x24)
    assert server.halted
    probe.resume()
    wait_for(lambda: not server.halted)
    assert server.log.count("c") == 2  # one to halt, one to resume


def test_gdb_reads_split_into_packets_of_the_servers_size(gdb_probe):
    server, probe = gdb_probe
    assert probe.read_bytes(0x100, 200) == bytes(range(256))[:200]
    reads = [p for p in server.log if p.startswith("m")]
    assert len(reads) > 1
    assert all(int(p.split(",")[1], 16) <= (0x40 - 16) // 2 for p in reads)


def test_gdb_write_then_read_back(gdb_probe):
    _, probe = gdb_probe
    probe.write32(0x40, 0xDEADBEEF)
    assert probe.read32(0x40) == 0xDEADBEEF


def test_gdb_refused_read_gives_none_or_raises(gdb_probe):
    _, probe = gdb_probe
    assert probe.read_bytes(0x10_0000, 4) is None
    with pytest.raises(ProbeError, match="cannot read"):
        probe.read32(0x10_0000)


def test_gdb_halt_gives_up_when_the_core_never_stops(monkeypatch):
    monkeypatch.setattr(PemicroRsp, "HALT_SETTLE", 0)
    server = FakeGdbServer(bytes(16), ctrl_c_after_c=False)
    probe = open_probe(f"gdb:127.0.0.1:{server.port}")
    probe._sock.settimeout(0.2)  # pylint: disable=protected-access
    with pytest.raises(ProbeError, match="did not answer|did not stop"):
        probe.halt()
    probe.close()


def test_gdb_unreachable_server_raises():
    with pytest.raises(ProbeError, match="cannot connect"):
        open_probe("gdb:127.0.0.1:1")


def test_gdb_write_bytes_and_run_from(gdb_probe):
    server, probe = gdb_probe
    probe.write_bytes(0x80, bytes(range(16, 32)))
    assert probe.read_bytes(0x80, 16) == bytes(range(16, 32))
    probe.run_from(0x2000_1000, 0x2040_0008)
    wait_for(lambda: not server.halted)
    assert server.regs == {13: 0x2000_1000, 15: 0x2040_0008}
