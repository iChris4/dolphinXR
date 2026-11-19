#!/usr/bin/env python3
"""Generate module_tables.inc for a dolphin-chassis static recompilation module.

Parses the generated dispatcher's coverage ranges out of generated.h. Both
the original per-chunk address guards and DolRecomp's compact offset-table
dispatch runs are supported. SMC candidate ranges come from generated_smc.txt,
so the module ABI tables can never drift from a DolRecomp regen.

A module may hold several DolRecomp trees: main.dol's, and (Wii games) a REL's,
recompiled at the address the game links it to. Their ranges and chunk hashes
are merged, and the REL's rel_layout.txt becomes the descriptor the chassis uses
to find the REL in the game's RAM.

Usage:
  gen_module_tables.py <generated.h> <generated_smc.txt> <main.dol> <out.inc>
  gen_module_tables.py --out <out.inc> --tree <generated.h> <smc.txt|-> <image.dol>
                       [--tree ...] [--rel-layout <rel_layout.txt>]
"""
import argparse
import re
import sys
from pathlib import Path


FNV64_OFFSET = 0xCBF29CE484222325
FNV64_PRIME = 0x100000001B3
MASK64 = (1 << 64) - 1


def fnv1a64(data: bytes) -> int:
    h = FNV64_OFFSET
    for b in data:
        h = ((h ^ b) * FNV64_PRIME) & MASK64
    return h


def load_dol_text(dol_path: Path):
    """Return a function mapping a guest address range to the DOL's bytes."""
    dol = dol_path.read_bytes()

    def be32(off: int) -> int:
        return int.from_bytes(dol[off : off + 4], "big")

    sections = []
    for i in range(18):  # 7 text + 11 data sections share the layout
        file_off = be32(0x00 + i * 4)
        address = be32(0x48 + i * 4)
        size = be32(0x90 + i * 4)
        if file_off and address and size:
            sections.append((address, size, file_off))

    def read_range(start: int, end: int) -> bytes:
        for address, size, file_off in sections:
            if address <= start and end <= address + size:
                lo = file_off + (start - address)
                return dol[lo : lo + (end - start)]
        raise ValueError(f"range [0x{start:08X},0x{end:08X}) not inside one DOL section")

    return read_range


def merge_ranges(ranges):
    """Overlapping or touching ranges as one (the plain list and the conditions overlap)."""
    merged = []
    for a, b in sorted(ranges):
        if merged and a <= merged[-1][1]:
            merged[-1] = (merged[-1][0], max(merged[-1][1], b))
        else:
            merged.append((a, b))
    return merged


