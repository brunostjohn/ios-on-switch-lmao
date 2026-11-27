"""The build's own merged_module, cdhash_list and trust_cache_bundle on sets of modules: switch_trustcache's oracle.

Each case is the module count (4 bytes, big-endian) and each module length-prefixed the same way, then for
merged_module, cdhash_list and trust_cache_bundle(merged) in turn a byte that is 0 when it returned the bytes that
follow (length-prefixed) and 1 when it refused.
"""

import random
import struct
import sys
from collections.abc import Callable
from pathlib import Path

from tools.package.trust import trust_cache_bundle
from tools.trustcache.trustcache import cdhash_list, merged_module

ENTRY_TYPES = ((2, 0, 0), (2, 0, 2), (1, 0, 0), (2, 1, 3), (4, 0, 2))


def _module(entries: list[bytes], uuid: bytes) -> bytes:
    return struct.pack('<I16sI', 2, uuid, len(entries)) + b''.join(entries)


def _cases() -> list[list[bytes]]:
    rng = random.Random(20261126)

    def entry(kind: tuple[int, int, int] | None = None) -> bytes:
        return rng.randbytes(20) + bytes(kind or rng.choice(ENTRY_TYPES)) + b'\0'

    def module(entries: list[bytes]) -> bytes:
        return _module(sorted(entries), rng.randbytes(16))

    pool = [entry() for _ in range(400)]
    static, cryptex, overlay = module(pool[:200]), module(pool[150:300]), module(pool[290:])
    conflicting = bytearray(pool[10])
    conflicting[21] ^= 1
    reserved = bytearray(pool[20])
    reserved[23] = 1
    version = bytearray(module(pool[:3]))
    version[0] = 1
    return [
        [],
        [module([])],
        [static],
        [static, cryptex, overlay],
        [overlay, cryptex, static],
        [static, module(pool[:50]), module([]), static],
        [static, module([bytes(conflicting)])],
        [static, module([bytes(reserved)])],
        [static, module([pool[1]] * 2)],
        [static, _module(sorted(pool[1:3], reverse=True), bytes(16))],
        [static, bytes(version)],
        [static, module(pool[:4])[:-1]],
        [b'\0' * 23],
    ]


def _result(out: bytearray, make: Callable[[], bytes]) -> bytes | None:
    try:
        made = make()
    except ValueError:
        out += b'\x01'
        return None
    out += b'\x00' + struct.pack('>I', len(made)) + made
    return made


def main() -> None:
    """Write every case to the file named by the first argument."""
    out = bytearray()
    for modules in _cases():
        out += struct.pack('>I', len(modules))
        for module in modules:
            out += struct.pack('>I', len(module)) + module
        merged = _result(out, lambda modules=modules: merged_module(modules))
        _result(out, lambda modules=modules: cdhash_list(modules))
        _result(out, lambda merged=merged: trust_cache_bundle(merged) if merged else merged_module([]))
    Path(sys.argv[1]).write_bytes(out)


if __name__ == '__main__':
    main()
