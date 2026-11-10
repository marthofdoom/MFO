#!/usr/bin/env python3
# census of Actor vfunc 0xFF HasPerkEntries (+0x7F8) / 0x100 ForEachPerkEntry (+0x800) indirect calls; edx = EP, rcx = owner
import sys, struct, bisect, json, re
sys.path.insert(0, '/mnt/gaming/modlists/Projects/Linux-Native-Tools/tools/steamstub-rtti')
import pefile, capstone, addrlib
from capstone import x86_const as X
exe, lib, ps_id = sys.argv[1:4]
id2 = addrlib.load(lib)['map']; PS = id2[int(ps_id)]
pe = pefile.PE(exe, fast_load=True); img = pe.get_memory_mapped_image()
text = [s for s in pe.sections if s.Name.rstrip(b'\0') == b'.text'][0]; tva, tsz = text.VirtualAddress, text.Misc_VirtualSize
pe.parse_data_directories(directories=[pefile.DIRECTORY_ENTRY['IMAGE_DIRECTORY_ENTRY_EXCEPTION']])
funcs = sorted({(e.struct.BeginAddress, e.struct.EndAddress) for e in pe.DIRECTORY_ENTRY_EXCEPTION}); starts = [f[0] for f in funcs]
def func_of(rva):
    i = bisect.bisect_right(starts, rva) - 1
    return funcs[i] if i >= 0 and funcs[i][0] <= rva < funcs[i][1] else None
code = bytes(img[tva:tva+tsz])
md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_64); md.detail = True
cands = set()
for disp, nm in ((0x7F8, 'HasPerkEntries'), (0x800, 'ForEachPerkEntry')):
    pat = struct.pack('<I', disp)
    for m in re.finditer(re.escape(pat), code):
        i = m.start()
        if i >= 2 and code[i-2] == 0xFF and (code[i-1] & 0xF8) == 0x90 and (code[i-1] & 7) != 4:
            cands.add((tva + i - 2, nm))
FULL = {'edx':'rdx','dl':'rdx','dx':'rdx','ecx':'rcx','eax':'rax','ebx':'rbx','esi':'rsi','edi':'rdi','r8d':'r8','r9d':'r9','ebp':'rbp','r12d':'r12','r13d':'r13','r14d':'r14','r15d':'r15'}
def full(r): n = md.reg_name(r); return FULL.get(n, n)
res = []
for site, nm in sorted(cands):
    f = func_of(site); fs = f[0] if f else site - 0x200
    st = {}; insns = list(md.disasm(code[fs-tva:site-tva+6], fs))
    if not insns or insns[-1].address != site: continue
    for ins in insns[:-1]:
        ops = ins.operands
        if ins.mnemonic in ('mov','lea','movzx','xor') and len(ops) == 2 and ops[0].type == X.X86_OP_REG:
            d = full(ops[0].reg); s = ops[1]
            if ins.mnemonic == 'xor' and s.type == X.X86_OP_REG and full(s.reg) == d: st[d] = ('imm', 0)
            elif s.type == X.X86_OP_IMM: st[d] = ('imm', s.imm)
            elif s.type == X.X86_OP_MEM and s.mem.base == X.X86_REG_RIP and ins.address+ins.size+s.mem.disp == PS: st[d] = ('player',)
            elif ins.mnemonic == 'lea' and s.type == X.X86_OP_MEM and s.mem.base not in (0, X.X86_REG_RIP) and s.mem.index == 0 and st.get(full(s.mem.base),(None,))[0] == 'imm':
                st[d] = ('imm', st[full(s.mem.base)][1] + s.mem.disp)
            elif s.type == X.X86_OP_REG: st[d] = st.get(full(s.reg), ('reg', full(s.reg)))
            else: st[d] = ('other', ins.op_str)
        elif ins.mnemonic == 'call':
            for r in ('rax','rcx','rdx','r8','r9'): st[r] = ('clob',)
        elif ops and ops[0].type == X.X86_OP_REG and ins.mnemonic not in ('cmp','test','push'):
            st[full(ops[0].reg)] = ('other', ins.mnemonic)
    ep = st.get('rdx'); own = st.get('rcx')
    res.append(dict(site=hex(site), fn=nm, func=hex(fs), ep=ep[1] if ep and ep[0]=='imm' else None, ep_src=str(ep), owner=str(own), player=bool(own and own[0]=='player')))
print(json.dumps(res, indent=0))