def parse_tree(generated_h: Path, smc_txt, image: Path):
    """One DolRecomp output: its code ranges, chunks with hashes, and SMC ranges."""
    header = generated_h.read_text()
    code_ranges = {
        (int(a, 16), int(b, 16))
        for a, b in re.findall(
            r"address >= (0x[0-9A-Fa-f]+)u && address < (0x[0-9A-Fa-f]+)u", header
        )
    }
    for base, span in re.findall(
        r"u32\s+offset\s*=\s*address\s*-\s*(0x[0-9A-Fa-f]+)u\s*;\s*"
        r"if\s*\(\s*offset\s*<\s*(0x[0-9A-Fa-f]+)u",
        header,
    ):
        start = int(base, 16)
        code_ranges.add((start, start + int(span, 16)))
    # DolphinXR: DolRecomp also lists the covered code plainly (needed with --function-chunks,
    # whose indexed lookup has no per-range conditions).
    for a, b in re.findall(r"// DOLRECOMP_CODE_RANGE (0x[0-9A-Fa-f]+) (0x[0-9A-Fa-f]+)", header):
        code_ranges.add((int(a, 16), int(b, 16)))
    code_ranges = merge_ranges(code_ranges)
    if not code_ranges:
        raise ValueError(f"no coverage ranges found in {generated_h}")
    code_ranges = sorted(code_ranges)

    # Chunk table: one range per generated func_XXXXXXXX translation unit.
    # A chunk is the demotion granule for the chassis SMC guard: an icache
    # invalidation inside a chunk retires exactly that chunk to the
    # interpreter for the session.
    func_addrs = sorted(
        int(a, 16)
        for a in re.findall(r"void func_([0-9A-Fa-f]{8})\(CPUState\* ctx\);", header)
    )
    if not func_addrs:
        raise ValueError(f"no func_ declarations found in {generated_h}")
    chunk_ranges = []
    for i, addr in enumerate(func_addrs):
        containing = next(((a, b) for a, b in code_ranges if a <= addr < b), None)
        if containing is None:
            raise ValueError(f"func_{addr:08X} outside all code ranges")
        end = containing[1]
        if i + 1 < len(func_addrs) and containing[0] <= func_addrs[i + 1] < containing[1]:
            end = func_addrs[i + 1]
        chunk_ranges.append((addr, end))

    smc_ranges = []
    if smc_txt is not None:
        for line in smc_txt.read_text().splitlines():
            m = re.match(r"(0x[0-9A-Fa-f]+)-(0x[0-9A-Fa-f]+)", line.strip())
            if m:
                # File ranges are inclusive instruction addresses; ABI ranges are
                # end-exclusive byte ranges.
                smc_ranges.append((int(m.group(1), 16), int(m.group(2), 16) + 4))

    # FNV-1a 64 of each chunk's original text, so the chassis can verify
    # that guest RAM still holds the code this module was compiled from.
    read_range = load_dol_text(image)
    hashes = [fnv1a64(read_range(a, b)) for a, b in chunk_ranges]
    return code_ranges, chunk_ranges, hashes, smc_ranges


def parse_rel_layout(path: Path):
    """rel_layout.txt (Tools/StaticRecomp/patches/dolrecomp-wii-rel.patch) -> descriptor fields."""
    fields = {}
    sections = []
    for line in path.read_text().splitlines():
        words = line.split()
        if not words:
            continue
        if words[0] == "section":
            sections.append((int(words[1]), int(words[2], 16), int(words[3], 16), words[4]))
        else:
            fields[words[0]] = int(words[1], 0)
    # Only the code is listed: the chassis maps it from where the game loaded the REL to where it
    # was recompiled, and finds the REL in RAM only when every listed section matches.
    code = [(index, start, size) for index, start, size, kind in sections if kind == "exec"]
    if not code:
        raise ValueError(f"{path}: no executable section")
    return fields, code


def overlapping(ranges):
    return any(a[1] > b[0] for a, b in zip(ranges, ranges[1:]))


