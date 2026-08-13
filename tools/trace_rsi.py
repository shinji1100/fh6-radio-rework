import struct

p = "D:/Forza Horizon 6/forzahorizon6.exe"
d = open(p, "rb").read()
e_lfanew = struct.unpack_from("<I", d, 0x3C)[0]
fh = e_lfanew + 4
nsec = struct.unpack_from("<H", d, fh + 2)[0]
soh = struct.unpack_from("<H", d, fh + 16)[0]
opt = fh + 20
soi = struct.unpack_from("<I", d, opt + 56)[0]
sec_off = opt + soh
sections = {}
for i in range(nsec):
    so = sec_off + i * 40
    nm = d[so:so+8].rstrip(b'\x00').decode('latin1')
    vsize = struct.unpack_from("<I", d, so+8)[0]
    vaddr = struct.unpack_from("<I", d, so+12)[0]
    rawptr = struct.unpack_from("<I", d, so+20)[0]
    rawsize = struct.unpack_from("<I", d, so+16)[0]
    sections[nm] = (vaddr, vsize, rawptr, rawsize)

def rva_to_off(rva):
    for nm, (va, vsz, rp, rs) in sections.items():
        if va <= rva < va + vsz:
            return rp + (rva - va)
    return None

call_rva = 0x31CF7CF
call_off = rva_to_off(call_rva)

# 从 call_off-15 (lea r14, [rsi+0x4C918] 起点) 往前扫描 128 字节，
# 找"写 rsi"的指令（字节模式）。
print("=== 扫描 call 前 128 字节找写 rsi 的指令 ===")
scan_start = call_off - 128
for off in range(scan_start, call_off - 15):
    b = d[off:off+16]
    # mov rsi, [rip+disp32]: 48 8B 35 xx xx xx xx
    if b[0] == 0x48 and b[1] == 0x8B and b[2] == 0x35:
        disp = struct.unpack_from("<i", d, off+3)[0]
        rva = call_rva - (call_off - off)
        print("  0x%X (call-%d): mov rsi, [rip+0x%X]" % (rva, call_off - off, disp))
    # lea rsi, [rip+disp32]: 48 8D 35 xx xx xx xx
    elif b[0] == 0x48 and b[1] == 0x8D and b[2] == 0x35:
        disp = struct.unpack_from("<i", d, off+3)[0]
        rva = call_rva - (call_off - off)
        print("  0x%X (call-%d): lea rsi, [rip+0x%X]" % (rva, call_off - off, disp))
    # mov rsi, [reg+disp8/32]: 48 8B 70..7F (mod=01/10, reg=110 rsi)
    elif b[0] == 0x48 and b[1] == 0x8B and (b[2] & 0xF8) == 0x70:
        mod = b[2] >> 6
        rm = b[2] & 7
        regs = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"]
        if mod == 1:
            disp = struct.unpack_from("<b", d, off+3)[0]
            rva = call_rva - (call_off - off)
            print("  0x%X (call-%d): mov rsi, [%s+%d]" % (rva, call_off - off, regs[rm], disp))
        elif mod == 2:
            disp = struct.unpack_from("<i", d, off+3)[0]
            rva = call_rva - (call_off - off)
            print("  0x%X (call-%d): mov rsi, [%s+0x%X]" % (rva, call_off - off, regs[rm], disp))
    # mov rsi, reg: 48 8B F0..F7 (mod=11, reg=110 rsi)
    elif b[0] == 0x48 and b[1] == 0x8B and (b[2] & 0xF8) == 0xF0:
        rm = b[2] & 7
        regs = ["rax","rcx","rdx","rbx","rsp","rbp","rsi","rdi"]
        rva = call_rva - (call_off - off)
        print("  0x%X (call-%d): mov rsi, %s" % (rva, call_off - off, regs[rm]))

print("\n=== call 前 32 字节原始 hex（对齐检查） ===")
raw = d[call_off-32:call_off]
print(" ".join("%02X" % x for x in raw))
