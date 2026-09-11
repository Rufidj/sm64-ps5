#!/usr/bin/env python3
"""hd_index - builds the index the console uses to replace the game's textures
with those of an sm64ex texture pack (SM64 Reloaded, say).

A pack for sm64ex names each image after the asset it replaces, the way the
decompilation extracts it: actors/amp/amp_body.rgba16.png, and the skyboxes as
tiles, textures/skybox_tiles/water.12.rgba16.png. The game on the console only
has the texture's bytes, at whatever address the build put them. So the index
files each texture under a hash of those bytes - FNV-1a, 64 bits, over the
texture as the game stores it - with its size and the image's path. The
console hashes a texture as it is imported, looks it up, and loads the image.

The bytes come from the ROM, where assets.json says each asset lies, and for
the skybox tiles from the build's own generated sources, which cut each skybox
into numbered 32x32 tiles the same way sm64ex does. The index itself holds only
hashes and paths, nothing of the game.

    hd_index.py <baserom.us.z64> <assets.json> <build bin dir> <index out> [<pack folder>]

With a pack folder, textures whose content is the same prefer a path the pack
has, and how much of the pack the index reaches is reported.

Index format, little-endian:
    char[8]  "SM64HDIX"
    u32      version (1)
    u32      entry count
    u32      size of the path table
    u32      reserved
    entries, sorted by hash: u64 hash, u32 texture size in bytes, u32 path offset
    path table: NUL-terminated paths relative to the pack's root
"""
import glob
import hashlib
import json
import os
import re
import struct
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "asset_tools"))
from asset_strip import US_SHA1, mio0_decode  # noqa: E402

MAGIC = b"SM64HDIX"
VERSION = 1


def fnv1a64(data):
    h = 0xCBF29CE484222325
    for b in data:
        h ^= b
        h = (h * 0x100000001B3) & 0xFFFFFFFFFFFFFFFF
    return h


def asset_textures(rom, assets):
    """(path, bytes) of every image assets.json places in the US ROM."""
    segments = {}
    for path, entry in assets.items():
        if not path.endswith(".png") or not isinstance(entry[-1], dict):
            continue
        pos, size = entry[-1].get("us"), entry[-2]
        if not pos or not isinstance(size, int) or isinstance(pos[0], str):
            continue
        if len(pos) == 1:
            yield path, rom[pos[0]:pos[0] + size]
        else:
            seg_at, offset = pos
            if seg_at not in segments:
                segments[seg_at] = mio0_decode(rom, seg_at)
            yield path, segments[seg_at][offset:offset + size]


def skybox_tiles(bin_dir):
    """(path, bytes) of every skybox tile in the build's generated sources. The
    array's hexadecimal suffix is the tile's position, which sm64ex writes in
    decimal in the tile's file name."""
    array = re.compile(r"static const Texture (\w+)_skybox_texture_([0-9A-Fa-f]+)\[\] = \{(.*?)\};", re.S)
    for source in sorted(glob.glob(os.path.join(bin_dir, "*_skybox.c"))):
        for sky, pos, body in array.findall(open(source).read()):
            data = bytes(int(x, 16) for x in re.findall(r"0x([0-9a-fA-F]+)", body))
            yield f"textures/skybox_tiles/{sky}.{int(pos, 16)}.rgba16.png", data


def main(argv):
    if len(argv) not in (5, 6):
        raise SystemExit(__doc__)
    rom_path, assets_path, bin_dir, out_path = argv[1:5]
    pack = argv[5] if len(argv) == 6 else None

    rom = open(rom_path, "rb").read()
    if hashlib.sha1(rom).hexdigest() != US_SHA1:
        raise SystemExit("the ROM is not the US version the build uses")
    assets = json.load(open(assets_path))

    def in_pack(path):
        return pack is not None and os.path.isfile(os.path.join(pack, path))

    chosen = {}      # (hash, size) -> path
    same_content = 0
    textures = list(asset_textures(rom, assets)) + list(skybox_tiles(bin_dir))
    for path, data in textures:
        if not data:
            continue
        key = (fnv1a64(data), len(data))
        if key not in chosen:
            chosen[key] = path
        else:
            same_content += 1
            if in_pack(path) and not in_pack(chosen[key]):
                chosen[key] = path

    entries = sorted(chosen.items())
    table = bytearray()
    records = bytearray()
    for (h, size), path in entries:
        records += struct.pack("<QII", h, size, len(table))
        table += path.encode() + b"\0"
    with open(out_path, "wb") as out:
        out.write(MAGIC + struct.pack("<IIII", VERSION, len(entries), len(table), 0))
        out.write(records)
        out.write(table)

    print(f"{out_path}: {len(entries)} textures ({same_content} more share their content), "
          f"{len(table)} bytes of paths")
    if pack is not None:
        reached = sum(1 for path in chosen.values() if in_pack(path))
        pack_images = sum(1 for _root, _dirs, files in os.walk(pack) for f in files if f.endswith(".png"))
        print(f"the pack has {pack_images} images; the index reaches {reached} of them")


if __name__ == "__main__":
    main(sys.argv)
