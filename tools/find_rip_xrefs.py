#!/usr/bin/env python3
"""Find x64 RIP-relative references to a PE section-relative address.

This is an offline/read-only helper.  It parses only the PE headers and the
requested code section, then uses Capstone to verify candidate instructions.
"""

from __future__ import annotations

import argparse
import mmap
import struct
from pathlib import Path

import numpy as np
from capstone import CS_ARCH_X86, CS_MODE_64, Cs
from capstone.x86 import X86_OP_MEM, X86_REG_RIP


def pe_layout(path: Path) -> tuple[int, dict[str, tuple[int, int, int, int]]]:
    with path.open("rb") as fh:
        header = fh.read(0x1000)
        pe = struct.unpack_from("<I", header, 0x3C)[0]
        count = struct.unpack_from("<H", header, pe + 6)[0]
        optional_size = struct.unpack_from("<H", header, pe + 20)[0]
        optional = pe + 24
        if struct.unpack_from("<H", header, optional)[0] != 0x20B:
            raise ValueError("expected a PE32+ image")
        image_base = struct.unpack_from("<Q", header, optional + 24)[0]
        fh.seek(optional + optional_size)
        sections: dict[str, tuple[int, int, int, int]] = {}
        for _ in range(count):
            raw = fh.read(40)
            name = raw[:8].rstrip(b"\0").decode("ascii", "replace")
            virtual_size, virtual_address, raw_size, raw_pointer = struct.unpack_from(
                "<IIII", raw, 8
            )
            sections[name] = (virtual_address, virtual_size, raw_pointer, raw_size)
    return image_base, sections


def main() -> int:
    parser = argparse.ArgumentParser()
    parser.add_argument("image", type=Path)
    parser.add_argument("--target-section", default=".data")
    parser.add_argument("--target-offset", required=True, type=lambda x: int(x, 0))
    parser.add_argument("--code-section", default=".text")
    args = parser.parse_args()

    image_base, sections = pe_layout(args.image)
    code_rva, _, code_raw, code_size = sections[args.code_section]
    target_rva = sections[args.target_section][0] + args.target_offset
    target_va = image_base + target_rva
    code_va = image_base + code_rva

    candidates: set[int] = set()
    chunk_size = 16 * 1024 * 1024
    with args.image.open("rb") as fh, mmap.mmap(fh.fileno(), 0, access=mmap.ACCESS_READ) as mm:
        for chunk_start in range(0, code_size, chunk_size):
            chunk_end = min(code_size, chunk_start + chunk_size)
            raw = memoryview(mm)[code_raw + chunk_start : code_raw + chunk_end]
            # A disp32 can begin at any byte alignment.  For RIP-relative x64
            # instructions it is followed by 0..8 immediate/trailer bytes.
            for alignment in range(4):
                usable = (len(raw) - alignment) // 4 * 4
                if usable < 4:
                    continue
                values = np.frombuffer(raw[alignment : alignment + usable], dtype="<i4")
                offsets = chunk_start + alignment + np.arange(values.size, dtype=np.int64) * 4
                sums = values.astype(np.int64) + offsets
                for trailer in range(9):
                    wanted = target_va - code_va - 4 - trailer
                    for hit in np.flatnonzero(sums == wanted):
                        candidates.add(int(offsets[hit]))
                del values, offsets, sums
            raw.release()

        md = Cs(CS_ARCH_X86, CS_MODE_64)
        md.detail = True
        verified: dict[int, object] = {}
        for disp_offset in sorted(candidates):
            lo = max(0, disp_offset - 15)
            hi = min(code_size, disp_offset + 13)
            window = bytes(mm[code_raw + lo : code_raw + hi])
            for start in range(min(15, disp_offset - lo) + 1):
                address = code_va + lo + start
                for insn in md.disasm(window[start:], address, count=1):
                    for operand in insn.operands:
                        if operand.type != X86_OP_MEM or operand.mem.base != X86_REG_RIP:
                            continue
                        absolute = insn.address + insn.size + operand.mem.disp
                        if absolute == target_va:
                            verified[insn.address] = insn

    print(f"target VA=0x{target_va:X} RVA=0x{target_rva:X}")
    for address in sorted(verified):
        insn = verified[address]
        print(f"0x{address:X} RVA=0x{address-image_base:X}: {insn.mnemonic} {insn.op_str}")
    print(f"verified references: {len(verified)}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
