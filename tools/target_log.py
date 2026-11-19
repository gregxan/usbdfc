"""Parse the test target's UART text protocol (test_target/src/main.c).

Lines:
  H <cyc_hz> <mode> <variant>    header
  S <cyc>                        SOF
  O <cyc> <hex>                  one bulk OUT packet received from the XIAO
  M <cyc> <level>                marker edge (XIAO D3, R trigger)
  E <cyc> <text>                 event
<cyc> is a 32-bit DWT cycle count (8 hex digits) that wraps every 2^32
cycles. Lines are not strictly in time order (packets and other events are
printed from different queues), so each count is unwrapped as a signed
offset from the newest time seen so far.
"""
from dataclasses import dataclass
from typing import Iterable, Iterator, Optional

DEFAULT_HZ = 150_000_000


@dataclass
class Event:
    kind: str           # 'H', 'S', 'O', 'M', 'E'
    t: float            # seconds since the first timestamped line
    data: bytes = b""   # 'O': packet bytes
    level: int = 0      # 'M': marker level
    text: str = ""      # 'E': text; 'H': mode and variant


class Unwrapper:
    def __init__(self):
        self.hz = DEFAULT_HZ
        self.ref_c: Optional[int] = None
        self.ref_ticks = 0

    def ticks(self, c: int) -> int:
        if self.ref_c is None:
            self.ref_c = c
            return 0
        d = (c - self.ref_c) & 0xFFFFFFFF
        if d >= 0x80000000:          # earlier than the reference: out of order
            return self.ref_ticks - (0x100000000 - d)
        self.ref_ticks += d
        self.ref_c = c
        return self.ref_ticks

    def seconds(self, c: int) -> float:
        return self.ticks(c) / self.hz


class LineParser:
    """Stateful: feed lines in the order received."""

    def __init__(self):
        self.u = Unwrapper()

    def parse(self, raw: str) -> Optional[Event]:
        parts = raw.strip().split(" ", 2)
        if not parts or not parts[0]:
            return None
        kind = parts[0]
        try:
            if kind == "H":
                self.u.hz = int(parts[1])
                return Event("H", 0.0, text=parts[2] if len(parts) > 2 else "")
            if kind in ("S", "O", "M", "E") and len(parts) >= 2:
                t = self.u.seconds(int(parts[1], 16))
                rest = parts[2] if len(parts) > 2 else ""
                if kind == "O":
                    return Event("O", t, data=bytes.fromhex(rest))
                if kind == "M":
                    return Event("M", t, level=int(rest or "0"))
                if kind == "E":
                    return Event("E", t, text=rest)
                return Event("S", t)
        except ValueError:
            pass  # malformed line (e.g. truncated at capture start)
        return None


def parse_lines(lines: Iterable[str]) -> Iterator[Event]:
    p = LineParser()
    for raw in lines:
        ev = p.parse(raw)
        if ev is not None:
            yield ev
