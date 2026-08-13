"""Read-only FMOD anchor/call provenance scout for a running FH6 process.

Usage:
    python -X utf8 tools/live_fmod_provenance.py --pid <pid> [anchors...]

The game decrypts much of .text at runtime, so this intentionally reads the
live image.  It never writes process memory and never injects code.
"""

from __future__ import annotations

import argparse
import ctypes
import re
import struct
from bisect import bisect_right
from ctypes import wintypes

from capstone import CS_ARCH_X86, CS_MODE_64, Cs


PROCESS_QUERY_INFORMATION = 0x0400
PROCESS_VM_READ = 0x0010
TH32CS_SNAPMODULE = 0x00000008
TH32CS_SNAPMODULE32 = 0x00000010
INVALID_HANDLE_VALUE = ctypes.c_void_p(-1).value


class MODULEENTRY32W(ctypes.Structure):
    _fields_ = [
        ("dwSize", wintypes.DWORD),
        ("th32ModuleID", wintypes.DWORD),
        ("th32ProcessID", wintypes.DWORD),
        ("GlblcntUsage", wintypes.DWORD),
        ("ProccntUsage", wintypes.DWORD),
        ("modBaseAddr", ctypes.POINTER(ctypes.c_byte)),
        ("modBaseSize", wintypes.DWORD),
        ("hModule", wintypes.HMODULE),
        ("szModule", wintypes.WCHAR * 256),
        ("szExePath", wintypes.WCHAR * 260),
    ]


kernel32 = ctypes.WinDLL("kernel32", use_last_error=True)
kernel32.OpenProcess.restype = wintypes.HANDLE
kernel32.CreateToolhelp32Snapshot.restype = wintypes.HANDLE
kernel32.Module32FirstW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
kernel32.Module32NextW.argtypes = [wintypes.HANDLE, ctypes.POINTER(MODULEENTRY32W)]
kernel32.ReadProcessMemory.argtypes = [
    wintypes.HANDLE,
    wintypes.LPCVOID,
    wintypes.LPVOID,
    ctypes.c_size_t,
    ctypes.POINTER(ctypes.c_size_t),
]


def main_module(pid: int) -> tuple[int, int, str]:
    snap = kernel32.CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid)
    if snap == INVALID_HANDLE_VALUE:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        entry = MODULEENTRY32W()
        entry.dwSize = ctypes.sizeof(entry)
        if not kernel32.Module32FirstW(snap, ctypes.byref(entry)):
            raise ctypes.WinError(ctypes.get_last_error())
        return ctypes.addressof(entry.modBaseAddr.contents), entry.modBaseSize, entry.szExePath
    finally:
        kernel32.CloseHandle(snap)


def read_exact(process: int, address: int, size: int) -> bytes:
    buf = ctypes.create_string_buffer(size)
    got = ctypes.c_size_t()
    if not kernel32.ReadProcessMemory(process, address, buf, size, ctypes.byref(got)):
        raise ctypes.WinError(ctypes.get_last_error())
    if got.value != size:
        raise RuntimeError(f"short read at 0x{address:X}: {got.value}/{size}")
    return buf.raw


def sections(image: bytes) -> dict[str, tuple[int, int]]:
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    count = struct.unpack_from("<H", image, pe + 6)[0]
    optional_size = struct.unpack_from("<H", image, pe + 20)[0]
    table = pe + 24 + optional_size
    out: dict[str, tuple[int, int]] = {}
    for i in range(count):
        off = table + i * 40
        name = image[off : off + 8].split(b"\0", 1)[0].decode("ascii", "replace")
        virtual_size, rva = struct.unpack_from("<II", image, off + 8)
        out[name] = (rva, virtual_size)
    return out


def function_rvas(image: bytes) -> list[int]:
    pe = struct.unpack_from("<I", image, 0x3C)[0]
    optional = pe + 24
    magic = struct.unpack_from("<H", image, optional)[0]
    directories = optional + (112 if magic == 0x20B else 96)
    pdata_rva, pdata_size = struct.unpack_from("<II", image, directories + 3 * 8)
    starts = []
    for off in range(pdata_rva, pdata_rva + pdata_size, 12):
        if off + 12 > len(image):
            break
        begin, end, _ = struct.unpack_from("<III", image, off)
        if begin and end > begin:
            starts.append(begin)
    return sorted(set(starts))


def containing_function(starts: list[int], rva: int) -> int | None:
    i = bisect_right(starts, rva) - 1
    return starts[i] if i >= 0 else None


def lea_xrefs(image: bytes, text_rva: int, text_size: int, target_rva: int) -> list[int]:
    out = []
    end = min(text_rva + text_size, len(image))
    for rva in range(text_rva, end - 7):
        b0, b1, b2 = image[rva], image[rva + 1], image[rva + 2]
        if b0 & 0xF8 != 0x48 or b1 != 0x8D or b2 & 0xC7 != 0x05:
            continue
        disp = struct.unpack_from("<i", image, rva + 3)[0]
        if rva + 7 + disp == target_rva:
            out.append(rva)
    return out


def direct_calls(image: bytes, text_rva: int, text_size: int, fn_rva: int) -> list[int]:
    out = []
    end = min(text_rva + text_size, len(image))
    for rva in range(text_rva, end - 5):
        if image[rva] != 0xE8:
            continue
        disp = struct.unpack_from("<i", image, rva + 1)[0]
        if rva + 5 + disp == fn_rva:
            out.append(rva)
    return out


def disassemble(md: Cs, image: bytes, base: int, fn_rva: int, size: int = 96) -> str:
    code = image[fn_rva : fn_rva + size]
    rows = []
    for insn in md.disasm(code, base + fn_rva):
        rows.append(f"    {insn.address - base:08X}: {insn.mnemonic:8} {insn.op_str}")
        if insn.mnemonic == "ret":
            break
    return "\n".join(rows)


