#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""
Static FMOD API signature scout for forzahorizon6.exe.

Read-only. Faithfully replicates src/fmod/pe_image.cpp (PE parse + pdata
function-RVA recovery via unwind chains) and src/fmod/sig_scanner.cpp
(anchor-string search in .rdata + LEA rip-relative scan in .text + pdata
upper_bound function mapping) but operates on the on-disk file instead of a
loaded image. Produces, for every desired FMOD API anchor, the candidate
function RVAs and their first 32 prologue bytes -- the raw material for the
find_by_anchor byte patterns the v2 resolver will hardcode.

Cross-validates against the four runtime-known functions logged by the
existing build (System::createDSP / DSP::release / ChannelControl::addDSP /
ChannelControl::removeDSP): runtime_addr - static_RVA must yield the same
ASLR load base across all four, or the parser is wrong and we stop.
"""

import mmap
import re
import struct
import sys


def u16(d, o): return struct.unpack_from("<H", d, o)[0]
def u32(d, o): return struct.unpack_from("<I", d, o)[0]
def i32(d, o): return struct.unpack_from("<i", d, o)[0]


class Section:
    __slots__ = ("name", "vaddr", "vsize", "rawptr", "rawsize", "chars")
    def __init__(self, name, vaddr, vsize, rawptr, rawsize, chars):
        self.name = name; self.vaddr = vaddr; self.vsize = vsize
        self.rawptr = rawptr; self.rawsize = rawsize; self.chars = chars


class PE:
    def __init__(self, path):
        self.f = open(path, "rb")
        self.data = mmap.mmap(self.f.fileno(), 0, access=mmap.ACCESS_READ)
        d = self.data
        if d[:2] != b"MZ": raise RuntimeError("not MZ")
        e_lfanew = u32(d, 0x3C)
        if d[e_lfanew:e_lfanew+4] != b"PE\x00\x00": raise RuntimeError("not PE")
        fh = e_lfanew + 4
        num_sections = u16(d, fh + 2)
        size_opt_hdr = u16(d, fh + 16)
        opt = fh + 20
        if u16(d, opt) != 0x20B: raise RuntimeError("not PE32+")
        self.size_of_image = u32(d, opt + 56)
        num_rva = u32(d, opt + 108)
        dd = opt + 112
        # EXCEPTION directory = index 3
        self.exc_va = u32(d, dd + 3*8)
        self.exc_sz = u32(d, dd + 3*8 + 4)
        sec_off = opt + size_opt_hdr
        self.sections = []
        for i in range(num_sections):
            so = sec_off + i*40
            name = d[so:so+8].rstrip(b"\x00").decode("latin1")
            self.sections.append(Section(
                name, u32(d, so+12), u32(d, so+8),
                u32(d, so+20), u32(d, so+16), u32(d, so+36)))
        self.text = self._sec(".text")
        self.rdata = self._sec(".rdata")
        if not self.text or not self.rdata:
            raise RuntimeError(".text/.rdata missing")

    def _sec(self, nm):
        for s in self.sections:
            if s.name == nm: return s
        return None

    def va_to_off(self, va):
        for s in self.sections:
            if s.vaddr <= va < s.vaddr + max(s.vsize, s.rawsize):
                off = s.rawptr + (va - s.vaddr)
                if off < len(self.data):
                    return off
        return None

    def read(self, va, n):
        off = self.va_to_off(va)
        if off is None: return None
        return self.data[off:off+n]


def recover_function_rvas(pe):
    """Replicate pe_image.cpp: walk pdata RUNTIME_FUNCTION, follow unwind
    chain (UNW_FLAG_CHAININFO=0x04 in flags -> byte0 & 0x20), 16 hops max."""
    d = pe.data
    base_off = pe.va_to_off(pe.exc_va)
    if base_off is None:
        return []
    count = pe.exc_sz // 12
    rvas = []
    for i in range(count):
        o = base_off + i*12
        begin, end, unw = u32(d, o), u32(d, o+4), u32(d, o+8)
        for _ in range(16):
            if unw == 0 or unw + 4 > pe.size_of_image:
                break
            uoff = pe.va_to_off(unw)
            if uoff is None:
                break
            flags_byte = d[uoff]
            if (flags_byte & 0x20) == 0:  # no chained info
                break
            n_codes = d[uoff + 2]
            codes = (2 * n_codes + 3) & ~3
            chain_rva = unw + 4 + codes
            if chain_rva + 16 > pe.size_of_image:
                break
            coff = pe.va_to_off(chain_rva)
            if coff is None:
                break
            begin, end, unw = u32(d, coff), u32(d, coff+4), u32(d, coff+8)
        if begin:
            rvas.append(begin)
    return sorted(set(rvas))


def find_anchors(pe, name_bytes):
    """Replicate sig_scanner anchors(): null-terminated, preceded by null."""
    d = pe.data
    s = pe.rdata
    start = s.rawptr
    end = start + s.rawsize
    n = len(name_bytes)
    hits = []
    # Use regex for speed; require null terminator + preceding null.
    pat = re.compile(b"(?<![\x00-\xff])" + re.escape(name_bytes) + b"\x00")
    # Simpler + matches original semantics: scan for exact bytes, check neighbors.
    pos = start
    first = d.find(name_bytes, pos, end)
    while first != -1:
        if first + n < len(d) and d[first + n] == 0:
            if first == start or d[first - 1] == 0:
                hits.append(first - start + s.vaddr)  # VA of the string
        first = d.find(name_bytes, first + 1, end)
    return hits


# LEA r64,[rip+disp32]: REX.W (0x48..0x4F), 0x8D, modrm with mod=00 rm=05
# -> modrm in {05,0D,15,1D,25,2D,35,3D}; then int32 disp.
LEA_RE = re.compile(b"[\x48-\x4F]\x8D[\x05\x0D\x15\x1D\x25\x2D\x35\x3D]")


def find_leas_to(pe, target_vas):
    """Scan .text for LEA rip-relative whose target VA is in target_vas.
    Returns list of (lea_va, target_va)."""
    s = pe.text
    start = s.rawptr
    end = start + s.rawsize
    base_va = s.vaddr
    d = pe.data
    tgt = set(target_vas)
    out = []
    for m in LEA_RE.finditer(d, start, end):
        off = m.start()
        disp = i32(d, off + 3)
        lea_va = base_va + (off - start)
        target = lea_va + 7 + disp
        if target in tgt:
            out.append((lea_va, target))
    return out


def map_to_function(func_rvas, va):
    """Upper-bound: function containing va = last RVA <= va."""
    import bisect
    i = bisect.bisect_right(func_rvas, va)
    if i == 0:
        return None
    return func_rvas[i - 1]


def hexbytes(b, n=32):
    return " ".join("%02X" % c for c in b[:n])


# Desired FMOD API surface. Variant names cover the 5 anchors that grep
# showed absent in this build (different FMOD naming / older split).
DESIRED = {
    # anchor name : human label
    "System::get3DListenerAttributes": "listener attrs",
    "System::get3DNumListeners": "listener count (variant)",
    "System::getNumListeners": "listener count",
    "System::getMasterChannelGroup": "master group",
    "ChannelControl::get3DAttributes": "channel 3D attrs",
    "Channel::get3DAttributes": "channel 3D attrs (variant)",
    "ChannelControl::get3DPanLevel": "3D pan level",
    "ChannelControl::get3DLevel": "3D level",
    "ChannelControl::getMixMatrix": "mix matrix",
    "ChannelGroup::getMixMatrix": "mix matrix (variant)",
    "Channel::getMixMatrix": "mix matrix (variant)",
    "ChannelControl::getParentGroup": "parent group",
    "ChannelGroup::getParentGroup": "parent group (variant)",
    "Channel::getParentGroup": "parent group (variant)",
    "ChannelControl::getNumDSPs": "num DSPs",
    "ChannelGroup::getNumDSPs": "num DSPs (variant)",
    "ChannelControl::getDSP": "get DSP",
    "ChannelGroup::getDSP": "get DSP (variant)",
    "ChannelControl::getVolume": "volume",
    "ChannelControl::getAudibility": "audibility",
    "ChannelControl::getPitch": "pitch",
    "ChannelControl::isVirtual": "virtual",
    "DSP::getType": "DSP type",
    "DSP::getActive": "DSP active",
    "DSP::isActive": "DSP active (variant)",
    "DSP::getBypass": "DSP bypass",
    "DSP::getNumParameters": "DSP num params",
    "DSP::getParameterFloat": "DSP param float",
    "DSP::getMeteringInfo": "DSP metering",
    "DSP::getChannelFormat": "DSP channel format",
    # the 4 ground-truth anchors (cross-validation):
    "System::createDSP": "createDSP (GROUND TRUTH)",
    "DSP::release": "release (GROUND TRUTH)",
    "ChannelControl::addDSP": "addDSP (GROUND TRUTH)",
    "ChannelControl::removeDSP": "removeDSP (GROUND TRUTH)",
    # speculative Studio:
    "Studio::System::getCoreSystem": "studio core",
    "Studio::System::getBankCount": "studio bank count",
    "Studio::System::getParameterDescription": "studio param desc",
}

# Runtime ground-truth absolute addresses from the existing build's log
# (one ASLR run; load base will be derived).
GROUND_TRUTH = {
    "System::createDSP": 0x7FF6CAD31320,
    "DSP::release": 0x7FF6CAD355E0,
    "ChannelControl::addDSP": 0x7FF6CAD38C50,
    "ChannelControl::removeDSP": 0x7FF6CAD39B00,
}


def main():
    if len(sys.argv) < 2:
        print("usage: pe_static_scout.py <forzahorizon6.exe>")
        return 1
    pe = PE(sys.argv[1])
    print("== PE ==")
    print("SizeOfImage=0x%X" % pe.size_of_image)
    print("text: va=0x%X rawptr=0x%X rawsize=0x%X" %
          (pe.text.vaddr, pe.text.rawptr, pe.text.rawsize))
    print("rdata: va=0x%X rawptr=0x%X rawsize=0x%X" %
          (pe.rdata.vaddr, pe.rdata.rawptr, pe.rdata.rawsize))
    print("exc dir: va=0x%X size=0x%X" % (pe.exc_va, pe.exc_sz))

    funcs = recover_function_rvas(pe)
    print("recovered function RVAs: %d" % len(funcs))

    # ---- scout each desired anchor ----
    results = {}  # anchor -> [(rva, prologue_hex)]
    for anchor, label in DESIRED.items():
        ab = anchor.encode("ascii")
        str_vas = find_anchors(pe, ab)
        if not str_vas:
            results[anchor] = []
            continue
        leas = find_leas_to(pe, str_vas)
        cand = []
        seen = set()
        for lea_va, _tgt in leas:
            frva = map_to_function(funcs, lea_va)
            if frva is None or frva in seen:
                continue
            seen.add(frva)
            pre = pe.read(frva, 32) or b""
            cand.append((frva, hexbytes(pre)))
        results[anchor] = cand

    print("\n== SCOUT RESULTS ==")
    for anchor, label in DESIRED.items():
        cand = results[anchor]
        if not cand:
            print("[%s] %-44s NOT FOUND" % (label, anchor))
            continue
        print("[%s] %-44s -> %d candidate(s)" % (label, anchor, len(cand)))
        for i, (rva, pre) in enumerate(cand[:8]):
            print("    #%d rva=0x%08X prologue=[%s]" % (i, rva, pre))
        if len(cand) > 8:
            print("    ...%d more" % (len(cand) - 8))

    # ---- cross-validate ground truth ----
    print("\n== CROSS-VALIDATION (load base must be consistent) ==")
    bases = []
    ok = True
    for anchor, rt in GROUND_TRUTH.items():
        cand = results.get(anchor, [])
        if len(cand) != 1:
            print("  %-32s candidates=%d  CANNOT VALIDATE" % (anchor, len(cand)))
            ok = False
            continue
        rva = cand[0][0]
        base = rt - rva
        bases.append(base)
        print("  %-32s rva=0x%08X rt=0x%X -> base=0x%X" %
              (anchor, rva, rt, base))
    if ok and bases and len(set(bases)) == 1:
        print("  LOAD BASE CONFIRMED = 0x%X  -- static parser validated" % bases[0])
    else:
        print("  LOAD BASE MISMATCH %s -- parser wrong, do NOT trust results" %
              (["0x%X" % b for b in bases]))
    return 0


if __name__ == "__main__":
    sys.exit(main())
