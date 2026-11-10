import sys,json,struct,pefile,capstone,re
sys.path.insert(0,'/mnt/gaming/modlists/Projects/Linux-Native-Tools/tools/steamstub-rtti'); import addrlib
from capstone import x86_const as X
pe=pefile.PE(sys.argv[1],fast_load=True); img=bytes(pe.get_memory_mapped_image())
db=addrlib.DB(sys.argv[2]); d=json.load(open(sys.argv[3]))
md=capstone.Cs(capstone.CS_ARCH_X86,capstone.CS_MODE_64); md.detail=True
names={int(m.group(2)):m.group(1) for l in open('/mnt/gaming/modlists/Projects/_commonlib/mit-3.7-fork/include/RE/B/BGSEntryPoint.h') for m in [re.search(r'(k\w+) = (\d+),',l)] if m}
def cstr(a):
    s=img[a:a+80].split(b'\0')[0]
    return s.decode('latin1') if len(s)>=4 and all(32<=c<127 for c in s) else None
for s in sorted(d['sites'],key=lambda s:s['ep'] if s['ep'] is not None else 99):
    if not s['owner_is_player'] and not s['ps_cmp']: continue
    fs=int(s['func'],16); fe=int(s['fend'],16) if s['fend'] else fs+0x400
    c=db.containing(fs); strs=[]
    for ins in md.disasm(img[fs:fe],fs):
        for o in ins.operands:
            if o.type==X.X86_OP_MEM and o.mem.base==X.X86_REG_RIP:
                t=cstr(ins.address+ins.size+o.mem.disp)
                if t and t not in strs: strs.append(t)
    print(s['ep'],names.get(s['ep']),s['site'],'fn',s['func'],'id',c[0] if c else None,'+%x'%(fs-c[1]) if c else '', strs[:4])
