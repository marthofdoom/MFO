#!/usr/bin/env python3
# Offline census of BGSEntryPoint::HandleEntryPoint call sites (read-only RE).
# usage: ep_census.py <exe> <addrlib.bin> <hep_id> <player_singleton_id> <pc_vtbl_id> <char_vtbl_id> [selfcheck_id selfcheck_rva]
import sys, struct, bisect, json
sys.path.insert(0, '/mnt/gaming/modlists/Projects/Linux-Native-Tools/tools/steamstub-rtti')
import pefile, capstone
from capstone import x86_const as X
import addrlib

exe, lib, hep_id, ps_id, pcv_id, chv_id = sys.argv[1:7]
db = addrlib.load(lib)
id2 = db["map"]
def R(i): return int(i,16) if str(i).startswith('0x') else id2[int(i)]
if len(sys.argv) > 8:
    assert R(sys.argv[7]) == int(sys.argv[8], 16), 'addrlib selfcheck FAILED'

pe = pefile.PE(exe, fast_load=True)
img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]
tva, tsz = text.VirtualAddress, text.Misc_VirtualSize
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted({(e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION})
starts = [f[0] for f in funcs]
def func_of(rva):
    i = bisect.bisect_right(starts, rva) - 1
    if i >= 0 and funcs[i][0] <= rva < funcs[i][1]: return funcs[i]
    return None

HEP = R(hep_id)
PS = R(ps_id)
# follow thunk
def follow(rva):
    b = img[rva:rva+5]
    if b[0] == 0xE9: return rva + 5 + struct.unpack('<i', b[1:5])[0]
    return rva
targets = {HEP, follow(HEP)}
# find E9 thunks to targets (5-byte jmp at a function start)
code = img[tva:tva+tsz]
for (s, e) in funcs:
    if e - s <= 8 and img[s] == 0xE9 and follow(s) in targets: targets.add(s)
sites = []
for i in range(len(code) - 5):
    op = code[i]
    if op in (0xE8, 0xE9):
        dst = tva + i + 5 + struct.unpack_from('<i', code, i + 1)[0]
        if dst in targets:
            sites.append((tva + i, 'call' if op == 0xE8 else 'jmp'))

# vtables
def vtbl_slots(vrva, n=0x140):
    out = []
    for k in range(n):
        p = struct.unpack_from('<Q', img, vrva + 8*k)[0] - pe.OPTIONAL_HEADER.ImageBase
        if not (tva <= p < tva + tsz): break
        out.append(follow(p))
    return out
pcv = vtbl_slots(R(pcv_id)) if pcv_id!='0' else []; chv = vtbl_slots(R(chv_id)) if chv_id!='0' else []
pc_only = {}
for k, f in enumerate(pcv):
    if k >= len(chv) or chv[k] != f: pc_only[f] = k

md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
base = pe.OPTIONAL_HEADER.ImageBase
def regname(r): return md.reg_name(r)
def full(r):
    n = regname(r)
    m = {'ecx':'rcx','cx':'rcx','cl':'rcx','edx':'rdx','dx':'rdx','dl':'rdx','r8d':'r8','r9d':'r9','eax':'rax','ebx':'rbx','esi':'rsi','edi':'rdi','r12d':'r12','r13d':'r13','r14d':'r14','r15d':'r15','ebp':'rbp','r10d':'r10','r11d':'r11'}
    return m.get(n, n)

res = []
for (site, kind) in sites:
    f = func_of(site)
    fs = f[0] if f else max(tva, site - 0x400)
    insns = list(md.disasm(bytes(img[fs:site+5]), fs))
    # state: reg -> description
    st = {}
    ps_cmp = False; ps_loads = []
    for ins in insns:
        if ins.address >= site: break
        # rip-relative refs to the player singleton
        for o in ins.operands:
            if o.type == X.X86_OP_MEM and o.mem.base == X.X86_REG_RIP:
                tgt = ins.address + ins.size + o.mem.disp
                if tgt == PS:
                    if ins.mnemonic == 'cmp': ps_cmp = True
                    ps_loads.append(ins.address)
        if ins.mnemonic in ('mov', 'lea', 'movzx', 'xor', 'movsxd') and len(ins.operands) == 2 and ins.operands[0].type == X.X86_OP_REG:
            d = full(ins.operands[0].reg); s = ins.operands[1]
            if ins.mnemonic == 'xor' and s.type == X.X86_OP_REG and full(s.reg) == d:
                st[d] = ('imm', 0)
            elif s.type == X.X86_OP_IMM:
                st[d] = ('imm', s.imm)
            elif s.type == X.X86_OP_MEM and s.mem.base == X.X86_REG_RIP and ins.address + ins.size + s.mem.disp == PS and ins.mnemonic == 'mov':
                st[d] = ('player', ins.address)
            elif ins.mnemonic == 'lea' and s.type == X.X86_OP_MEM and s.mem.base != X.X86_REG_RIP and s.mem.index == 0 and st.get(full(s.mem.base), (None,))[0] == 'imm':
                st[d] = ('imm', st[full(s.mem.base)][1] + s.mem.disp)
            elif s.type == X.X86_OP_REG:
                st[d] = st.get(full(s.reg), ('reg', full(s.reg)))
            else:
                st[d] = ('other', '%s %s' % (ins.mnemonic, ins.op_str))
        elif ins.mnemonic == 'call':
            for r in ('rax', 'rcx', 'rdx', 'r8', 'r9', 'r10', 'r11'):
                st[r] = ('clobbered', 'call@%x' % ins.address) if r != 'rax' else ('ret', 'call %s @%x' % (ins.op_str, ins.address))
        elif ins.operands and ins.operands[0].type == X.X86_OP_REG and ins.mnemonic not in ('cmp', 'test', 'push'):
            st[full(ins.operands[0].reg)] = ('other', '%s %s' % (ins.mnemonic, ins.op_str))
    ep = st.get('rcx'); owner = st.get('rdx')
    res.append(dict(site=hex(site), kind=kind, func=hex(fs), fend=hex(f[1]) if f else None,
                    ep=ep[1] if ep and ep[0] == 'imm' else None, ep_src=str(ep),
                    owner=str(owner), owner_is_player=bool(owner and owner[0] == 'player'),
                    ps_cmp=ps_cmp, ps_refs=len(ps_loads),
                    pc_vfunc=pc_only.get(fs)))
print(json.dumps(dict(hep=hex(HEP), targets=[hex(t) for t in targets], ps=hex(PS), nsites=len(sites), sites=res), indent=0))
