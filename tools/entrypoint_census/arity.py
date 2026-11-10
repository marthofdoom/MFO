import sys,json,re,pefile,capstone
from capstone import x86_const as X
pe=pefile.PE(sys.argv[1],fast_load=True); img=bytes(pe.get_memory_mapped_image())
d=json.load(open(sys.argv[2])); want=set(int(x) for x in sys.argv[3].split(','))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
for s in d['sites']:
    if s['ep'] not in want: continue
    site=int(s['site'],16); best=None
    for back in range(0x30,0x80):
        ins=list(md.disasm(img[site-back:site+5],site-back))
        if ins and ins[-1].address==site: best=ins; break
    slots={}
    for i in reversed(best[:-1]):
        if i.mnemonic in ('call','ret','jmp'): break
        if not i.operands: continue
        o=i.operands[0]
        if o.type==X.X86_OP_REG and md.reg_name(o.reg) in ('r8','r8d','r9','r9d') and i.mnemonic in ('mov','lea','xor'):
            slots.setdefault(md.reg_name(o.reg)[:2], '%s %s'%(i.mnemonic,i.op_str))
        if o.type==X.X86_OP_MEM and o.mem.base==X.X86_REG_RSP and o.mem.disp in (0x20,0x28,0x30,0x38) and i.mnemonic=='mov':
            slots.setdefault('rsp+%x'%o.mem.disp, i.op_str.split(',',1)[1].strip())
    print(s['ep'], s['site'], ' | '.join('%s=%s'%(k,slots[k]) for k in sorted(slots)))
