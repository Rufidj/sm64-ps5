#!/usr/bin/env python3
"""asset_strip - takes the game's ROM-derived data out of a linked eboot ELF and
writes the map the console uses to put it back from the player's own ROM.

    asset_strip.py <elf> <baserom.us.z64> <assets.json> <stripped elf> <map> [<report.json>]

The build compiles what the decompilation extracted from the ROM straight into
.rodata and .data. Rather than change hundreds of sources, this works on the
finished ELF: it finds the ROM's bytes in the ELF, zeroes them, and records
where they go and where in the ROM they come from.

  - Textures and binary assets (assets.json .png and .bin), and the sequences
    (.m64): found whole, or - the skyboxes, which the build cuts into tiles - in
    blocks. Their source is a raw ROM offset or an offset into one of the ROM's
    MIO0-compressed segments.
  - The sample bank (@sound tbl): carried over by the build unchanged but for a
    header, so found in blocks and each match extended as far as it goes.
  - The instrument bank (@sound ctl): rewritten by the build little-endian with
    64-bit pointers, so most of it is the ROM's bytes reversed in pairs or in
    fours. It is matched as it is, byte-swapped in 16-bit units and in 32-bit
    units, at every alignment; the console reverses the bytes again as it
    copies. Pointers and counts, which match nothing, stay in the program.

What is zeroed is only ever put back byte for byte, so a match in the wrong
place would still restore correctly - with one exception: .data holds pointers
the loader relocates when the program starts, and restoring over one would put
back the unrelocated value. No claimed range overlaps a relocation; runs are
split around them. Tiny or near-uniform matches are not taken.

The console finds the base the program was loaded at through an anchor string
the program keeps in .rodata; its address is recorded in the map.

Before anything is written, restoring the stripped ELF from the ROM and the map
is checked to reproduce the original exactly.

Map format, little-endian:
    char[8]  "SM64AMAP"
    u32      version (2)
    u32      entry count
    u32      segment count
    u32      anchor address (0xFFFFFFFF if the program has no anchor)
    u32      .rodata address and u32 size, which the console makes writable
             while it restores and read-only again afterwards
    u8[20]   SHA-1 of the ROM the map was made against
    u32[]    ROM offset of each MIO0 segment used
    entries: u32 address, u32 length, u32 source, u32 offset
             source: a segment index - offset into its decompressed data -
             or, with offset into the ROM itself, 0xFFFFFFFF (copied as is),
             0xFFFFFFFE (bytes reversed in pairs), 0xFFFFFFFD (in fours)
"""
import bisect
import hashlib
import json
import struct
import sys

US_SHA1 = "9bef1128717f958171a4afac3ed78ee2bb4e86ce"
ANCHOR = b"SM64-ASSET-ANCHOR\0"
MAP_MAGIC = b"SM64AMAP"
MAP_VERSION = 2
RAW, RAW_SWAP16, RAW_SWAP32 = 0xFFFFFFFF, 0xFFFFFFFE, 0xFFFFFFFD
UNIT = {RAW: 1, RAW_SWAP16: 2, RAW_SWAP32: 4}
MIN_SIZE = 64
MIN_DISTINCT_BYTES = 8
BLOCK = 2048
SOUND_BLOCK = 1024
CTL_CHUNK = 16
MIN_RUN = 16
R_X86_64_64, R_X86_64_GLOB_DAT, R_X86_64_RELATIVE = 1, 6, 8
DT_RELA, DT_RELASZ = 7, 8


def mio0_decode(rom, at):
    if rom[at:at + 4] != b"MIO0":
        raise ValueError(f"no MIO0 header at {at:#x}")
    size, comp_at, raw_at = struct.unpack_from(">III", rom, at + 4)
    out = bytearray()
    layout, comp, raw = at + 16, at + comp_at, at + raw_at
    bits = nbits = 0
    while len(out) < size:
        if nbits == 0:
            bits = struct.unpack_from(">I", rom, layout)[0]
            layout += 4
            nbits = 32
        if bits & 0x80000000:
            out.append(rom[raw])
            raw += 1
        else:
            v = struct.unpack_from(">H", rom, comp)[0]
            comp += 2
            length, back = (v >> 12) + 3, (v & 0xFFF) + 1
            for _ in range(length):
                out.append(out[-back])
        bits = (bits << 1) & 0xFFFFFFFF
        nbits -= 1
    return bytes(out)


