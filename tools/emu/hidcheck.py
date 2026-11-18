"""check.py's `typing` milestone: a keyboard hot-plugged once SpringBoard runs, its HID path, and the keys reaching it.

The run boots the kind's install image. At SpringBoard's first kernel line a QEMU keyboard goes onto the USB-C port
over QMP and the BM92T36's partner turns sink, as a cable plugged in; once the kind's HID path is up the keys go in with
`send-key` (the keyboard is the machine's only keyboard handler). It passes when Apple's stack enumerated the keyboard,
the HID path's steps all came, and the interrupt IN reports in the keyboard's pcap carry every key sent, in order.

iOS 17 takes the kernel driver: IOUSBHostHIDDevice starts and IOHIDEventDriver opens it. iOS 27 takes the HID dext:
the kernel asks driverkitd for AppleUserHIDDrivers ("DK: ... waiting for server"), the server launches, the dext's
service starts and the event driver opens it. The pcap is the device's side: it shows the reports going out to a host
that polled for them.
"""

import re
import time
from collections.abc import Sequence
from dataclasses import dataclass, field
from pathlib import Path

from tools.emu import usbcheck
from tools.emu.monitor import Monitor

SPRINGBOARD = re.compile(r'\bSpringBoard\[\d+\]')
ENUMERATED = re.compile(r'enumerated 0x0627/0001/\S+ \(QEMU USB Keyboard')
PCAP = 'hot0-keyboard.pcap'
DEVICE_ID = 'hot0'
KEYS = ('h', 'i')
USAGES = {'h': 0x0B, 'i': 0x0C}  # USB HID Usage Tables 1.4, Keyboard/Keypad page
HID_SECONDS = 300.0
SETTLE_SECONDS = 20.0


@dataclass(frozen=True)
class HidPath:
    name: str
    steps: tuple[tuple[str, re.Pattern], ...]


PATHS = {
    'ios17': HidPath(
        'kernel driver (IOUSBHostHIDDevice, IOHIDEventDriver)',
        (
            ('started', re.compile(r'IOUSBHostHIDDevice:0x[0-9a-f]+ start\b')),
            ('opened', re.compile(r'IOUSBHostHIDDevice:0x[0-9a-f]+ open by IOHIDEventDriver')),
        ),
    ),
    'ios27': HidPath(
        'HID dext (AppleUserHIDDrivers)',
        (
            (
                'requested',
                re.compile(r'AppleUserUSBHostHIDDevice-0x[0-9a-f]+ waiting for server '
                           r'com\.apple\.driverkit\.AppleUserHIDDrivers-'),
            ),
            ('launched', re.compile(r'AppleUserUSBHostHIDDevice-0x[0-9a-f]+ server launched, validating')),
            ('started', re.compile(r'AppleUserUSBHostHIDDevice-0x[0-9a-f]+::start\(.*\) ok')),
            ('opened', re.compile(r'AppleUserUSBHostHIDDevice:0x[0-9a-f]+ open by AppleUserHIDEventDriver:')),
        ),
    ),
}


def first(log: Sequence[str], pattern: re.Pattern, since: int = 0) -> str | None:
    return next((line for line in log[since:] if pattern.search(line)), None)


def keys_in(pcap: Path) -> list[int]:
    """Return the key usages the keyboard reported, each once per press: its interrupt IN completions in order."""
    pressed, held = [], set()
    for urb in usbcheck.urbs(pcap):
        if urb.event != 'C' or urb.transfer != usbcheck.INTERRUPT or not urb.endpoint & 0x80 or len(urb.data) < 8:
            continue
        down = {u for u in urb.data[2:8] if u}
        pressed += sorted(down - held)
        held = down
    return pressed


@dataclass
class Typing:
    """The milestone's watch: boot to SpringBoard, plug, wait for the HID path, type, settle."""

    kind: str
    phase: str = 'boot'
    base: int = 0
    since: float = 0.0
    failed: str | None = None
    marks: list[str] = field(default_factory=list)

    def step(self, log: Sequence[str], monitor: Monitor) -> bool:
        """Advance on this poll's log; return True when the run can stop."""
        if self.phase == 'boot' and first(log, SPRINGBOARD):
            self.plug(monitor)
            self.base, self.since, self.phase = len(log), time.monotonic(), 'hid'
            self.marks.append(f'SpringBoard: {first(log, SPRINGBOARD)[:120]}')
        elif self.phase == 'hid':
            path = PATHS[self.kind]
            if all(first(log, pattern, self.base) for _, pattern in path.steps):
                for key in KEYS:
                    monitor.command('send-key', keys=[{'type': 'qcode', 'data': key}], **{'hold-time': 100})
                self.since, self.phase = time.monotonic(), 'typed'
            elif time.monotonic() - self.since > HID_SECONDS:
                missing = [name for name, pattern in path.steps if not first(log, pattern, self.base)]
                self.failed = f'no {", ".join(missing)} step on the {path.name} path {HID_SECONDS:.0f} s after the plug'
                return True
        return self.phase == 'typed' and time.monotonic() - self.since > SETTLE_SECONDS

    @staticmethod
    def plug(monitor: Monitor) -> None:
        monitor.command('device_add', driver='usb-kbd', bus='xusb.0', port='1', id=DEVICE_ID, pcap=PCAP)
        monitor.command('qom-set', path=find_pd(monitor), property='partner', value='sink')

    def judge(self, log: Sequence[str], run: Path) -> tuple[list[str], list[str]]:
        """Return (problems, notes) for the finished run."""
        if self.failed:
            return [self.failed], self.marks
        path = PATHS[self.kind]
        notes = [*self.marks, f'path: {path.name}']
        problems = []
        enumerated = first(log, ENUMERATED, self.base)
        if not enumerated:
            problems.append('the hot-plugged keyboard never enumerated')
        for name, pattern in path.steps:
            line = first(log, pattern, self.base)
            notes.append(f'{name}: {line[:140] if line else "none"}')
        sent = [USAGES[key] for key in KEYS]
        got = keys_in(run / PCAP)
        notes.append(f'keys sent {[hex(u) for u in sent]}, reported {[hex(u) for u in got]}')
        if got != sent:
            problems.append(f'the keyboard reported {[hex(u) for u in got]} for {[hex(u) for u in sent]} sent')
        return problems, notes


def find_pd(monitor: Monitor, path: str = '/machine') -> str | None:
    """Return the QOM path of the BM92T36."""
    for child in monitor.command('qom-list', path=path):
        if child['type'] == 'child<bm92t36>':
            return f'{path}/{child["name"]}'
        if child['type'].startswith('child<'):
            found = find_pd(monitor, f'{path}/{child["name"]}')
            if found:
                return found
    return None
