#!/usr/bin/env python3
"""Summarise PCMAP/ALLFIX.DAT, the Rayman object/animation archive.

Reproduces LOAD_ALL_FIX() from src/load.c:537 so that a suspicious level
object or animation can be inspected without running the game.

Usage:
    python tools/parse_allfix.py <path/to/ALLFIX.DAT>
    python tools/parse_allfix.py <path> --eta 0            # dump every state table
    python tools/parse_allfix.py <path> --des 1 --anim     # dump one object's animations
"""

import argparse
import struct
import sys

ETA_FIELDS = ("speed_x_right", "speed_x_left", "anim_index", "next_main_etat",
              "next_sub_etat", "anim_speed", "sound_index", "flags")

SPECIALS = ("ray", "alpha", "alpha2", "alpha_numbers", "raylittle",
            "mapobj", "clockobj", "div_obj")


class Reader:
    def __init__(self, data):
        self.data = data
        self.pos = 0

    def take(self, n):
        if self.pos + n > len(self.data):
            raise ValueError(f"truncated at offset {self.pos}: wanted {n} bytes")
        chunk = self.data[self.pos:self.pos + n]
        self.pos += n
        return chunk

    def u8(self):
        return self.take(1)[0]

    def u16(self):
        return struct.unpack("<H", self.take(2))[0]

    def u32(self):
        return struct.unpack("<I", self.take(4))[0]

    def s32(self):
        return struct.unpack("<i", self.take(4))[0]


def parse(data):
    r = Reader(data)

    nb_loaded_eta = r.u8()
    etas = []
    for _ in range(nb_loaded_eta):
        groups = []
        for _ in range(r.u8()):
            entries = []
            for _ in range(r.u8()):
                entries.append(struct.unpack("<bBBBBBBB", r.take(8)))
            groups.append(entries)
        etas.append(groups)

    nb_des = r.u16()
    objs = {}
    for des in range(1, nb_des):
        o = {"index": des, "which_eta": r.s32()}
        r.u32()                                  # RaymanExeSize
        r.u32()                                  # RaymanExeCheckSum1
        atlas_len = r.u32()
        atlas = r.take(atlas_len)
        checksum = r.u8()
        for byte in atlas:                       # LOAD_ALL_FIX:590 - the byte must cancel out
            checksum = (checksum - byte) & 0xFF
        o["atlas"] = (atlas_len, checksum == 0)
        r.u32()                                  # RaymanExeCheckSum2
        o["nb_sprites"] = r.u16()
        r.take(12 * o["nb_sprites"])
        anims = []
        o["anim_count"] = anim_count = r.u8()
        for i in range(anim_count):
            a = {"anim_index": i, "layers_per_frame": r.u16(), "frames_count": r.u16()}
            frames_ptr = r.s32()
            a["has_frames"] = frames_ptr != -1
            a["layer_table_size"] = r.u16()
            r.take(a["layer_table_size"])
            if a["has_frames"]:
                r.take(4 * a["frames_count"])
            anims.append(a)
        o["anims"] = anims
        objs[des] = o

    r.u32()                                      # RaymanExeCheckSum3
    specials = {name: r.s32() for name in SPECIALS}
    return {"eta_cursor": None, "etas": etas, "objs": objs,
            "specials": specials, "end": r.pos, "size": len(data)}


def show_eta(archive, index):
    groups = archive["etas"][index]
    print(f"\neta table {index}: {len(groups)} main_etat groups")
    for main, group in enumerate(groups):
        print(f"  main_etat {main}: {len(group)} sub states")
        if not group:
            continue
        print("      " + " ".join(f"{f:>15}" for f in ETA_FIELDS))
        for sub, entry in enumerate(group):
            print(f"  [{main:2}][{sub:2}] " + " ".join(f"{v:>15}" for v in entry))


def show_anim(archive, index):
    obj = archive["objs"][index]
    print(f"\nobject {index}: which_eta={obj['which_eta']} sprites={obj['nb_sprites']} "
          f"animations={obj['anim_count']}")
    for a in obj["anims"]:
        print(f"  anim[{a['anim_index']:3}] layers/frame={a['layers_per_frame']:3} "
              f"frames={a['frames_count']:3} layer_table={a['layer_table_size']:4} "
              f"frames_inline={'yes' if a['has_frames'] else 'no'}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path")
    ap.add_argument("--eta", type=int, help="dump one eta table's state entries")
    ap.add_argument("--des", type=int, help="dump one object index")
    ap.add_argument("--anim", action="store_true", help="with --des: list its animations")
    args = ap.parse_args()

    with open(args.path, "rb") as fh:
        archive = parse(fh.read())

    if args.eta is not None:
        show_eta(archive, args.eta)
    if args.des is not None:
        if args.anim:
            show_anim(archive, args.des)
        else:
            o = archive["objs"][args.des]
            print(f"object {args.des}: which_eta={o['which_eta']} atlas={o['atlas'][0]} "
                  f"integrity={'ok' if o['atlas'][1] else 'BAD'} sprites={o['nb_sprites']} "
                  f"animations={o['anim_count']}")
    if args.eta is None and args.des is None:
        bad = [i for i, o in archive["objs"].items() if not o["atlas"][1]]
        with_anim = [i for i, o in archive["objs"].items() if o["anim_count"] > 0]
        print(f"{args.path}: {archive['size']} bytes, consumed {archive['end']}")
        print(f"  eta tables={len(archive['etas'])}  objects={len(archive['objs'])}")
        print(f"  objects with animations ({len(with_anim)}): {with_anim}")
        print(f"  atlas integrity failures: {bad or 'none'}")
        print("  specials:")
        for name, index in archive["specials"].items():
            o = archive["objs"].get(index)
            detail = f"eta={o['which_eta']} anims={o['anim_count']}" if o else "missing"
            print(f"    {name:<13} -> object {index:3}  {detail}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