def swapped(buf, kind):
    """The bytes as `kind` reverses them, in whole units."""
    if kind == RAW:
        return bytes(buf)
    unit = UNIT[kind]
    n = len(buf) // unit * unit
    out = bytearray(n)
    if unit == 2:
        out[0::2], out[1::2] = buf[1:n:2], buf[0:n:2]
    else:
        out[0::4], out[1::4], out[2::4], out[3::4] = buf[3:n:4], buf[2:n:4], buf[1:n:4], buf[0:n:4]
    return bytes(out)


def read_sections(elf):
    shoff = struct.unpack_from("<Q", elf, 0x28)[0]
    entsize, count, strndx = struct.unpack_from("<HHH", elf, 0x3A)
    raw = [struct.unpack_from("<IIQQQQIIQQ", elf, shoff + i * entsize) for i in range(count)]
    names = raw[strndx][4]
    out = {}
    for name_at, _type, _flags, addr, offset, size, *_ in raw:
        name = elf[names + name_at:elf.index(b"\0", names + name_at)].decode()
        out[name] = (addr, offset, size)
    return out


def read_segments(elf):
    phoff = struct.unpack_from("<Q", elf, 0x20)[0]
    entsize, count = struct.unpack_from("<HH", elf, 0x36)
    segs = []
    for i in range(count):
        p_type, _flags, off, vaddr, _paddr, filesz, _memsz, _align = struct.unpack_from("<IIQQQQQQ", elf, phoff + i * entsize)
        if p_type == 1:
            segs.append((vaddr, off, filesz))
    return segs


def vaddr_to_offset(segs, vaddr):
    for base, off, size in segs:
        if base <= vaddr < base + size:
            return off + (vaddr - base)
    raise ValueError(f"address {vaddr:#x} is in no loadable segment")


