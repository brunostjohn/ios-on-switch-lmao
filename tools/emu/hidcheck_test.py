"""hidcheck.py: the keys in a keyboard's pcap, each HID path's steps, and the judgement of a finished run."""

import struct
import tempfile
import unittest
from pathlib import Path

from tools.emu import hidcheck, usbcheck


def pcap(reports: list[bytes]) -> bytes:
    """Return a QEMU USB pcap of interrupt IN completions carrying `reports`, as usbcheck reads them."""
    out = b'\0' * usbcheck.PCAP_HEADER
    for data in reports:
        packet = usbcheck.USBMON.pack(
            0, ord('C'), usbcheck.INTERRUPT, 0x81, 1, 0, 0, 0, 0, 0, 0, len(data), len(data), 0, 0, 0, 0, 0
        )
        record = packet + data
        out += struct.pack('<IIII', 0, 0, len(record), len(record)) + record
    return out


def report(*usages: int) -> bytes:
    return bytes([0, 0, *usages, *([0] * (6 - len(usages)))])


class Keys(unittest.TestCase):
    def test_each_press_counts_once_however_long_it_is_held(self):
        with tempfile.TemporaryDirectory() as d:
            path = Path(d) / 'k.pcap'
            path.write_bytes(pcap([report(), report(0x0B), report(0x0B), report(), report(0x0C), report()]))
            self.assertEqual(hidcheck.keys_in(path), [0x0B, 0x0C])

    def test_no_pcap_is_no_keys(self):
        self.assertEqual(hidcheck.keys_in(Path('/nonexistent/k.pcap')), [])


class Judge(unittest.TestCase):
    LOG = (
        '[ 9.0] SwitchStall: SpringBoard[27] main thread',
        (
            '[12.0] AppleUSB20XHCITypeCPort@00500000: AppleUSBHostPort::enumerateDeviceComplete_block_invoke: '
            'enumerated 0x0627/0001/0000 (QEMU USB Keyboard / 1) at 480 Mbps'
        ),
        '[12.1] IOUSBHostHIDDevice:0x1000002f1 start',
        '[12.2] IOUSBHostHIDDevice:0x1000002f1 open by IOHIDEventDriver:0x1000002f5 (0x0)',
    )

    def judge(self, reports: list[bytes]) -> tuple[list[str], list[str]]:
        with tempfile.TemporaryDirectory() as d:
            (Path(d) / hidcheck.PCAP).write_bytes(pcap(reports))
            return hidcheck.Typing('ios17', base=1).judge(self.LOG, Path(d))

    def test_the_keys_sent_reported_in_order_pass(self):
        problems, notes = self.judge([report(0x0B), report(), report(0x0C), report()])
        self.assertEqual(problems, [])
        self.assertIn('path: kernel driver (IOUSBHostHIDDevice, IOHIDEventDriver)', notes)

    def test_a_missing_or_wrong_key_fails(self):
        problems, _ = self.judge([report(0x0B), report()])
        self.assertTrue(problems and 'reported' in problems[0])

    def test_the_dext_path_needs_each_of_its_steps(self):
        path = hidcheck.PATHS['ios27']
        lines = [
            'DK: AppleUserUSBHostHIDDevice-0x10000047d waiting for server '
            'com.apple.driverkit.AppleUserHIDDrivers-100000474',
            'DK: AppleUserUSBHostHIDDevice-0x10000047d server launched, validating',
            'DK: AppleUserUSBHostHIDDevice-0x10000047d::start(IOUSBHostInterface-0x10000047c) ok',
            'AppleUserUSBHostHIDDevice:0x10000047d open by AppleUserHIDEventDriver:0x10000048a (0x0)',
        ]
        self.assertTrue(all(hidcheck.first(lines, pattern) for _, pattern in path.steps))
        self.assertIsNone(hidcheck.first(lines[1:], path.steps[0][1]))
        self.assertIsNone(hidcheck.first(self.LOG, path.steps[-1][1]))


if __name__ == '__main__':
    unittest.main()
