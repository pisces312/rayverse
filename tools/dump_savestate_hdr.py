#!/usr/bin/env python3
"""Dump the ss_file_hdr_t header of rayverse savestate slot files (rayverse_ss_*.bin).

Layout (src/savestate.c, all fixed-width, 88 bytes total):
    u32 magic          @0   SS_MAGIC 0x53535652
    u32 fmt            @4
    u32 stamp          @8
    u32 rec_count      @12
    s32 world          @16
    s32 level          @20
    u64 created        @24  seconds since epoch
    u64 payload_bytes  @32
    u64 payload_hash   @40
    u64 aud[5]         @48

Usage: python tools/dump_savestate_hdr.py <file> [<file>...]
"""
import struct
import sys
import time

HDR_SIZE = 88


def dump(path: str) -> None:
    with open(path, "rb") as f:
        raw = f.read(HDR_SIZE)
    if len(raw) < HDR_SIZE:
        print(f"{path}: too small ({len(raw)} bytes)")
        return
    magic, fmt, stamp, rec_count, world, level, created, payload_bytes, payload_hash = struct.unpack(
        "<IIIIiiQQQ", raw[:48])
    if magic != 0x53535652:
        print(f"{path}: bad magic 0x{magic:08x} (not a savestate slot?)")
        return
    ts = time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(created))
    print(f"{path}:")
    print(f"  fmt=v{fmt} rec_count={rec_count} stamp={stamp:08x}")
    print(f"  world={world} level={level} created={ts} ({created})")
    print(f"  payload_bytes={payload_bytes} payload_hash={payload_hash:016x}")


def main() -> int:
    if len(sys.argv) < 2:
        print(__doc__)
        return 1
    for path in sys.argv[1:]:
        dump(path)
    return 0


if __name__ == "__main__":
    sys.exit(main())