def relocation_targets(elf, sections, segs):
    """Every address the loader writes a relocated value into."""
    _addr, off, size = sections[".dynamic"]
    rela = relasz = None
    for i in range(size // 16):
        tag, val = struct.unpack_from("<qQ", elf, off + 16 * i)
        if tag == DT_RELA:
            rela = val
        elif tag == DT_RELASZ:
            relasz = val
    targets = []
    if rela is not None and relasz:
        at = vaddr_to_offset(segs, rela)
        for i in range(relasz // 24):
            r_offset, r_info, _addend = struct.unpack_from("<QQq", elf, at + 24 * i)
            if r_info & 0xFFFFFFFF in (R_X86_64_RELATIVE, R_X86_64_64, R_X86_64_GLOB_DAT):
                targets.append(r_offset)
    return sorted(targets)


def main(argv):
    if len(argv) not in (6, 7):
        raise SystemExit(__doc__)
    elf_path, rom_path, assets_path, stripped_path, map_path = argv[1:6]
    report_path = argv[6] if len(argv) == 7 else None

    elf = open(elf_path, "rb").read()
    rom = open(rom_path, "rb").read()
    rom_sha1 = hashlib.sha1(rom).hexdigest()
    if rom_sha1 != US_SHA1:
        raise SystemExit(f"the ROM is not the US version the build uses (SHA-1 {rom_sha1})")
    assets = json.load(open(assets_path))

    sections = read_sections(elf)
    segs = read_segments(elf)
    parts = []                                   # (name, address, blob) of .rodata and .data
    for name in (".rodata", ".data"):
        addr, off, size = sections[name]
        parts.append((name, addr, elf[off:off + size]))
    relocated = relocation_targets(elf, sections, segs)

    claimed = []          # (address, length), sorted
    entries = []          # (address, length, source, offset); source a RAW kind or a segment's ROM offset
    segment_cache = {}
    stats = {"whole": 0, "duplicate": 0, "blocks": 0, "skipped_small": 0, "not_found": []}
    covered = {"images_and_bins": [0, 0], "sequences": [0, 0], "samples": [0, 0], "instruments": [0, 0]}

    def obstacles_in(addr, length):
        found = []
        i = bisect.bisect_left(relocated, addr - 7)
        while i < len(relocated) and relocated[i] < addr + length:
            found.append((relocated[i], 8))
            i += 1
        k = max(bisect.bisect_right(claimed, (addr, 1 << 62)) - 1, 0)
        while k < len(claimed) and claimed[k][0] < addr + length:
            ca, cl = claimed[k]
            if ca + cl > addr:
                found.append((ca, cl))
            k += 1
        return sorted(found)

    def claim_whole(data, source, offset):
        """Claims every occurrence of `data` that is wholly free. Returns how
        many it claimed and how many were claimed already."""
        hits = already = 0
        for _name, raddr, blob in parts:
            at = blob.find(data)
            while at != -1:
                addr = raddr + at
                obstacles = obstacles_in(addr, len(data))
                if not obstacles:
                    bisect.insort(claimed, (addr, len(data)))
                    entries.append((addr, len(data), source, offset))
                    hits += 1
                elif all(o in claimed for o in obstacles):
                    already += 1
                at = blob.find(data, at + 1)
        return hits, already

    def claim_span(addr, length, kind, offset):
        """Claims the parts of a matched span clear of relocations and of what
        is claimed already, each part whole units of `kind`."""
        unit = UNIT.get(kind, 1)
        got, pos = 0, addr
        for oa, ol in obstacles_in(addr, length) + [(addr + length, 0)]:
            start, end = pos, min(oa, addr + length)
            start += (-(start - addr)) % unit
            if end > start:
                end -= (end - start) % unit
            if end - start >= MIN_RUN:
                bisect.insort(claimed, (start, end - start))
                entries.append((start, end - start, kind, offset + (start - addr)))
                got += end - start
            pos = max(pos, oa + ol)
        return got

    def claim_runs(source, source_rom_offset, kind, chunk, step):
        """Scans `source` - ROM bytes as they appear in the program - for
        chunks the program contains, extends each match both ways, and claims
        it. Returns the bytes claimed."""
        unit = UNIT[kind]
        total, j = 0, 0
        while j + chunk <= len(source):
            piece = source[j:j + chunk]
            if len(set(piece)) < MIN_DISTINCT_BYTES:
                j += step
                continue
            hit = None
            for _name, raddr, blob in parts:
                at = blob.find(piece)
                if at != -1:
                    hit = (raddr, blob, at)
                    break
            if hit is None:
                j += step
                continue
            raddr, blob, at = hit
            s, d = j, at
            while s > 0 and d > 0 and source[s - 1] == blob[d - 1]:
                s -= 1
                d -= 1
            e, f = j + chunk, at + chunk
            while e < len(source) and f < len(blob) and source[e] == blob[f]:
                e += 1
                f += 1
            shift = (-s) % unit
            s += shift
            d += shift
            if e - s >= MIN_RUN:
                total += claim_span(raddr + d, (e - s) // unit * unit, kind, source_rom_offset + s)
            j = max(e, j + step)
        return total

    # Textures, binary assets and sequences.
    for path, entry in assets.items():
        if path.startswith("@") or not path.endswith((".png", ".bin", ".m64")):
            continue
        pos = entry[-1].get("us") if isinstance(entry[-1], dict) else None
        size = entry[-2]
        if pos is None or not isinstance(size, int) or not pos or isinstance(pos[0], str):
            continue
        bucket = covered["sequences" if path.endswith(".m64") else "images_and_bins"]
        if len(pos) == 1:
            source, offset, data = RAW, pos[0], rom[pos[0]:pos[0] + size]
        else:
            seg_at, offset = pos
            if seg_at not in segment_cache:
                segment_cache[seg_at] = mio0_decode(rom, seg_at)
            source, data = seg_at, segment_cache[seg_at][offset:offset + size]
        bucket[1] += len(data)
        if len(data) < MIN_SIZE or len(set(data)) < MIN_DISTINCT_BYTES:
            stats["skipped_small"] += 1
            continue
        hits, already = claim_whole(data, source, offset)
        if hits or already:
            stats["whole" if hits else "duplicate"] += 1
            bucket[0] += len(data)
            continue
        got = 0
        for b in range(0, len(data) - BLOCK + 1, BLOCK):
            block = data[b:b + BLOCK]
            if len(set(block)) >= MIN_DISTINCT_BYTES and sum(claim_whole(block, source, offset + b)):
                got += BLOCK
        if got:
            stats["blocks"] += 1
            bucket[0] += got
        else:
            stats["not_found"].append(path)

    # The sample bank, carried over as it is.
    tbl_size, tbl_pos = assets["@sound tbl us"][0], assets["@sound tbl us"][1]["us"][0]
    covered["samples"] = [claim_runs(rom[tbl_pos:tbl_pos + tbl_size], tbl_pos, RAW, SOUND_BLOCK, SOUND_BLOCK // 4), tbl_size]

    # The instrument bank, byte-swapped by the build in most places.
    ctl_size, ctl_pos = assets["@sound ctl us"][0], assets["@sound ctl us"][1]["us"][0]
    ctl = rom[ctl_pos:ctl_pos + ctl_size]
    got = 0
    for kind in (RAW_SWAP16, RAW_SWAP32, RAW):
        for align in range(UNIT[kind]):
            got += claim_runs(swapped(ctl[align:], kind), ctl_pos + align, kind, CTL_CHUNK, UNIT[kind] * 4)
    covered["instruments"] = [got, ctl_size]

    # Neighbouring entries from contiguous sources become one.
    entries.sort()
    merged = []
    for e in entries:
        if merged:
            a, l, s, o = merged[-1]
            if s == e[2] and a + l == e[0] and o + l == e[3]:
                merged[-1] = (a, l + e[1], s, o)
                continue
        merged.append(e)
    entries = merged

    seg_list = sorted({s for _a, _l, s, _o in entries if s not in UNIT})
    segment_index = {s: i for i, s in enumerate(seg_list)}

    def source_bytes(source, offset, length):
        if source in UNIT:
            return swapped(rom[offset:offset + length], source)
        return segment_cache[source][offset:offset + length]

    stripped = bytearray(elf)
    for addr, length, _s, _o in entries:
        off = vaddr_to_offset(segs, addr)
        stripped[off:off + length] = bytes(length)

    anchor = RAW
    rod_addr, rod_off, rod_size = sections[".rodata"]
    at = elf.find(ANCHOR, rod_off, rod_off + rod_size)
    if at != -1:
        if elf.find(ANCHOR, at + 1, rod_off + rod_size) != -1:
            raise SystemExit("the anchor string appears more than once in .rodata")
        anchor = rod_addr + (at - rod_off)

    restored = bytearray(stripped)
    for addr, length, source, offset in entries:
        off = vaddr_to_offset(segs, addr)
        restored[off:off + length] = source_bytes(source, offset, length)
    if bytes(restored) != elf:
        raise SystemExit("self-check failed: restoring the stripped ELF does not reproduce the original")

    with open(stripped_path, "wb") as f:
        f.write(stripped)
    with open(map_path, "wb") as f:
        f.write(MAP_MAGIC)
        f.write(struct.pack("<IIIIII", MAP_VERSION, len(entries), len(seg_list), anchor, rod_addr, rod_size))
        f.write(bytes.fromhex(rom_sha1))
        for s in seg_list:
            f.write(struct.pack("<I", s))
        for addr, length, source, offset in entries:
            f.write(struct.pack("<IIII", addr, length, source if source in UNIT else segment_index[source], offset))

    zeroed = sum(l for _a, l, _s, _o in entries)
    kinds = {RAW: 0, RAW_SWAP16: 0, RAW_SWAP32: 0}
    for _a, l, s, _o in entries:
        if s in kinds:
            kinds[s] += l
    print(f"entries {len(entries)}, segments {len(seg_list)}, bytes zeroed {zeroed:,} "
          f"(raw {kinds[RAW]:,}, swapped in pairs {kinds[RAW_SWAP16]:,}, in fours {kinds[RAW_SWAP32]:,})")
    print(f"textures and bins: whole {stats['whole']}, identical to one found earlier {stats['duplicate']}, "
          f"in blocks {stats['blocks']}, skipped as tiny or uniform {stats['skipped_small']}, not found {len(stats['not_found'])}")
    for name, (got, total) in covered.items():
        print(f"  {name:16s} {got:>10,} of {total:>10,} bytes ({100.0 * got / max(total, 1):.1f}%)")
    print(f"anchor: {'none' if anchor == RAW else hex(anchor)}")
    print("self-check passed: the stripped ELF restores to the original byte for byte")
    if report_path:
        json.dump({"stats": stats, "covered": covered, "entries": len(entries), "segments": seg_list,
                   "zeroed": zeroed}, open(report_path, "w"), indent=1)


if __name__ == "__main__":
    main(sys.argv)