def main() -> int:
    if len(sys.argv) == 5 and not sys.argv[1].startswith("--"):
        trees = [(Path(sys.argv[1]), Path(sys.argv[2]), Path(sys.argv[3]))]
        out_path = Path(sys.argv[4])
        rel_layouts = []
    else:
        parser = argparse.ArgumentParser()
        parser.add_argument("--out", required=True, type=Path)
        parser.add_argument("--tree", nargs=3, action="append", required=True,
                            metavar=("GENERATED_H", "SMC_TXT", "IMAGE_DOL"))
        parser.add_argument("--rel-layout", action="append", default=[], type=Path)
        args = parser.parse_args()
        trees = [(Path(h), None if smc == "-" else Path(smc), Path(image))
                 for h, smc, image in args.tree]
        out_path = args.out
        rel_layouts = args.rel_layout

    code_ranges, chunk_ranges, chunk_hashes, smc_ranges = [], [], [], []
    try:
        for generated_h, smc_txt, image in trees:
            code, chunks, hashes, smc = parse_tree(generated_h, smc_txt, image)
            code_ranges += code
            chunk_ranges += list(zip(chunks, hashes))
            smc_ranges += smc
        rels = [parse_rel_layout(path) for path in rel_layouts]
    except ValueError as error:
        print("error:", error, file=sys.stderr)
        return 1
    code_ranges.sort()
    chunk_ranges.sort()
    smc_ranges.sort()
    if overlapping(code_ranges) or overlapping([c for c, _ in chunk_ranges]):
        print("error: the trees' code ranges overlap", file=sys.stderr)
        return 1

    with out_path.open("w") as f:
        f.write("// Generated by gen_module_tables.py — do not edit.\n")
        f.write("static const StaticRecompRange s_code_ranges[] = {\n")
        for a, b in code_ranges:
            f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        f.write("};\n")
        f.write(f"#define MODULE_CODE_RANGE_COUNT {len(code_ranges)}u\n")
        f.write("static const StaticRecompRange s_smc_ranges[] = {\n")
        if smc_ranges:
            for a, b in smc_ranges:
                f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        else:
            f.write("    {0u, 0u}, /* C and MSVC do not support zero-sized arrays. */\n")
        f.write("};\n")
        f.write(f"#define MODULE_SMC_RANGE_COUNT {len(smc_ranges)}u\n")
        f.write("static const StaticRecompRange s_chunk_ranges[] = {\n")
        for (a, b), _ in chunk_ranges:
            f.write(f"    {{0x{a:08X}u, 0x{b:08X}u}},\n")
        f.write("};\n")
        f.write(f"#define MODULE_CHUNK_RANGE_COUNT {len(chunk_ranges)}u\n")
        f.write("static const u64 s_chunk_hashes[] = {\n")
        for _, h in chunk_ranges:
            f.write(f"    0x{h:016X}u,\n")
        f.write("};\n")
        # DolphinXR: each chunk's function, in the same order (indirect calls, chunk_ready_at).
        for (a, _), _ in chunk_ranges:
            f.write(f"void func_{a:08X}(CPUState* ctx);\n")
        f.write("static void (*const s_chunk_funcs[])(CPUState*) = {\n")
        for (a, _), _ in chunk_ranges:
            f.write(f"    func_{a:08X},\n")
        f.write("};\n")

        # REL modules: where each was recompiled, and the header fields that identify it in RAM.
        if rels:
            f.write("static const StaticRecompRelSection s_rel_sections[] = {\n")
            for fields, code in rels:
                for index, start, size in code:
                    f.write(f"    {{{fields['module_id']}u, {index}u, 0x{start:08X}u, 0x{size:X}u}},\n")
            f.write("};\n")
            f.write("static const StaticRecompRelModule s_rel_modules[] = {\n")
            first = 0
            for fields, code in rels:
                f.write(f"    {{{fields['module_id']}u, {fields['version']}u, "
                        f"{fields['section_count']}u, 0x{fields['section_info_offset']:X}u, "
                        f"0x{fields['file_size']:X}u, &s_rel_sections[{first}], {len(code)}u}},\n")
                first += len(code)
            f.write("};\n")
            f.write("#define MODULE_REL_MODULES s_rel_modules\n")
            f.write(f"#define MODULE_REL_MODULE_COUNT {len(rels)}u\n")
            # The REL tree's code, which module_export.c hands to the REL dispatcher.
            rel_starts = [start for _, code in rels for _, start, _ in code]
            rel_ends = [start + size for _, code in rels for _, start, size in code]
            f.write(f"#define MODULE_REL_CODE_START 0x{min(rel_starts):08X}u\n")
            f.write(f"#define MODULE_REL_CODE_END 0x{max(rel_ends):08X}u\n")
        else:
            f.write("#define MODULE_REL_MODULES ((const StaticRecompRelModule*)0)\n")
            f.write("#define MODULE_REL_MODULE_COUNT 0u\n")
    print(
        f"module_tables.inc: {len(code_ranges)} code ranges, "
        f"{len(smc_ranges)} smc ranges, {len(chunk_ranges)} chunk ranges (hashed), "
        f"{len(rels)} REL module(s)"
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