def call_context(md: Cs, image: bytes, base: int, starts: list[int], call_rva: int) -> str:
    caller = containing_function(starts, call_rva)
    if caller is None:
        return ""
    instructions = list(md.disasm(image[caller : call_rva + 8], base + caller))
    rows = []
    for insn in instructions[-24:]:
        marker = " => " if insn.address - base == call_rva else "    "
        rows.append(f"{marker}{insn.address - base:08X}: {insn.mnemonic:8} {insn.op_str}")
    return "\n".join(rows)


def run(pid: int, anchors: list[str], dump_rvas: list[int]) -> None:
    base, size, path = main_module(pid)
    process = kernel32.OpenProcess(PROCESS_QUERY_INFORMATION | PROCESS_VM_READ, False, pid)
    if not process:
        raise ctypes.WinError(ctypes.get_last_error())
    try:
        image = read_exact(process, base, size)
    finally:
        kernel32.CloseHandle(process)

    sec = sections(image)
    text_rva, text_size = sec[".text"]
    starts = function_rvas(image)
    md = Cs(CS_ARCH_X86, CS_MODE_64)
    print(f"pid={pid} base=0x{base:X} size=0x{size:X} path={path}")

    for requested_rva in dump_rvas:
        fn = containing_function(starts, requested_rva)
        if fn is None:
            print(f"\n[dump 0x{requested_rva:X}] no containing pdata function")
            continue
        print(f"\n[dump 0x{requested_rva:X}] function_rva=0x{fn:X}")
        print(disassemble(md, image, base, fn, 768))

    string_rvas_by_anchor: dict[str, list[int]] = {}
    anchor_by_string_rva: dict[int, set[str]] = {}
    for anchor in anchors:
        needle = anchor.encode() + b"\0"
        string_rvas = []
        pos = 0
        while True:
            pos = image.find(needle, pos)
            if pos < 0:
                break
            string_rvas.append(pos)
            pos += 1
        string_rvas_by_anchor[anchor] = string_rvas
        for rva in string_rvas:
            anchor_by_string_rva.setdefault(rva, set()).add(anchor)

    # The image is hundreds of MiB. Scan .text once for every requested anchor
    # instead of once per anchor, then scan calls once for all candidate funcs.
    refs_by_anchor: dict[str, list[int]] = {anchor: [] for anchor in anchors}
    text_end = min(text_rva + text_size, len(image))
    live_text = image[text_rva:text_end]
    for match in re.finditer(rb"[\x48-\x4f]\x8d.", live_text):
        rva = text_rva + match.start()
        b0, b1, b2 = image[rva], image[rva + 1], image[rva + 2]
        if b2 & 0xC7 != 0x05:
            continue
        disp = struct.unpack_from("<i", image, rva + 3)[0]
        for anchor in anchor_by_string_rva.get(rva + 7 + disp, ()):
            refs_by_anchor[anchor].append(rva)

    functions_by_anchor: dict[str, set[int]] = {anchor: set() for anchor in anchors}
    all_functions: set[int] = set()
    for anchor, refs in refs_by_anchor.items():
        for ref in refs:
            fn = containing_function(starts, ref)
            if fn is not None:
                functions_by_anchor[anchor].add(fn)
                all_functions.add(fn)

    calls_by_function: dict[int, list[int]] = {fn: [] for fn in all_functions}
    pos = 0
    while True:
        pos = live_text.find(b"\xE8", pos)
        if pos < 0 or pos + 5 > len(live_text):
            break
        rva = text_rva + pos
        disp = struct.unpack_from("<i", image, rva + 1)[0]
        target = rva + 5 + disp
        if target in calls_by_function:
            calls_by_function[target].append(rva)
        pos += 1

    for anchor in anchors:
        string_rvas = string_rvas_by_anchor[anchor]
        print(f"\n[{anchor}] strings={len(string_rvas)}")
        for string_rva in string_rvas:
            refs = [r for r in refs_by_anchor[anchor]
                    if r + 7 + struct.unpack_from("<i", image, r + 3)[0] == string_rva]
            print(f"  string_rva=0x{string_rva:X} lea_refs={len(refs)}")
        for fn in sorted(functions_by_anchor[anchor]):
            calls = calls_by_function[fn]
            print(f"  candidate_rva=0x{fn:X} direct_calls={len(calls)} "
                  f"call_rvas={[hex(x) for x in calls[:12]]}")
            print(disassemble(md, image, base, fn))
            for call in calls[:4]:
                print(f"  call context rva=0x{call:X}:")
                print(call_context(md, image, base, starts, call))


if __name__ == "__main__":
    parser = argparse.ArgumentParser()
    parser.add_argument("--pid", type=int, required=True)
    parser.add_argument("--dump-rva", action="append", default=[], type=lambda s: int(s, 0))
    parser.add_argument(
        "anchors",
        nargs="*",
        default=[
            "System::getCoreSystem",
            "System::getBankCount",
            "System::getBankList",
            "System::getEvent",
            "EventDescription::getInstanceCount",
            "EventDescription::getInstanceList",
            "EventDescription::is3D",
            "EventDescription::isStream",
            "EventDescription::getPath",
            "EventInstance::getDescription",
            "EventInstance::getPlaybackState",
            "EventInstance::getChannelGroup",
            "Channel::getMode",
            "Sound::getMode",
            "ChannelControl::get3DAttributes",
        ],
    )
    args = parser.parse_args()
    run(args.pid, args.anchors, args.dump_rva)
